#include <onload/onload_receiver.h>
#include <onload/log.h>
#include <logging/macros.h>
#include <cstring>

#if defined(__linux__)
#  include <arpa/inet.h>
#  include <net/if.h>
#  include <netinet/in.h>
#  include <sys/socket.h>
#  include <unistd.h>
#endif

#if defined(MARKETLIB_WITH_ONLOAD)
#  include <onload/extensions.h>
#endif

#if defined(__x86_64__) || defined(_M_X64)
#  include <immintrin.h>
#  define MARKETLIB_ONLOAD_PAUSE() _mm_pause()
#else
#  define MARKETLIB_ONLOAD_PAUSE() ((void)0)
#endif

namespace marketlib::onload {

OnloadMulticastReceiver::~OnloadMulticastReceiver() { close(); }

std::expected<void, std::string> OnloadMulticastReceiver::open() noexcept {
#if !defined(__linux__)
    return std::unexpected("OnloadMulticastReceiver requires Linux");
#else
    fd_ = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd_ < 0) return std::unexpected("socket() failed");

#if defined(MARKETLIB_WITH_ONLOAD)
    // Isolate this socket's Onload stack from any other socket in the
    // process — avoids the rx thread contending on a shared poll set.
    if (!cfg_.stack_name.empty())
        onload_set_stackname(ONLOAD_ALL_THREADS, ONLOAD_SCOPE_THREAD, cfg_.stack_name.c_str());
    if (cfg_.spin)
        onload_thread_set_spin(ONLOAD_SPIN_ALL, 1);
#endif

    int reuse = 1;
    setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
#ifdef SO_REUSEPORT
    // SO_REUSEPORT is also what backs Onload's clustering (EF_CLUSTER_*):
    // multiple accelerated sockets on the same group, each pinned to its own
    // rx thread/core, sharing NIC-level fan-out instead of one thread
    // draining everything and re-dispatching.
    setsockopt(fd_, SOL_SOCKET, SO_REUSEPORT, &reuse, sizeof(reuse));
#endif

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port        = htons(cfg_.port);
    if (bind(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        close();
        return std::unexpected("bind() failed on port " + std::to_string(cfg_.port));
    }

    ip_mreqn mreq{};
    mreq.imr_multiaddr.s_addr = inet_addr(cfg_.mcast_group.c_str());
    mreq.imr_address.s_addr  = htonl(INADDR_ANY);
    if (!cfg_.bind_iface.empty())
        mreq.imr_ifindex = static_cast<int>(if_nametoindex(cfg_.bind_iface.c_str()));
    if (setsockopt(fd_, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) != 0) {
        close();
        return std::unexpected("IP_ADD_MEMBERSHIP failed for " + cfg_.mcast_group);
    }

    LOG_INFO(log, "onload receiver open: group={} port={} iface={} stack={}",
              cfg_.mcast_group, cfg_.port, cfg_.bind_iface, cfg_.stack_name);
    return {};
#endif
}

void OnloadMulticastReceiver::run(std::stop_token st, OnMessage handler, void* ctx) noexcept {
#if defined(__linux__)
    static constexpr std::size_t kMaxDatagram = 2048;
    alignas(WireHeader) unsigned char buf[kMaxDatagram];

    while (!st.stop_requested()) {
        // Onload spin (set in open()) turns this blocking recv into a
        // userspace busy-poll rather than a kernel trap-and-sleep — the recv
        // call itself is unchanged; the acceleration is transparent.
        const ssize_t n = recv(fd_, buf, sizeof(buf), 0);
        if (n < static_cast<ssize_t>(sizeof(WireHeader))) {
            if (n < 0) MARKETLIB_ONLOAD_PAUSE();
            continue;
        }

        WireHeader hdr;
        std::memcpy(&hdr, buf, sizeof(hdr));

        const auto payload_len = static_cast<std::size_t>(n) - sizeof(hdr);
        const auto* payload    = reinterpret_cast<const std::byte*>(buf + sizeof(hdr));

        // Peek-only: symbol_id + sequence, enough to hash-partition. Full
        // protocol decode is the receiving worker's job, not this thread's.
        handler(hdr.symbol_id, hdr.sequence, std::span{payload, payload_len}, ctx);
    }
#else
    (void)st; (void)handler; (void)ctx;
#endif
}

void OnloadMulticastReceiver::close() noexcept {
#if defined(__linux__)
    if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
#endif
}

} // namespace marketlib::onload
