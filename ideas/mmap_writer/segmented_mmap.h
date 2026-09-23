// Sliding-window growable memory-mapped file for a write-only, ever-growing
// file: at most ONE segment is ever mapped at a time. When the write cursor
// moves past the current segment, it is unmapped immediately (this process
// never reads it again, so there is no reason to keep it resident) and a
// new segment is mapped at the new offset -- memory usage stays bounded by
// view_size (N), independent of how large the file has grown.
//
// This replaces an earlier version of this file that kept every segment
// mapped until close() -- fine for random access, wasteful for a
// write-only stream, which is the actual use case.
//
// Growth sequence, per transition:
//   1. UnmapViewOfFile the current (now fully-written) segment
//   2. Extend the file to cover the new segment
//   3. MapViewOfFile the new segment at the new offset
// Unmapping before extending means this never needs "does extending the
// file succeed while some OTHER view of it is still mapped" -- by the time
// the file is extended, nothing is mapped at all. Verified (not assumed)
// that data written into a segment and then unmapped, without ever being
// explicitly flushed, is still correctly on disk after close() and a
// completely fresh file read -- see segmented_mmap_test.cpp.
//
// Even with that ordering, extending the file (set_file_size(), below) can
// still spuriously fail with ERROR_USER_MAPPED_FILE: the kernel's memory
// manager can lag tearing a section down internally even after
// UnmapViewOfFile + CloseHandle have both already returned. set_file_size()
// retries briefly (bounded, not infinite) on that specific error only.
#pragma once
#ifndef NOMINMAX
#define NOMINMAX // otherwise windows.h's max()/min() macros mangle std::max/std::min below
#endif
#include <windows.h>
#include <algorithm>
#include <cstddef>
#include <cstring>
#include <functional>
#include <string>

namespace marketlib::serialization {

class SegmentedMmapFile {
public:
    ~SegmentedMmapFile() { close(pos_); }

    // view_size (N): the size of the one segment kept mapped at a time.
    // Must be a multiple of the system allocation granularity (the same
    // requirement MapViewOfFile itself enforces on the offset every
    // segment after the first is mapped at).
    [[nodiscard]] bool open(const std::wstring& path, std::size_t view_size) {
        SYSTEM_INFO si{};
        GetSystemInfo(&si);
        if (view_size == 0 || view_size % si.dwAllocationGranularity != 0) {
            last_error_ = ERROR_INVALID_PARAMETER;
            return false;
        }
        view_size_ = view_size;
        file_ = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file_ == INVALID_HANDLE_VALUE) { last_error_ = GetLastError(); return false; }
        closed_ = false;
        pos_ = 0;
        seg_offset_ = 0;
        seg_size_ = 0;
        base_ = nullptr;
        return map_segment(0, view_size_); // the initial [0, N) segment
    }

    // Writes `len` bytes, sliding the mapped window forward (unmapping the
    // old segment, extending the file, mapping the new one) as needed.
    // Returns false on failure -- check last_error(). At most one segment
    // is ever mapped at once, regardless of how many grows this call causes.
    [[nodiscard]] bool write(const void* data, std::size_t len) {
        const char* src = static_cast<const char*>(data);
        std::size_t remaining = len;
        while (remaining > 0) {
            if (pos_ >= seg_offset_ + seg_size_) {
                const std::size_t need = std::max(view_size_, remaining);
                const std::size_t size = ((need + view_size_ - 1) / view_size_) * view_size_;
                if (!slide_to(pos_, size)) return false;
            }
            const std::size_t offset_in_seg = pos_ - seg_offset_;
            const std::size_t avail = seg_size_ - offset_in_seg;
            const std::size_t n = std::min(avail, remaining);
            std::memcpy(base_ + offset_in_seg, src, n);
            src += n; pos_ += n; remaining -= n;
        }
        return true;
    }

    [[nodiscard]] std::size_t position() const noexcept { return pos_; }
    [[nodiscard]] DWORD last_error() const noexcept { return last_error_; }

    // For tests only: the currently-mapped segment's base pointer and file
    // offset, to directly verify the sliding-window claim (e.g. capture a
    // segment's base, force a slide past it, then confirm that pointer is
    // genuinely unmapped -- not merely unused).
    [[nodiscard]] char*       debug_current_base() const noexcept { return base_; }
    [[nodiscard]] std::size_t debug_segment_offset() const noexcept { return seg_offset_; }

    // For tests only: called with (old_base, old_size) at the exact moment a
    // segment is unmapped, before anything else can reuse that address
    // range -- the OS is free to (and does) immediately hand a freed VA
    // range to the very next mapping, so checking "is this pointer still
    // valid" any time AFTER this call is ambiguous; checking from inside
    // this hook is not.
    std::function<void(char*, std::size_t)> debug_on_unmap;

    bool close(std::size_t real_size) {
        if (closed_) return true;
        closed_ = true;
        bool ok = true;
        if (base_) {
            char* old_base = base_;
            const std::size_t old_size = seg_size_;
            if (!UnmapViewOfFile(base_)) { last_error_ = GetLastError(); ok = false; }
            base_ = nullptr;
            if (debug_on_unmap) debug_on_unmap(old_base, old_size);
        }
        if (file_ != INVALID_HANDLE_VALUE) {
            if (!set_file_size(real_size)) ok = false;
            CloseHandle(file_);
            file_ = INVALID_HANDLE_VALUE;
        }
        return ok;
    }

private:
    // Unmaps the current segment (if any), extends the file to cover
    // [offset, offset+size), and maps exactly that as the new current
    // segment. By unmapping BEFORE extending, the file is never resized
    // while anything is mapped at all -- no reliance on "extend while some
    // other view is still mapped" behavior.
    bool slide_to(std::size_t offset, std::size_t size) {
        if (base_) {
            char* old_base = base_;
            const std::size_t old_size = seg_size_;
            if (!UnmapViewOfFile(base_)) { last_error_ = GetLastError(); return false; }
            base_ = nullptr;
            if (debug_on_unmap) debug_on_unmap(old_base, old_size); // see the member comment: only safe here
        }
        return map_segment(offset, size);
    }

    // Sets the file's length, retrying briefly on ERROR_USER_MAPPED_FILE:
    // the kernel's memory manager can lag tearing down a section even
    // after UnmapViewOfFile + CloseHandle have both already returned
    // (slide_to() always does both before calling here), which can make
    // SetEndOfFile spuriously fail right after a slide. A real, observed
    // transient race, not hardening against something theoretical --
    // bounded retries with a short sleep, not an infinite loop, and any
    // OTHER error fails immediately (not retryable).
    bool set_file_size(std::size_t new_size) {
        LARGE_INTEGER sz{};
        sz.QuadPart = static_cast<LONGLONG>(new_size);
        if (!SetFilePointerEx(file_, sz, nullptr, FILE_BEGIN)) {
            last_error_ = GetLastError();
            return false; // moving the cursor isn't part of the retryable race -- fail immediately
        }
        constexpr int kMaxAttempts = 5;
        constexpr DWORD kRetryDelayMs = 2;
        for (int attempt = 0; attempt < kMaxAttempts; ++attempt) {
            if (SetEndOfFile(file_)) return true;
            last_error_ = GetLastError();
            if (last_error_ != ERROR_USER_MAPPED_FILE) return false; // different, non-retryable failure
            if (attempt + 1 < kMaxAttempts) Sleep(kRetryDelayMs);
        }
        return false; // exhausted retries; last_error() is ERROR_USER_MAPPED_FILE
    }

    bool map_segment(std::size_t offset, std::size_t size) {
        const std::size_t new_total = offset + size;
        if (!set_file_size(new_total)) return false;
        HANDLE mapping = CreateFileMappingW(file_, nullptr, PAGE_READWRITE,
                                             static_cast<DWORD>(new_total >> 32), static_cast<DWORD>(new_total & 0xFFFFFFFFu),
                                             nullptr);
        if (!mapping) { last_error_ = GetLastError(); return false; }
        base_ = static_cast<char*>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS,
                                                  static_cast<DWORD>(offset >> 32), static_cast<DWORD>(offset & 0xFFFFFFFFu), size));
        // The mapping-object handle can be closed immediately: the VIEW
        // keeps the underlying section alive on its own (documented
        // CreateFileMapping/MapViewOfFile behavior) -- verified, not just
        // assumed, in segmented_mmap_test.cpp.
        CloseHandle(mapping);
        if (!base_) { last_error_ = GetLastError(); return false; }
        seg_offset_ = offset;
        seg_size_ = size;
        return true;
    }

    HANDLE      file_{INVALID_HANDLE_VALUE};
    char*       base_{nullptr};
    std::size_t seg_offset_{0}, seg_size_{0}; // the one currently-mapped segment
    std::size_t view_size_{0};
    std::size_t pos_{0};
    DWORD       last_error_{0};
    bool        closed_{true};
};

} // namespace marketlib::serialization
