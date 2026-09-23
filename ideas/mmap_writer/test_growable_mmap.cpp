// Correctness test for growable_mmap_win.h: forces several grows (small
// page_size, tiny initial reservation), checks data survives each remap,
// checks the final file size and content by reopening the file fresh
// afterward (not just trusting the still-live mapping), and separately
// checks the "reserve big, never actually grow" path (1GB reservation).
#include "growable_mmap_win.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using marketlib::serialization::GrowableMmapFile;

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

int main() {
    SYSTEM_INFO si{};
    GetSystemInfo(&si);
    const std::size_t gran = si.dwAllocationGranularity;
    std::wprintf(L"allocation granularity: %zu bytes\n", gran);

    const std::wstring path = L"C:\\Users\\prasa\\AppData\\Local\\Temp\\claude\\growable_mmap_test.bin";

    // --- Test 1: forced growth across several remaps ---------------------
    std::puts("\n--- forced growth (page_size = allocation granularity, 1 page initial) ---");
    {
        GrowableMmapFile f;
        check(f.open(path, gran, gran), "open() with tiny initial reservation");
        check(f.capacity() == gran, "capacity() == one page after open");

        // Write a distinct byte pattern spanning ~6 pages, one page-sized
        // chunk at a time via ensure_capacity, so each chunk after the
        // first forces a remap. Verify every earlier chunk is still intact
        // (through the *current* base pointer, which may have moved) after
        // each new remap -- this is the real risk with a moving base.
        constexpr int kChunks = 6;
        bool all_intact = true, all_capacity_ok = true;
        for (int c = 0; c < kChunks; ++c) {
            const std::size_t off = static_cast<std::size_t>(c) * gran;
            char* base = f.ensure_capacity(off, gran);
            if (!base) { all_capacity_ok = false; break; }
            if (f.capacity() < off + gran) { all_capacity_ok = false; }
            std::memset(base + off, 'A' + c, gran); // fill this chunk with a distinct byte

            // Re-check every earlier chunk through the (possibly new) base pointer.
            for (int prev = 0; prev < c; ++prev) {
                const std::size_t prev_off = static_cast<std::size_t>(prev) * gran;
                unsigned char expected = static_cast<unsigned char>('A' + prev);
                if (static_cast<unsigned char>(base[prev_off]) != expected ||
                    static_cast<unsigned char>(base[prev_off + gran - 1]) != expected) {
                    all_intact = false;
                }
            }
        }
        check(all_capacity_ok, "ensure_capacity grew enough on every chunk");
        check(all_intact, "every earlier chunk's data survives later remaps");
        check(f.capacity() == static_cast<std::size_t>(kChunks) * gran, "capacity() == exactly kChunks pages (page-aligned growth)");

        // Real size actually written by this test = kChunks*gran (we filled every byte of every chunk).
        const std::size_t real_size = static_cast<std::size_t>(kChunks) * gran;
        check(f.close(real_size), "close() succeeds");
    }
    const std::size_t on_disk = file_size(path);
    std::wprintf(L"  file size on disk after close: %zu\n", on_disk);
    check(on_disk == static_cast<std::size_t>(6) * gran, "on-disk size == real_size passed to close() (no leftover reservation)");

    const auto content = read_whole_file(path);
    bool content_ok = content.size() == on_disk;
    if (content_ok)
        for (std::size_t c = 0; c < 6 && content_ok; ++c)
            for (std::size_t i = 0; i < gran; ++i)
                if (content[c * gran + i] != static_cast<unsigned char>('A' + c)) { content_ok = false; break; }
    check(content_ok, "reopening the file fresh reads back exactly what was written, in order");

    // --- Test 2: truncate to a size that is NOT page-aligned --------------
    std::puts("\n--- truncate to a sub-page real size (the common case: last row doesn't fill a page) ---");
    {
        GrowableMmapFile f;
        check(f.open(path, gran, gran), "open() (reused path, CREATE_ALWAYS truncates it)");
        char* base = f.ensure_capacity(0, 100);
        check(base != nullptr, "ensure_capacity for a small write");
        std::memcpy(base, "hello, growable mmap", 21);
        check(f.close(21), "close() truncating to 21 bytes, well inside one page");
    }
    check(file_size(path) == 21, "on-disk size == 21, not rounded up to the page");
    const auto sub_page = read_whole_file(path);
    check(sub_page.size() == 21 && std::memcmp(sub_page.data(), "hello, growable mmap", 21) == 0,
          "content matches exactly at the sub-page truncation");

    // --- Test 3: a big up-front reservation that's never actually grown --
    std::puts("\n--- 1 GiB reservation, small write, no grow needed ---");
    {
        GrowableMmapFile f;
        constexpr std::size_t one_gib = std::size_t{1} << 30;
        check(f.open(path, gran, one_gib), "open() with 1 GiB initial reservation");
        check(f.capacity() >= one_gib, "capacity() >= 1 GiB immediately, no grow() call needed");
        char* base = f.ensure_capacity(0, 5);
        std::memcpy(base, "small", 5);
        check(f.close(5), "close() truncating a 1 GiB reservation down to 5 bytes");
    }
    check(file_size(path) == 5, "on-disk size == 5, NOT 1 GiB -- the reservation never touches disk beyond what's used");

    DeleteFileW(path.c_str());
    std::printf("\n%s (%d failure(s))\n", g_failures == 0 ? "ALL PASSED" : "FAILED", g_failures);
    return g_failures == 0 ? 0 : 1;
}
