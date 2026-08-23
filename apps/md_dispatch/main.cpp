// Wiring demo: Onload multicast receiver -> symbol-affinity ThreadPool ->
// per-worker OrderBook -> shared-memory snapshot publication.
//
// This is the shape a real feed handler takes, minus a real protocol decoder
// (proto/itch, proto/mdp3 are TBD — see project layout notes). The receive
// thread here only peeks a symbol id; everything after that is the same
// dispatch/book/publish pipeline a real ITCH or MDP3 decode would feed into.
#include <hashmap/hashmap.h>
#include <logging/macros.h>
#include <onload/log.h>
#include <onload/onload_receiver.h>
#include <orderbook/order_book.h>
#include <orderbook/shared_book_table.h>
#include <threadpool/threadpool.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <memory>
#include <stop_token>
#include <vector>

using namespace marketlib;

namespace {

constexpr std::size_t kNumBookWorkers = 4;
constexpr std::size_t kMaxSymbols     = 1024;

struct MarketDataTask {
    static constexpr std::size_t kMaxPayload = 64;
    std::uint32_t symbol_id{};
    std::uint64_t sequence{};
    std::uint16_t length{};
    std::array<std::byte, kMaxPayload> payload{};
};

using Pool = threadpool::ThreadPool<MarketDataTask, /*QueueCapacity=*/4096, kNumBookWorkers>;

// One of these per worker — private book storage, never shared across
// threads. A worker only ever sees symbols hash-partitioned to it, so no
// synchronization is needed here; see order_book.h for why.
struct WorkerCtx {
    marketlib::HashMap<std::uint32_t, orderbook::OrderBook<32, 8192>> books;
    orderbook::SharedBookTable<kMaxSymbols>*                          shared_table{nullptr};
};

// Demo wire format layered on WireHeader's payload: [msg_type:1][order_id:8]
// then, for 'A': [side:1][price:8][qty:8]; for 'E': [qty:8]. A real system
// decodes ITCH/OUCH/MDP3 here instead of this ad hoc layout.
void process_market_data(MarketDataTask& task, void* ctx_raw) noexcept {
    auto& ctx = *static_cast<WorkerCtx*>(ctx_raw);
    auto [it, inserted] = ctx.books.try_emplace(task.symbol_id, task.symbol_id);
    auto& book = it.value();

    const auto* p = task.payload.data();
    std::uint8_t msg_type{};
    std::memcpy(&msg_type, p, sizeof(msg_type));
    p += sizeof(msg_type);

    orderbook::OrderId order_id{};
    std::memcpy(&order_id, p, sizeof(order_id));
    p += sizeof(order_id);

    switch (msg_type) {
    case 'A': {
        std::uint8_t side_byte{};
        std::memcpy(&side_byte, p, sizeof(side_byte)); p += sizeof(side_byte);
        orderbook::Price price{};
        std::memcpy(&price, p, sizeof(price)); p += sizeof(price);
        orderbook::Qty qty{};
        std::memcpy(&qty, p, sizeof(qty));
        book.on_add(order_id, side_byte ? orderbook::Side::Ask : orderbook::Side::Bid, price, qty);
        break;
    }
    case 'X':
        book.on_cancel(order_id);
        break;
    case 'E': {
        orderbook::Qty exec_qty{};
        std::memcpy(&exec_qty, p, sizeof(exec_qty));
        book.on_execute(order_id, exec_qty);
        break;
    }
    default:
        break;
    }

    if (ctx.shared_table) {
        const auto slot = task.symbol_id % ctx.shared_table->capacity();
        ctx.shared_table->publish(slot, book.snapshot<5>());
    }
}

// Bridges the NIC-facing receiver thread into the pool: peek-parse only,
// then submit_by_key so every message for a symbol lands on the same
// worker, in arrival order, every time.
struct DispatchCtx { Pool* pool; };

void on_packet(std::uint32_t symbol_id, std::uint64_t sequence,
               std::span<const std::byte> payload, void* ctx_raw) noexcept {
    auto& ctx = *static_cast<DispatchCtx*>(ctx_raw);

    MarketDataTask task{};
    task.symbol_id = symbol_id;
    task.sequence  = sequence;
    task.length    = static_cast<std::uint16_t>(
        std::min(payload.size(), MarketDataTask::kMaxPayload));
    std::memcpy(task.payload.data(), payload.data(), task.length);

    if (!ctx.pool->submit_by_key(symbol_id, task)) [[unlikely]]
        LOG_WARN(onload::log, "book worker queue full, dropping symbol={} seq={}",
                  symbol_id, sequence);
}

} // namespace

int main() {
    auto& log = logging::Logger::get("marketlib.md_dispatch");

    orderbook::SharedBookTable<kMaxSymbols> shared_table;
    if (auto r = shared_table.create("/marketlib_books"); !r) {
        LOG_ERROR(log, "shared_table.create failed: {}", r.error());
        return 1;
    }

    // Per-worker contexts — one private book map each, never shared.
    std::vector<std::unique_ptr<WorkerCtx>> ctxs;
    std::vector<void*>                      ctx_ptrs;
    for (std::size_t i = 0; i < kNumBookWorkers; ++i) {
        ctxs.push_back(std::make_unique<WorkerCtx>());
        ctxs.back()->shared_table = &shared_table;
        ctx_ptrs.push_back(ctxs.back().get());
    }

    Pool pool;
    // One core per book worker (adjust to the box's isolated/isolcpus set).
    const std::array<int, kNumBookWorkers> cpu_ids{2, 3, 4, 5};
    pool.spawn_group("book-builders", process_market_data, ctx_ptrs, cpu_ids);
    pool.start();

    DispatchCtx dispatch_ctx{.pool = &pool};

    onload::OnloadMulticastReceiver receiver(onload::Config{
        .bind_iface  = "eth2",
        .mcast_group = "233.54.12.1",
        .port        = 18001,
        .stack_name  = "md_rx_a",
        .spin        = true,
    });
    if (auto r = receiver.open(); !r) {
        LOG_ERROR(log, "receiver.open failed: {}", r.error());
        return 1;
    }

    LOG_INFO(log, "md_dispatch running: {} book workers, shared table capacity={}",
              kNumBookWorkers, shared_table.capacity());

    std::stop_source stop_src;
    receiver.run(stop_src.get_token(), on_packet, &dispatch_ctx);

    pool.stop();
    return 0;
}
