// Demo for the binding/ idea: one boost::pfr reflection core (reflect.h)
// shared between a config-style "flat_schema" check and an Oracle OCI
// binder/reconnect layer. The client's methods read as SQL verbs:
// execute() runs any statement that returns no rows, insert() is a
// same-mechanism alias for execute() (plus a vector<T> overload for several
// rows), select() runs a query and returns its rows. Scenarios:
//
//   0. execute() with no bind struct at all -- for DDL/literal-only DML.
//   1. A transient disconnect during execute() -- one reconnect, then the
//      same statement succeeds.
//   2. A plain execution error (e.g. a constraint violation) -- must NOT be
//      retried, must fail on the first attempt.
//   3. select() into vector<T> -- the "vector<S> as a result set" case,
//      mirroring how a config file binds repeated sections into vector<S>.
//   4. Binding std::optional -- an empty one maps to SQL NULL.
//   4.5. insert() with a vector<T> -- several rows, one execute per row.
//   5. select() with std::optional -- a NULL column maps back to nullopt.
//   5.5. A dynamic IN (...) list: dedup + deterministic bind order via
//      std::set, for both select_with_in_list() and execute_with_in_list()
//      (std::vector and std::valarray ID collections both accepted).
//   6. A NULL landing on a field that isn't std::optional -- throws,
//      instead of silently leaving the field's stale/default value.
//
// Builds against the mock OCI backend (binding/oci_mock.h) since there's no
// real Oracle client in this environment -- see oci_compat.h.

#include <chrono>
#include <iostream>
#include <set>
#include <valarray>
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

// ---- OCI DML row: fields bind by name (:emp_id, :bonus_pct, ...) -- any
// declaration order is fine, since matching doesn't depend on where each
// placeholder happens to occur in the SQL text. ---------------------------
struct FinancialUpdate {
    int emp_id;
    double bonus_pct;
    binding::OciClob notes;
    double final_payout;
};
static_assert(binding::bindable<FinancialUpdate>);

// ---- OCI SELECT row: field order matches the SELECT list order ----------
struct TradeRow {
    int trade_id;
    double notional;
};
static_assert(binding::bindable<TradeRow>);

// ---- nullable column demo: commission is a nullable NUMBER -------------
struct EmployeeRow {
    int emp_id;
    std::optional<double> commission;
};
static_assert(binding::bindable<EmployeeRow>);

int main() {
    binding::OciConnection conn("orcl", "app_user", "secret",
                                /*max_retries=*/3, std::chrono::milliseconds(200));
    conn.connect();
    binding::OciClient client;

    std::cout << "--- Demo 0: execute() with no bind struct -- DDL/literal-only DML ---\n";
    binding::mock::set_mode(binding::mock::FailureMode::None);
    bool ddl_ok = client.execute(conn, "TRUNCATE TABLE staging_trades");
    std::cout << "result=" << (ddl_ok ? "success" : "failed") << "\n\n";

    std::cout << "--- Demo 1: transient disconnect mid-execute, one reconnect, then success ---\n";
    binding::mock::set_mode(binding::mock::FailureMode::DisconnectThenRecover, /*disconnect_count=*/1);
    binding::mock::g_execute_calls = 0;
    FinancialUpdate update{8821, 0.12, binding::OciClob("Q3 bonus calc log"), 0.0};
    bool ok = client.execute(conn,
        "UPDATE ledger SET bonus = :bonus_pct, notes = :notes "
        "WHERE id = :emp_id RETURNING total INTO :final_payout",
        update);
    std::cout << "result=" << (ok ? "success" : "failed")
              << ", OCIStmtExecute calls=" << binding::mock::g_execute_calls.load()
              << " (expect 2: one failure, one reconnect+retry)\n\n";

    std::cout << "--- Demo 2: non-disconnect exec error, must NOT retry ---\n";
    binding::mock::set_mode(binding::mock::FailureMode::ExecErrorAlways);
    binding::mock::g_execute_calls = 0;
    FinancialUpdate dupe{8821, 0.12, binding::OciClob("dup"), 0.0};
    ok = client.execute(conn,
        "INSERT INTO ledger (bonus, notes, id) VALUES (:bonus_pct, :notes, :emp_id)", dupe);
    std::cout << "result=" << (ok ? "success" : "failed")
              << ", OCIStmtExecute calls=" << binding::mock::g_execute_calls.load()
              << " (expect 1: no retry on a plain exec error)\n\n";

    std::cout << "--- Demo 3: select() into vector<T>, the result-row-set case ---\n";
    binding::mock::set_mode(binding::mock::FailureMode::None);
    std::vector<TradeRow> rows;
    client.select(conn, "SELECT trade_id, notional FROM trades", rows);
    for (const auto& r : rows) {
        std::cout << "trade_id=" << r.trade_id << " notional=" << r.notional << "\n";
    }
    std::cout << "\n";

    std::cout << "--- Demo 4: bind std::optional -- empty maps to SQL NULL ---\n";
    EmployeeRow with_commission{101, 250.5};
    client.execute(conn, "INSERT INTO employees (id, commission) VALUES (:emp_id, :commission)", with_commission);
    std::cout << "emp 101 (has commission): bind indicator="
              << binding::mock::g_last_bind_indicators[1] << " (expect 0 = NOT NULL)\n";

    EmployeeRow without_commission{102, std::nullopt};
    client.execute(conn, "INSERT INTO employees (id, commission) VALUES (:emp_id, :commission)", without_commission);
    std::cout << "emp 102 (no commission):  bind indicator="
              << binding::mock::g_last_bind_indicators[1] << " (expect -1 = NULL)\n\n";

    std::cout << "--- Demo 4.5: insert() with a vector<T> -- several rows, one execute per row ---\n";
    binding::mock::g_execute_calls = 0;
    std::vector<EmployeeRow> new_employees{
        {201, 100.0}, {202, std::nullopt}, {203, 75.5},
    };
    bool bulk_ok = client.insert(
        conn, "INSERT INTO employees (id, commission) VALUES (:emp_id, :commission)", new_employees);
    std::cout << "result=" << (bulk_ok ? "success" : "failed")
              << ", OCIStmtExecute calls=" << binding::mock::g_execute_calls.load()
              << " (expect 3: a naive per-row loop for now -- see the TODO on insert(vector<T>&))\n\n";

    std::cout << "--- Demo 5.5: dynamic IN (...) list -- dedup + deterministic order via std::set ---\n";
    binding::mock::set_mode(binding::mock::FailureMode::None);
    std::vector<int> raw_ids = {305, 101, 305, 210, 101}; // duplicates, out of order, on purpose
    std::set<int> unique_ids(raw_ids.begin(), raw_ids.end());
    std::cout << "input vector had " << raw_ids.size() << " elements (with duplicates, unordered); "
              << "deduped set has " << unique_ids.size() << ": "
              << binding::make_in_placeholders(unique_ids.size(), 1)
              << " (positions map to the set's sorted order: 101,210,305)\n";

    std::vector<TradeRow> in_rows;
    bool in_ok = client.select_with_in_list(
        conn, "SELECT trade_id, notional FROM trades WHERE trade_id IN ({IN})", raw_ids, in_rows);
    std::cout << "select_with_in_list (vector overload) result=" << (in_ok ? "success" : "failed")
              << ", rows returned=" << in_rows.size()
              << " (the mock always returns its canned rows -- it doesn't actually filter by id)\n";

    bool del_ok = client.execute_with_in_list(
        conn, "DELETE FROM trades WHERE trade_id IN ({IN})", unique_ids);
    std::cout << "execute_with_in_list result=" << (del_ok ? "success" : "failed") << "\n";

    std::valarray<int> valarray_ids = {305, 101, 210}; // same ids, as a std::valarray this time
    std::vector<TradeRow> valarray_rows;
    bool valarray_ok = client.select_with_in_list(
        conn, "SELECT trade_id, notional FROM trades WHERE trade_id IN ({IN})", valarray_ids, valarray_rows);
    std::cout << "select_with_in_list (std::valarray overload) result=" << (valarray_ok ? "success" : "failed")
              << ", rows returned=" << valarray_rows.size() << "\n";

    std::cout << "empty ID list generates: \"" << binding::make_in_placeholders(0, 1)
              << "\" (matches nothing, without a SQL syntax error)\n\n";

    std::cout << "--- Demo 5: select() with std::optional -- NULL indicator maps back to nullopt ---\n";
    binding::mock::set_simulate_null_last_column(true); // commission is the last column here
    std::vector<EmployeeRow> employees;
    client.select(conn, "SELECT id, commission FROM employees", employees);
    for (const auto& e : employees) {
        std::cout << "emp_id=" << e.emp_id << " commission="
                  << (e.commission ? std::to_string(*e.commission) : std::string("NULL")) << "\n";
    }
    std::cout << "\n";

    std::cout << "--- Demo 6: NULL on a non-optional field throws -- no schema to check this ahead of time ---\n";
    binding::mock::set_simulate_null_last_column(true); // notional is the last column here too
    std::vector<TradeRow> maybe_null_rows;
    try {
        client.select(conn, "SELECT trade_id, notional FROM trades", maybe_null_rows);
        std::cout << "ERROR: expected a throw -- notional is not std::optional\n";
    } catch (const std::exception& e) {
        std::cout << "threw as expected: " << e.what() << "\n";
    }
    binding::mock::set_simulate_null_last_column(false);

    conn.disconnect();
    return 0;
}
