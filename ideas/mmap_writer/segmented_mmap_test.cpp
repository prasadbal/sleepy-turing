// Correctness test for the sliding-window segmented_mmap.h: at most one
// segment mapped at a time, older segments unmapped immediately once the
// write cursor moves past them.
//
// Two things verified here that no prior test in this session checked:
//   1. A segment's memory is genuinely UNMAPPED once slid past, not just
//      abandoned. Checked via debug_on_unmap, which fires at the instant
//      of UnmapViewOfFile -- checking any LATER is ambiguous, because the
//      OS is free to (and does, observed directly below) immediately hand
//      that exact freed address to the very next MapViewOfFile.
//   2. Data written into a segment that gets unmapped WITHOUT an explicit
//      flush is still correctly on disk after close() -- read back with a
//      completely fresh file handle, not the live mapping.
#include "segmented_mmap.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using marketlib::serialization::SegmentedMmapFile;

static int g_failures = 0;
static void check(bool ok, const char* what) {
    std::wprintf(L"  [%s] %hs\n", ok ? L"ok" : L"FAIL", what);
    if (!ok) ++g_failures;
}
static std::size_t file_size(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA d{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &d)) return SIZE_MAX;
    return (static_cast<std::size_t>(d.nFileSizeHigh) << 32) | d.nFileSizeLow;
}
static std::vector<unsigned char> read_whole_file(const std::wstring& path) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return {};
    LARGE_INTEGER sz{};
    GetFileSizeEx(h, &sz);
    std::vector<unsigned char> buf(static_cast<std::size_t>(sz.QuadPart));
    DWORD read = 0;
    ReadFile(h, buf.data(), static_cast<DWORD>(buf.size()), &read, nullptr);
    CloseHandle(h);
    buf.resize(read);
    return buf;
}
// true if writing to `p` traps (genuinely unmapped); false if it silently succeeds.
static bool write_traps(volatile char* p) {
    __try {
        *p = 'X';
        return false;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return true;
    }
}

int main() {
    SYSTEM_INFO si{};
    GetSystemInfo(&si);
    const std::size_t N = si.dwAllocationGranularity;
    const std::wstring path = L"C:\\Users\\prasa\\AppData\\Local\\Temp\\claude\\segmented_mmap_test.bin";

    std::puts("--- at most one segment mapped: unmapped segments trap on write, checked at the safe instant ---");
    int unmap_calls = 0;
    bool all_unmaps_trapped = true;
    bool address_reuse_observed = false;
    char* last_new_base = nullptr;
    {
        SegmentedMmapFile f;
        f.debug_on_unmap = [&](char* old_base, std::size_t old_size) {
            ++unmap_calls;
            (void)old_size;
            if (!write_traps(old_base)) all_unmaps_trapped = false; // checked RIGHT NOW: nothing has reused it yet
        };
        check(f.open(path, N), "open() maps the initial [0, N) segment");
        char* seg0_base = f.debug_current_base();
        check(seg0_base != nullptr, "segment 0 has a real base pointer");
        check(!write_traps(seg0_base), "segment 0 is writable while it's the current segment");

        std::vector<char> chunk(N, 'A');
        check(f.write(chunk.data(), chunk.size()), "fill segment 0 exactly");
        check(f.debug_current_base() == seg0_base, "still segment 0 -- exactly filling it doesn't slide yet");
        check(unmap_calls == 0, "no unmap yet -- segment 0 hasn't been slid past");

        check(f.write("X", 1), "one more byte forces a slide to segment 1");
        check(unmap_calls == 1, "exactly one unmap happened for that one slide");
        char* seg1_base = f.debug_current_base();
        if (seg1_base == seg0_base) address_reuse_observed = true; // legitimate Windows behavior, not a bug
        last_new_base = seg1_base;

        check(f.write(chunk.data(), chunk.size() - 1), "fill the rest of segment 1");
        check(f.write("Y", 1), "one more byte forces a slide to segment 2");
        check(unmap_calls == 2, "a second unmap happened for the second slide");

        check(f.close(f.position()), "close()");
        check(unmap_calls == 3, "close() unmaps the last live segment too (3 total: seg0, seg1, seg2)");
    }
    check(all_unmaps_trapped,
          "every segment traps on write at the exact instant it's unmapped -- genuinely freed, not just abandoned");
    std::wprintf(L"  (address reuse for the next segment: %hs -- expected Windows behavior either way, "
                 L"not itself a pass/fail signal)\n", address_reuse_observed ? "observed" : "not observed");
    (void)last_new_base;

    const std::size_t on_disk = file_size(path);
    // N 'A' + 1 'X' + (N-1) 'A' + 1 'Y' = 2N + 1 total.
    check(on_disk == 2 * N + 1, "on-disk size == 2N + 1 (N bytes 'A', 1 'X', N-1 bytes 'A', 1 'Y')");
    const auto content = read_whole_file(path);
    bool content_ok = content.size() == on_disk;
    if (content_ok) {
        for (std::size_t i = 0; i < N && content_ok; ++i) content_ok = (content[i] == 'A');
        content_ok = content_ok && content[N] == 'X';
        for (std::size_t i = N + 1; i < 2 * N && content_ok; ++i) content_ok = (content[i] == 'A');
        content_ok = content_ok && content[2 * N] == 'Y';
    }
    check(content_ok, "every byte, including from segments unmapped WITHOUT an explicit flush, "
                       "is correctly on disk when read back with a completely fresh handle");

    std::puts("\n--- a single write spanning several segments' worth of bytes in one call ---");
    {
        SegmentedMmapFile f;
        check(f.open(path, N), "open() (reused path, CREATE_ALWAYS truncates it)");
        std::vector<char> big(N * 3 + 100);
        for (std::size_t i = 0; i < big.size(); ++i) big[i] = static_cast<char>('a' + (i % 26));
        check(f.write(big.data(), big.size()), "one write() call spanning several segments' worth of bytes");
        check(f.close(f.position()), "close()");
        const auto c2 = read_whole_file(path);
        bool ok = c2.size() == big.size();
        if (ok) for (std::size_t i = 0; i < big.size() && ok; ++i) ok = (c2[i] == static_cast<unsigned char>(big[i]));
        check(ok, "content round-trips exactly across the whole spanning write");
    }

    std::puts("\n--- sub-N truncation, like a real report's last partial row ---");
    {
        SegmentedMmapFile f;
        check(f.open(path, N), "open()");
        check(f.write("hello, sliding window mmap", 27), "small write, well inside the first segment");
        check(f.close(27), "close() truncating to 27 bytes");
    }
    check(file_size(path) == 27, "on-disk size == 27, not rounded up to N");

    DeleteFileW(path.c_str());
    std::printf("\n%s (%d failure(s))\n", g_failures == 0 ? "ALL PASSED" : "FAILED", g_failures);
    return g_failures == 0 ? 0 : 1;
}
