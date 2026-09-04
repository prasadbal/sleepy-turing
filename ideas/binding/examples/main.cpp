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
//   3.5. select() with an input-binding struct -- a WHERE clause bound by
//      name, the read-side counterpart to execute()'s bind_struct.
//   4. Binding std::optional -- an empty one maps to SQL NULL.
//   4.5. insert() with a vector<T> -- a real array bind (OCIBindArrayOfStruct),
//      one execute for the whole batch.
//   4.75. A struct field that's itself a std::set<int> -- binds as its own
//      dynamic IN-list, alongside an ordinary named scalar field in the
//      same statement.
//   5. select() with std::optional -- a NULL column maps back to nullopt.
//   6. A NULL landing on a field that isn't std::optional -- throws,
//      instead of silently leaving the field's stale/default value.
//   7. A row with string columns and a 64-bit id: FixedString<N> fetched as
//      a select() output column and bulk-inserted through a real array
//      bind, neither of which a std::string field can do.
//
// Builds against the mock OCI backend (binding/oci_mock.h) since there's no
// real Oracle client in this environment -- see oci_compat.h.

#include <chrono>
#include <iostream>
#include <set>
#include <vector>

#include "binding/oci_client.h"
#include "binding/oci_connection.h"
#include "binding/oci_fixed_string.h"
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

// ---- select() input-binding struct: an ordinary named WHERE-clause
// parameter, bound exactly like execute()'s bind_struct. ------------------
struct TradeFilter {
    int min_trade_id;
};
static_assert(binding::bindable<TradeFilter>);

// ---- nullable column demo: commission is a nullable NUMBER -------------
struct EmployeeRow {
    int emp_id;
    std::optional<double> commission;
};
static_assert(binding::bindable<EmployeeRow>);

// ---- mixed scalar + dynamic multi-value IN-list, in ONE struct: a plain
// named field (status) alongside a std::set<int> field (trade_ids), which
// binds as its own dynamic IN-list -- the query text's "{trade_ids}" marker
// (named after the field, same convention as "{IN}") gets replaced with a
// placeholder list sized to trade_ids.size() before preparing. -----------
struct TradeStatusUpdate {
    std::string status;
    std::set<int> trade_ids;
};
static_assert(binding::bindable<TradeStatusUpdate>);

// ---- a realistic reporting row: string keys, a 64-bit id, a sparse
// numeric. std::string works as an execute()/insert() *parameter* but can be
// neither a select() output column nor a bulk-insert column -- OCI needs a
// fixed maximum buffer size up front, and a string's characters live in its
// own heap/SSO storage rather than inline in the row at a fixed stride.
// FixedString<N> supplies both: N is the buffer size, and the characters sit
// inside the row struct where an array bind/define can stride over them. ---
struct ReportRow {
    std::int64_t                     position_id;
    binding::FixedString<16>         desk;
    binding::FixedString<8>          risk_class;
    double                           delta;
    std::optional<double>            vega; // sparse: absent for non-optionable risk
};
static_assert(binding::bindable<ReportRow>);

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

    std::cout << "--- Demo 3.5: select() with an input-binding struct -- WHERE clause bound by name ---\n";
    TradeFilter filter_by_id{200};
    std::vector<TradeRow> filtered_rows;
    client.select(conn, "SELECT trade_id, notional FROM trades WHERE trade_id >= :min_trade_id",
                  filter_by_id, filtered_rows);
    std::cout << "rows returned=" << filtered_rows.size()
              << " (the mock always returns its canned rows -- it doesn't actually filter by min_trade_id)\n\n";

    std::cout << "--- Demo 4: bind std::optional -- empty maps to SQL NULL ---\n";
    EmployeeRow with_commission{101, 250.5};
    client.execute(conn, "INSERT INTO employees (id, commission) VALUES (:emp_id, :commission)", with_commission);
    std::cout << "emp 101 (has commission): bind indicator="
              << binding::mock::g_last_bind_indicators[1] << " (expect 0 = NOT NULL)\n";

    EmployeeRow without_commission{102, std::nullopt};
    client.execute(conn, "INSERT INTO employees (id, commission) VALUES (:emp_id, :commission)", without_commission);
    std::cout << "emp 102 (no commission):  bind indicator="
              << binding::mock::g_last_bind_indicators[1] << " (expect -1 = NULL)\n\n";

    std::cout << "--- Demo 4.5: insert() with a vector<T> -- a real array bind, ONE execute for all rows ---\n";
    binding::mock::g_execute_calls = 0;
    std::vector<TradeRow> new_trades{
        {301, 10.0}, {302, 20.0}, {303, 30.0},
    };
    bool bulk_ok = client.insert(
        conn, "INSERT INTO trades (trade_id, notional) VALUES (:trade_id, :notional)", new_trades);
    std::cout << "result=" << (bulk_ok ? "success" : "failed")
              << ", OCIStmtExecute calls=" << binding::mock::g_execute_calls.load()
              << ", iters=" << binding::mock::g_last_iters.load()
              << " (expect calls=1, iters=3: one OCIStmtExecute for the whole batch via "
              << "OCIBindArrayOfStruct, not one call per row)\n\n";

    std::cout << "--- Demo 4.75: struct field is itself a dynamic multi-value IN-list ---\n";
    TradeStatusUpdate filter{"CLOSED", {305, 101, 305, 210, 101}}; // duplicates, out of order, on purpose
    bool mixed_ok = client.execute(
        conn, "UPDATE trades SET status = :status WHERE trade_id IN ({trade_ids})", filter);
    std::cout << "result=" << (mixed_ok ? "success" : "failed")
              << " -- status bound by name (:status), trade_ids bound as its own "
              << filter.trade_ids.size() << "-element IN-list ({trade_ids} -> "
              << binding::make_named_placeholders("trade_ids", filter.trade_ids.size()) << ")\n\n";

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
    std::cout << "\n";

    std::cout << "--- Demo 7: string columns (FixedString<N>) and a 64-bit id, both directions ---\n";
    std::vector<ReportRow> report;
    client.select(conn, "SELECT position_id, desk, risk_class, delta, vega FROM sensitivities", report);
    for (const auto& r : report) {
        std::cout << "  position_id=" << r.position_id
                  << " desk=" << r.desk.str()
                  << " risk_class=" << r.risk_class.str()
                  << " delta=" << r.delta
                  << " vega=" << (r.vega ? std::to_string(*r.vega) : std::string("NULL")) << "\n";
    }
    std::cout << "  (each value is truncated to its own field's capacity -- desk holds 16 chars,\n"
              << "   risk_class 8, exactly as a VARCHAR2 column of that width would)\n";

    std::vector<ReportRow> to_write{
        {9001, binding::FixedString<16>("RATES_LDN"), binding::FixedString<8>("IR_1"), 1234.56, 12.5},
        {9002, binding::FixedString<16>("FX_NY"),     binding::FixedString<8>("FX_2"),  987.65, std::nullopt},
    };
    // Only the non-optional columns array-bind, so this row type goes one row
    // at a time -- the vector<T> overload static_asserts on the optional.
    for (auto& row : to_write) {
        client.insert(conn, "INSERT INTO sensitivities VALUES(:position_id,:desk,:risk_class,:delta,:vega)", row);
    }
    std::cout << "  inserted " << to_write.size() << " rows one at a time (vega is std::optional,\n"
              << "   which the array-bind overload rejects at compile time)\n";

    conn.disconnect();
    return 0;
}
