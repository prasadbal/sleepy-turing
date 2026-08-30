// Demo for the binding/ idea: one boost::pfr reflection core (reflect.h)
// shared between a config-style "flat_schema" check and an Oracle OCI
// binder/reconnect layer. Three scenarios:
//
//   1. A transient disconnect during execute() -- one reconnect, then the
//      same statement succeeds.
//   2. A plain execution error (e.g. a constraint violation) -- must NOT be
//      retried, must fail on the first attempt.
//   3. A SELECT into vector<T> -- the "vector<S> as a result set" case,
//      mirroring how a config file binds repeated sections into vector<S>.
//
// Builds against the mock OCI backend (binding/oci_mock.h) since there's no
// real Oracle client in this environment -- see oci_compat.h.

#include <chrono>
#include <iostream>
#include <vector>

#include "binding/oci_client.h"
#include "binding/oci_connection.h"
#include "binding/reflect.h"

// ---- config-style struct: pure flat_schema, no OCI involved at all -------
struct AppConfigSection {
    std::string host;
    int port;
    double timeout_seconds;
};
static_assert(binding::flat_schema<AppConfigSection>,
              "AppConfigSection must be a flat struct of bindable leaves");

// ---- OCI DML row: fields declared in the order the SQL's bind placeholders
// occur left-to-right (see oci_client.h's bind_fields for why) -----------
struct FinancialUpdate {
    double bonus_pct;      // :1  -- SET bonus = :1
    binding::OciClob notes; // :2  -- , notes = :2
    int emp_id;             // :3  -- WHERE id = :3
    double final_payout;    // :4  -- RETURNING total INTO :4
};
static_assert(binding::oci_row_schema<FinancialUpdate>);

// ---- OCI SELECT row: field order matches the SELECT list order ----------
struct TradeRow {
    int trade_id;
    double notional;
};
static_assert(binding::oci_row_schema<TradeRow>);

// ---- nullable column demo: commission is a nullable NUMBER -------------
struct EmployeeRow {
    int emp_id;
    std::optional<double> commission;
};
static_assert(binding::oci_row_schema<EmployeeRow>);

int main() {
    binding::OciConnection conn("orcl", "app_user", "secret",
                                /*max_retries=*/3, std::chrono::milliseconds(200));
    conn.connect();
    binding::OciClient client;

    std::cout << "--- Demo 1: transient disconnect mid-execute, one reconnect, then success ---\n";
    binding::mock::set_mode(binding::mock::FailureMode::DisconnectThenRecover, /*disconnect_count=*/1);
    binding::mock::g_execute_calls = 0;
    FinancialUpdate update{0.12, binding::OciClob("Q3 bonus calc log"), 8821, 0.0};
    bool ok = client.execute(conn,
        "UPDATE ledger SET bonus = :1, notes = :2 WHERE id = :3 RETURNING total INTO :4",
        update);
    std::cout << "result=" << (ok ? "success" : "failed")
              << ", OCIStmtExecute calls=" << binding::mock::g_execute_calls.load()
              << " (expect 2: one failure, one reconnect+retry)\n\n";

    std::cout << "--- Demo 2: non-disconnect exec error, must NOT retry ---\n";
    binding::mock::set_mode(binding::mock::FailureMode::ExecErrorAlways);
    binding::mock::g_execute_calls = 0;
    FinancialUpdate dupe{0.12, binding::OciClob("dup"), 8821, 0.0};
    ok = client.execute(conn, "INSERT INTO ledger (bonus, notes, id) VALUES (:1, :2, :3)", dupe);
    std::cout << "result=" << (ok ? "success" : "failed")
              << ", OCIStmtExecute calls=" << binding::mock::g_execute_calls.load()
              << " (expect 1: no retry on a plain exec error)\n\n";

    std::cout << "--- Demo 3: SELECT into vector<T>, the result-row-set case ---\n";
    binding::mock::set_mode(binding::mock::FailureMode::None);
    std::vector<TradeRow> rows;
    client.query(conn, "SELECT trade_id, notional FROM trades", rows);
    for (const auto& r : rows) {
        std::cout << "trade_id=" << r.trade_id << " notional=" << r.notional << "\n";
    }
    std::cout << "\n";

    std::cout << "--- Demo 4: bind std::optional -- empty maps to SQL NULL ---\n";
    EmployeeRow with_commission{101, 250.5};
    client.execute(conn, "INSERT INTO employees (id, commission) VALUES (:1, :2)", with_commission);
    std::cout << "emp 101 (has commission): bind indicator="
              << binding::mock::g_last_bind_indicators[1] << " (expect 0 = NOT NULL)\n";

    EmployeeRow without_commission{102, std::nullopt};
    client.execute(conn, "INSERT INTO employees (id, commission) VALUES (:1, :2)", without_commission);
    std::cout << "emp 102 (no commission):  bind indicator="
              << binding::mock::g_last_bind_indicators[1] << " (expect -1 = NULL)\n\n";

    std::cout << "--- Demo 5: query std::optional -- NULL indicator maps back to nullopt ---\n";
    binding::mock::set_simulate_null_last_column(true); // commission is the last column here
    std::vector<EmployeeRow> employees;
    client.query(conn, "SELECT id, commission FROM employees", employees);
    for (const auto& e : employees) {
        std::cout << "emp_id=" << e.emp_id << " commission="
                  << (e.commission ? std::to_string(*e.commission) : std::string("NULL")) << "\n";
    }

    conn.disconnect();
    return 0;
}
