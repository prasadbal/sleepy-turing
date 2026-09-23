// Windows growable memory-mapped file: reserve/create a file, map it, grow
// it in page_size-aligned steps as writes need more room, truncate to the
// real (used) size on close. The Windows counterpart to the Linux design
// (mmap/mremap/ftruncate) discussed for MmapStreamBuf.
//
// Windows has no mremap. Growing means: unmap the current view, extend the
// file (SetFilePointerEx + SetEndOfFile), close the old file-mapping object,
// create a new one sized to the new capacity, and map a fresh view of it.
// The data already written is preserved (it lives in the file, not in the
// mapping), but THE BASE POINTER CAN MOVE on every grow -- exactly the same
// caveat as Linux's MREMAP_MAYMOVE, so a caller written against the Linux
// design already has to tolerate this and needs no extra care here: never
// hold a pointer into the mapping across a call that might grow it, only an
// offset, and refetch base() afterward.
//
// A newer Windows API (VirtualAlloc2 + MapViewOfFile3, Windows 10 1803+)
// can reserve a virtual range up front and map into it without the base
// ever moving, closer to how the 1GB-reservation idea reads. Not used here
// -- since the Linux side already can't promise a stable pointer either,
// there is no cross-platform win from adding that Windows-version
// requirement, only extra complexity. Worth revisiting only if something
// downstream specifically needs a stable base pointer.
#pragma once
#include <windows.h>
#include <cstddef>
#include <cstdint>
#include <string>

namespace marketlib::serialization {

class GrowableMmapFile {
public:
    GrowableMmapFile() = default;
    ~GrowableMmapFile() { close(pos_); }

    GrowableMmapFile(const GrowableMmapFile&)            = delete;
    GrowableMmapFile& operator=(const GrowableMmapFile&) = delete;

    // page_size: growth increment: must be a multiple of the system's
    // allocation granularity (SYSTEM_INFO::dwAllocationGranularity, 64KB on
    // every current Windows -- NOT dwPageSize/4096; CreateFileMapping's
    // offset/size only need page-size alignment, but MapViewOfFile's start
    // offset must be a multiple of the allocation granularity, so page_size
    // must satisfy the stricter of the two or every remap after the first
    // can fail). Checked in open(), not silently rounded.
    // initial_size: up-front reservation, rounded up to a page_size
    // multiple; the file is created (or truncated) and mapped at this size
    // immediately, so writes within it need no grow() at all.
    [[nodiscard]] bool open(const std::wstring& path, std::size_t page_size,
                             std::size_t initial_size = std::size_t{1} << 30);

    // Ensures [pos, pos+len) is inside the mapped region, growing (and
    // possibly moving the base pointer) if not. Returns the current base
    // pointer (== base() after this call) or nullptr on failure -- check
    // last_error() then. On success, write into base()+pos yourself; this
    // class does not do the copy (kept a thin allocator, not a streambuf,
    // so it plugs into either an ostream-derived sink or a template Sink).
    [[nodiscard]] char* ensure_capacity(std::size_t pos, std::size_t len);

    [[nodiscard]] char*       base() const noexcept { return base_; }
    [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }
    [[nodiscard]] std::size_t page_size() const noexcept { return page_size_; }
    [[nodiscard]] DWORD       last_error() const noexcept { return last_error_; }

    // Truncates the file to `real_size` bytes, unmaps, closes handles.
    // Safe to call more than once (a no-op after the first). `pos_` tracks
    // the caller's own high-water write mark via ensure_capacity, so a
    // plain close() call (e.g. from the destructor) truncates to that.
    bool close(std::size_t real_size);

private:
    bool remap(std::size_t new_capacity);

    HANDLE      file_{INVALID_HANDLE_VALUE};
    HANDLE      mapping_{nullptr};
    char*       base_{nullptr};
    std::size_t capacity_{0};
    std::size_t page_size_{0};
    std::size_t pos_{0};       // high-water mark from ensure_capacity, for the destructor's close()
    DWORD       last_error_{0};
    bool        closed_{true};
};

inline bool GrowableMmapFile::open(const std::wstring& path, std::size_t page_size, std::size_t initial_size) {
    SYSTEM_INFO si{};
    GetSystemInfo(&si);
    if (page_size == 0 || page_size % si.dwAllocationGranularity != 0) {
        last_error_ = ERROR_INVALID_PARAMETER;
        return false;
    }
    page_size_ = page_size;

    file_ = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                         FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file_ == INVALID_HANDLE_VALUE) {
        last_error_ = GetLastError();
        return false;
    }
    closed_ = false;

    const std::size_t rounded = ((initial_size + page_size_ - 1) / page_size_) * page_size_;
    if (!remap(rounded)) return false;
    pos_ = 0;
    return true;
}

inline bool GrowableMmapFile::remap(std::size_t new_capacity) {
    if (base_) {
        if (!UnmapViewOfFile(base_)) { last_error_ = GetLastError(); return false; }
        base_ = nullptr;
    }
    if (mapping_) {
        CloseHandle(mapping_);
        mapping_ = nullptr;
    }

    LARGE_INTEGER size{};
    size.QuadPart = static_cast<LONGLONG>(new_capacity);
    if (!SetFilePointerEx(file_, size, nullptr, FILE_BEGIN) || !SetEndOfFile(file_)) {
        last_error_ = GetLastError();
        return false;
    }

    mapping_ = CreateFileMappingW(file_, nullptr, PAGE_READWRITE, size.HighPart, size.LowPart, nullptr);
    if (!mapping_) {
        last_error_ = GetLastError();
        return false;
    }
    base_ = static_cast<char*>(MapViewOfFile(mapping_, FILE_MAP_ALL_ACCESS, 0, 0, new_capacity));
    if (!base_) {
        last_error_ = GetLastError();
        CloseHandle(mapping_);
        mapping_ = nullptr;
        return false;
    }
    capacity_ = new_capacity;
    return true;
}

inline char* GrowableMmapFile::ensure_capacity(std::size_t pos, std::size_t len) {
    if (pos > pos_) pos_ = pos; // track high-water mark regardless of outcome below
    const std::size_t need = pos + len;
    if (need <= capacity_) return base_;

    const std::size_t new_capacity = ((need + page_size_ - 1) / page_size_) * page_size_;
    if (!remap(new_capacity)) return nullptr;
    return base_;
}

inline bool GrowableMmapFile::close(std::size_t real_size) {
    if (closed_) return true;
    closed_ = true;

    bool ok = true;
    if (base_) {
        if (!UnmapViewOfFile(base_)) { last_error_ = GetLastError(); ok = false; }
        base_ = nullptr;
    }
    if (mapping_) {
        CloseHandle(mapping_);
        mapping_ = nullptr;
    }
    if (file_ != INVALID_HANDLE_VALUE) {
        LARGE_INTEGER size{};
        size.QuadPart = static_cast<LONGLONG>(real_size);
        if (!SetFilePointerEx(file_, size, nullptr, FILE_BEGIN) || !SetEndOfFile(file_)) {
            last_error_ = GetLastError();
            ok = false;
        }
        CloseHandle(file_);
        file_ = INVALID_HANDLE_VALUE;
    }
    capacity_ = 0;
    return ok;
}

} // namespace marketlib::serialization
