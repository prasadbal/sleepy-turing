// Demo for the binding/ idea's simplified core: scalar bind parameters
// (optionally nullable via std::optional<U>), batch fetch with a row
// callback, and no automatic retry -- a failure comes back classified as
// ConnectionLost or QueryError, and the caller decides what to do about it.
//
// Everything this file exercises used to have a considerably larger
// version -- LOB, fixed-width strings, dates, a dynamic-collection IN-list,
// and an automatic reconnect-and-retry wrapper. All of that is still in git
// history; this rewrite deliberately starts over with a smaller, useful
// core instead of trying to keep every prior feature working at once.
//
//   1. execute() with no bind struct at all -- DDL/literal-only DML.
//   2. execute() with a bind struct -- named parameters, one of them
//      std::optional (binds SQL NULL when empty).
//   3. select_rows() with no input params, collecting into a std::vector via
//      the batch callback -- "insert into a container" is just what the
//      callback does, not a separate code path.
//   4. select_rows() with an input struct -- a WHERE clause bound by name.
//   5. select_rows() with std::optional output columns -- a NULL column
//      comes back as std::nullopt.
//   6. prefetch_rows vs. fetch_batch_size as two independent numbers: a
//      small fetch_batch_size (as you'd want if the destination were a
//      std::map, where each insertion is its own O(log n) regardless of
//      batch size) still triggers multiple callback invocations even
//      though prefetch is configured much larger.
//   7. Failure classification, and why there's no retry loop here: a query
//      error is returned immediately; a connection-lost error is also
//      returned immediately, and the caller reconnects and calls the same
//      function again itself.
//
// Builds against the mock OCI backend (binding/oci_mock.h) since there's no
// real Oracle client in this environment -- see oci_compat.h.

#include <iostream>
#include <optional>
#include <vector>

#include "binding/oci_client.h"
#include "binding/oci_connection.h"

namespace {

const char* status_name(binding::ExecStatus s) {
    switch (s) {
        case binding::ExecStatus::Success:        return "Success";
        case binding::ExecStatus::ConnectionLost: return "ConnectionLost";
        case binding::ExecStatus::QueryError:     return "QueryError";
    }
    return "?";
}

} // namespace

struct EmployeeUpdate {
    int id;
    std::optional<double> bonus_pct;
};

struct TradeFilter {
    int status;
};

struct TradeRow {
    int trade_id;
    std::optional<double> notional;
};

int main() {
    binding::OciConnection conn("orcl", "app_user", "secret");
    conn.connect();

    std::cout << "--- Demo 1: execute() with no bind struct -- DDL/literal-only DML ---\n";
    {
        auto r = binding::execute(conn, "CREATE TABLE trades (trade_id NUMBER, notional NUMBER)");
        std::cout << "result=" << status_name(r.status) << "\n\n";
    }

    std::cout << "--- Demo 2: execute() with a bind struct -- one field optional ---\n";
    {
        EmployeeUpdate withBonus{101, 2.5};
        EmployeeUpdate noBonus{102, std::nullopt};
        binding::execute(conn, "UPDATE employees SET bonus_pct = :bonus_pct WHERE id = :id", withBonus);
        binding::execute(conn, "UPDATE employees SET bonus_pct = :bonus_pct WHERE id = :id", noBonus);
        std::cout << "bind indicators recorded: " << binding::mock::g_last_bind_indicators.size()
                  << " (last call's, per field)\n\n";
    }

    std::cout << "--- Demo 3: select_rows() with no input, collecting into a std::vector ---\n";
    {
        std::vector<TradeRow> rows;
        std::function<void(const TradeRow*, std::size_t)> on_batch =
            [&](const TradeRow* batch, std::size_t count) {
                for (std::size_t i = 0; i < count; ++i) rows.push_back(batch[i]);
            };
        auto r = binding::select_rows<TradeRow>(conn, "SELECT trade_id, notional FROM trades",
                                                 /*prefetch_rows=*/100, /*fetch_batch_size=*/100, on_batch);
        std::cout << "result=" << status_name(r.status) << " rows collected=" << rows.size() << "\n";
        for (auto& row : rows) {
            std::cout << "  trade_id=" << row.trade_id << " notional="
                      << (row.notional ? std::to_string(*row.notional) : std::string("NULL")) << "\n";
        }
        std::cout << "\n";
    }

    std::cout << "--- Demo 4: select_rows() with an input struct -- WHERE bound by name ---\n";
    {
        TradeFilter filter{1};
        std::vector<TradeRow> rows;
        std::function<void(const TradeRow*, std::size_t)> on_batch =
            [&](const TradeRow* batch, std::size_t count) {
                for (std::size_t i = 0; i < count; ++i) rows.push_back(batch[i]);
            };
        auto r = binding::select_rows<TradeFilter, TradeRow>(
            conn, "SELECT trade_id, notional FROM trades WHERE status = :status", filter,
            /*prefetch_rows=*/100, /*fetch_batch_size=*/100, on_batch);
        std::cout << "result=" << status_name(r.status) << " rows collected=" << rows.size() << "\n\n";
    }

    std::cout << "--- Demo 5: std::optional output column -- a NULL comes back as nullopt ---\n";
    {
        binding::mock::set_simulate_null_last_column(true); // notional is the last column here
        std::vector<TradeRow> rows;
        std::function<void(const TradeRow*, std::size_t)> on_batch =
            [&](const TradeRow* batch, std::size_t count) {
                for (std::size_t i = 0; i < count; ++i) rows.push_back(batch[i]);
            };
        binding::select_rows<TradeRow>(conn, "SELECT trade_id, notional FROM trades",
                                        100, 100, on_batch);
        for (auto& row : rows) {
            std::cout << "  trade_id=" << row.trade_id << " notional="
                      << (row.notional ? std::to_string(*row.notional) : std::string("NULL")) << "\n";
        }
        binding::mock::set_simulate_null_last_column(false);
        std::cout << "\n";
    }

    std::cout << "--- Demo 6: prefetch_rows and fetch_batch_size are independent numbers ---\n";
    {
        int batches = 0;
        std::function<void(const TradeRow*, std::size_t)> on_batch =
            [&](const TradeRow*, std::size_t count) {
                ++batches;
                std::cout << "  batch of " << count << " rows\n";
            };
        // prefetch large (network efficiency), fetch_batch_size small (as
        // you'd want feeding a std::map -- see the file comment above).
        binding::select_rows<TradeRow>(conn, "SELECT trade_id, notional FROM trades",
                                        /*prefetch_rows=*/500, /*fetch_batch_size=*/1, on_batch);
        std::cout << "  " << batches << " callback invocations for a small fetch_batch_size,\n"
                  << "  independent of the much larger prefetch_rows\n\n";
    }

    std::cout << "--- Demo 7: failure classification -- no retry happens here ---\n";
    {
        binding::mock::set_mode(binding::mock::FailureMode::ExecErrorAlways);
        EmployeeUpdate u{999, std::nullopt};
        auto r = binding::execute(conn, "UPDATE employees SET bonus_pct = :bonus_pct WHERE id = :id", u);
        std::cout << "bad SQL / constraint violation -> " << status_name(r.status)
                  << " (never retried, whoever's asking)\n";
        binding::mock::set_mode(binding::mock::FailureMode::None);

        binding::mock::set_mode(binding::mock::FailureMode::DisconnectThenRecover, 1);
        auto r2 = binding::execute(conn, "UPDATE employees SET bonus_pct = :bonus_pct WHERE id = :id", u);
        std::cout << "session killed mid-call -> " << status_name(r2.status)
                  << " -- the caller reconnects and calls execute() again itself:\n";
        binding::mock::set_mode(binding::mock::FailureMode::None);
        conn.disconnect();
        conn.connect();
        auto r3 = binding::execute(conn, "UPDATE employees SET bonus_pct = :bonus_pct WHERE id = :id", u);
        std::cout << "  retried by hand after reconnect -> " << status_name(r3.status) << "\n";
    }

    conn.disconnect();
    return 0;
}
