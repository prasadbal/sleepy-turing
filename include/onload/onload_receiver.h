#pragma once
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <stop_token>
#include <string>

namespace marketlib::onload {

// Solarflare/AMD Onload multicast receiver — the NIC-facing edge of the
// market-data path.
//
// Onload accelerates a normal BSD socket transparently (via the onload
// wrapper/LD_PRELOAD, or an EF_CLUSTER-scoped stack): this class makes no
// Onload-specific syscalls for I/O, it's plain socket()/bind()/recv(). The
// only Onload-specific calls are extensions used to tune latency:
//   - onload_set_stackname: give this receive thread its own Onload stack,
//     so its ring buffer / poll set isn't shared (and therefore serialized)
//     with unrelated sockets elsewhere in the process.
//   - onload_thread_set_spin: put this thread's blocking calls into
//     userspace busy-poll instead of trapping into the kernel to sleep —
//     this is most of what "Onload" buys you over a stock kernel stack.
// Both are guarded behind MARKETLIB_WITH_ONLOAD (set by CMake when the
// Onload SDK/extensions header is found) and no-op otherwise, so this file
// builds and runs correctly — just without the acceleration — on a dev box
// without the Solarflare/AMD NIC and driver stack.
//
// Deliberately minimal parsing: this thread only peeks the symbol id (and
// sequence number, for gap detection upstream) out of each datagram, then
// hands the whole payload off. Full protocol decode (ITCH/OUCH/MDP3 — see
// proto/) happens on the worker thread the message is dispatched to, not
// here. The NIC-facing thread's only job is draining the socket fast enough
// that the kernel/NIC ring never backs up; anything that slows this thread
// down risks dropping packets before symbol partitioning ever happens.
struct WireHeader {
    std::uint64_t sequence;
    std::uint32_t symbol_id;
    std::uint16_t msg_type;
    std::uint16_t payload_length;
};
static_assert(sizeof(WireHeader) == 16);

struct Config {
    std::string   bind_iface;                 // e.g. "eth2" — the accelerated NIC
    std::string   mcast_group;                // e.g. "233.54.12.1"
    std::uint16_t port{0};
    std::string   stack_name;                 // Onload stack name, one per rx thread
    bool          spin{true};                 // busy-poll via onload_thread_set_spin
};

// Called once per datagram, from the receiver's own thread. Must not block —
// this should be a hash + queue push (ThreadPool::submit_by_key), nothing more.
using OnMessage = void (*)(std::uint32_t symbol_id, std::uint64_t sequence,
                            std::span<const std::byte> payload, void* ctx) noexcept;

class OnloadMulticastReceiver {
public:
    explicit OnloadMulticastReceiver(Config cfg) noexcept : cfg_(std::move(cfg)) {}
    ~OnloadMulticastReceiver();

    OnloadMulticastReceiver(const OnloadMulticastReceiver&)            = delete;
    OnloadMulticastReceiver& operator=(const OnloadMulticastReceiver&) = delete;

    // Opens the socket, tags the Onload stack, joins the multicast group.
    [[nodiscard]] std::expected<void, std::string> open() noexcept;

    // Blocks the calling thread until st.stop_requested(). Call this from a
    // thread you have already pinned to an isolated core — it busy-polls.
    void run(std::stop_token st, OnMessage handler, void* ctx) noexcept;

    void close() noexcept;

private:
    Config cfg_;
    int    fd_{-1};
};

} // namespace marketlib::onload
