#pragma once
#include <lockfree/seqlock.h>
#include <orderbook/order_book.h>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>

#if defined(__linux__)
#  include <fcntl.h>
#  include <sys/mman.h>
#  include <sys/stat.h>
#  include <unistd.h>
#endif

namespace marketlib::orderbook {

// Fixed-size table of SeqLock<Snapshot> in a POSIX shared-memory segment —
// one slot per symbol. This is the cross-process half of the publication
// path described in order_book.h: the book itself stays thread-local inside
// the process that owns it; only the compact Snapshot crosses into shm, and
// only through a seqlock, so the owning (writer) thread never blocks even if
// a reader process is slow, dead, or paged out.
//
// Symbol → slot mapping is the caller's responsibility (e.g. a HashMap built
// once at startup from the instrument universe) — this class only knows
// about slot indices, not symbols, to stay a flat trivially-mappable region.
template<std::size_t MaxSymbols, std::size_t Depth = 5>
class SharedBookTable {
public:
    using Slot = lockfree::SeqLock<Snapshot<Depth>>;

    SharedBookTable()                                   = default;
    SharedBookTable(const SharedBookTable&)              = delete;
    SharedBookTable& operator=(const SharedBookTable&)   = delete;

    ~SharedBookTable() { close(); }

    // Writer side: create (or truncate-and-recreate) the segment. Call once,
    // from the process that owns the books.
    [[nodiscard]] std::expected<void, std::string> create(std::string_view shm_name) noexcept {
#if defined(__linux__)
        name_ = std::string(shm_name);
        const int fd = shm_open(name_.c_str(), O_CREAT | O_RDWR | O_TRUNC, 0644);
        if (fd < 0) return std::unexpected("shm_open failed: " + name_);
        if (ftruncate(fd, sizeof(Table)) != 0) {
            ::close(fd);
            return std::unexpected("ftruncate failed: " + name_);
        }
        void* addr = mmap(nullptr, sizeof(Table), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        ::close(fd); // fd not needed after mmap
        if (addr == MAP_FAILED) return std::unexpected("mmap failed: " + name_);
        table_ = new (addr) Table(); // placement-new: zero-init all slots in place
        return {};
#else
        (void)shm_name;
        return std::unexpected("SharedBookTable requires Linux (shm_open/mmap)");
#endif
    }

    // Reader side: map an existing segment read-write is still required
    // because SeqLock::read() only takes a const ref but the mapping itself
    // must match the writer's layout; readers simply never call publish().
    [[nodiscard]] std::expected<void, std::string> open(std::string_view shm_name) noexcept {
#if defined(__linux__)
        name_ = std::string(shm_name);
        const int fd = shm_open(name_.c_str(), O_RDONLY, 0644);
        if (fd < 0) return std::unexpected("shm_open failed: " + name_);
        void* addr = mmap(nullptr, sizeof(Table), PROT_READ, MAP_SHARED, fd, 0);
        ::close(fd);
        if (addr == MAP_FAILED) return std::unexpected("mmap failed: " + name_);
        table_ = static_cast<Table*>(addr);
        owns_unlink_ = false;
        return {};
#else
        (void)shm_name;
        return std::unexpected("SharedBookTable requires Linux (shm_open/mmap)");
#endif
    }

    // Writer only — single writer per slot, matching the book's own
    // single-owner-thread invariant.
    void publish(std::size_t slot, const Snapshot<Depth>& snap) noexcept {
        table_->slots[slot].write(snap);
    }

    [[nodiscard]] Snapshot<Depth> read(std::size_t slot) const noexcept {
        return table_->slots[slot].read();
    }

    [[nodiscard]] bool try_read(std::size_t slot, Snapshot<Depth>& out) const noexcept {
        return table_->slots[slot].try_read(out);
    }

    static constexpr std::size_t capacity() noexcept { return MaxSymbols; }

private:
    struct Table { std::array<Slot, MaxSymbols> slots; };

    Table*      table_{nullptr};
    std::string name_;
    bool        owns_unlink_{true};

    void close() noexcept {
#if defined(__linux__)
        if (table_) {
            munmap(table_, sizeof(Table));
            table_ = nullptr;
        }
        if (owns_unlink_ && !name_.empty()) shm_unlink(name_.c_str());
#endif
    }
};

} // namespace marketlib::orderbook
