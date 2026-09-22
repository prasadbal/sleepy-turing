// Tests for the db layer, run against the OCI mock (include/db/oracle/
// oci_mock.h). The mock returns deterministic canned data -- three rows,
// trade_id 100/110/120 with notional 2.5/4.0/5.5, and LOB columns reading
// back as "lob_row<i>_col1" -- so these assert exact values.
//
// Nothing here can tell you the layer works against a real Oracle: that is
// what core/db/examples/live_oracle_*.cpp are for. What these do pin down is
// everything that doesn't need a database to be right -- the reflection
// layer's struct walking, NULL/optional handling, batching, statement state
// checking, and OCI status classification.
#include <catch2/catch_test_macros.hpp>

#include <db/oracle/oci_client.h>
#include <db/oracle/oci_connection.h>
#include <db/oracle/oci_log.h>
#include <db/oracle/oci_lob.h>
#include <db/oracle/oci_statement.h>

#include <optional>
#include <string>
#include <vector>

using namespace marketlib::db::oracle;

#if MARKETLIB_DB_HAS_REAL_OCI

TEST_CASE("db: mock-backed tests are skipped against a real Oracle client", "[db]") {
    SKIP("these assert the OCI mock's canned data; see core/db/examples/live_oracle_*.cpp");
}

#else

namespace {

// A connected mock connection, disconnected on scope exit.
struct Connected {
    OciConnection conn{"orcl", "app_user", "secret"};
    Connected() { REQUIRE(conn.connect()); }
    ~Connected() { conn.disconnect(); }
};

// The mock's fetch cursor is process-global state; put it back no matter how
// a test exits so one test can't poison the next.
struct FetchCursorReset {
    ~FetchCursorReset() { mock::g_fetch_row = 0; }
};

struct TradeRow { int trade_id; double notional; };
struct TradeRowOpt { int trade_id; std::optional<double> notional; };

// Compile-time field-type gating, checked through the concept rather than by
// trying to instantiate execute()/select_rows() with a bad struct.
struct WithString { std::string name; };
struct WithOptionalFixedString { std::optional<FixedString<8>> name; };
struct WithOptionalLob { std::optional<OciClob> body; };

} // namespace

TEST_CASE("connection: connect and disconnect", "[db]") {
    OciConnection conn("orcl", "app_user", "secret");
    REQUIRE_FALSE(conn.connected());
    REQUIRE(conn.connect());
    REQUIRE(conn.connected());
    conn.disconnect();
    REQUIRE_FALSE(conn.connected());
}

TEST_CASE("scalar_bindable: which structs the reflection layer accepts", "[db]") {
    STATIC_REQUIRE(scalar_bindable<TradeRow>);
    STATIC_REQUIRE(scalar_bindable<TradeRowOpt>);

    // std::string has no fixed size to bind into or define from.
    STATIC_REQUIRE_FALSE(scalar_bindable<WithString>);
    // A plain FixedString<N> is already nullable through length()==0, so
    // wrapping it in optional would be a second way to say the same thing.
    STATIC_REQUIRE_FALSE(scalar_bindable<WithOptionalFixedString>);
    // A nullable LOB isn't wired in.
    STATIC_REQUIRE_FALSE(scalar_bindable<WithOptionalLob>);
}

TEST_CASE("execute: DDL with no bind parameters", "[db]") {
    Connected c;
    auto r = execute(c.conn, "CREATE TABLE trades (trade_id NUMBER, notional NUMBER)");
    REQUIRE(r.status == ExecStatus::Success);
}

TEST_CASE("execute: bind struct, and NULL logged for an empty optional", "[db]") {
    Connected c;

    std::vector<std::string> logged;
    set_statement_logger([&](std::string_view line) { logged.emplace_back(line); });
    struct LoggerReset { ~LoggerReset() { set_statement_logger(nullptr); } } reset;

    TradeRowOpt with_value{100, 42.5};
    TradeRowOpt with_null{100, std::nullopt};
    const std::string sql = "UPDATE trades SET notional=:notional WHERE trade_id=:trade_id";

    REQUIRE(execute(c.conn, sql, with_value).status == ExecStatus::Success);
    REQUIRE(execute(c.conn, sql, with_null).status == ExecStatus::Success);

    REQUIRE(logged.size() == 2);
    // Substring checks, not exact lines: which of the two binds logs first
    // isn't something these tests should pin down.
    CHECK(logged[0].find("notional=42.5") != std::string::npos);
    CHECK(logged[0].find("trade_id=100") != std::string::npos);
    CHECK(logged[1].find("notional=NULL") != std::string::npos);
}

TEST_CASE("select_rows: plain scalar rows", "[db]") {
    Connected c;

    std::vector<TradeRow> rows;
    auto r = select_rows<TradeRow>(c.conn, "SELECT trade_id, notional FROM trades", 100, 10,
        [&](const TradeRow* batch, std::size_t n) { rows.insert(rows.end(), batch, batch + n); });

    REQUIRE(r.status == ExecStatus::Success);
    REQUIRE(rows.size() == 3);
    CHECK(rows[0].trade_id == 100);
    CHECK(rows[0].notional == 2.5);
    CHECK(rows[1].trade_id == 110);
    CHECK(rows[1].notional == 4.0);
    CHECK(rows[2].trade_id == 120);
    CHECK(rows[2].notional == 5.5);
}

TEST_CASE("select_rows: optional field carries the value when not NULL", "[db]") {
    Connected c;

    std::vector<TradeRowOpt> rows;
    auto r = select_rows<TradeRowOpt>(c.conn, "SELECT trade_id, notional FROM trades", 100, 10,
        [&](const TradeRowOpt* batch, std::size_t n) { rows.insert(rows.end(), batch, batch + n); });

    REQUIRE(r.status == ExecStatus::Success);
    REQUIRE(rows.size() == 3);
    for (const auto& row : rows) REQUIRE(row.notional.has_value());
    CHECK(*rows[0].notional == 2.5);
    CHECK(*rows[2].notional == 5.5);
}

TEST_CASE("select_rows: a batch size smaller than the result set", "[db]") {
    Connected c;

    std::vector<std::size_t> batch_sizes;
    std::vector<TradeRow> rows;
    auto r = select_rows<TradeRow>(c.conn, "SELECT trade_id, notional FROM trades", 100, 2,
        [&](const TradeRow* batch, std::size_t n) {
            batch_sizes.push_back(n);
            rows.insert(rows.end(), batch, batch + n);
        });

    REQUIRE(r.status == ExecStatus::Success);
    // Nothing dropped or duplicated across batch boundaries, and no batch
    // larger than what was asked for.
    REQUIRE(rows.size() == 3);
    CHECK(rows[0].trade_id == 100);
    CHECK(rows[1].trade_id == 110);
    CHECK(rows[2].trade_id == 120);
    for (std::size_t n : batch_sizes) CHECK(n <= 2);
}

TEST_CASE("select: struct-free positional single-row fetch", "[db]") {
    Connected c;

    int trade_id = 0;
    double notional = 0.0;
    auto r = select(c.conn, "SELECT trade_id, notional FROM trades WHERE trade_id=100", trade_id, notional);

    REQUIRE(r.status == ExecStatus::Success);
    CHECK(trade_id == 100);
    CHECK(notional == 2.5);
}

TEST_CASE("insert_rows: chunked array bind", "[db]") {
    Connected c;
    const std::string sql = "INSERT INTO trades(trade_id, notional) VALUES(:trade_id, :notional)";

    SECTION("chunk size that leaves a partial final chunk") {
        std::vector<TradeRow> rows = {{1, 1.5}, {2, 3.0}, {3, 4.5}, {4, 6.0}, {5, 7.5}};
        REQUIRE(insert_rows(c.conn, sql, rows, 2).status == ExecStatus::Success);
    }
    SECTION("chunk size larger than the row count") {
        std::vector<TradeRow> rows = {{1, 1.5}, {2, 3.0}};
        REQUIRE(insert_rows(c.conn, sql, rows, 100).status == ExecStatus::Success);
    }
}

TEST_CASE("LOB: OciClob binds on insert and reads back on select", "[db]") {
    Connected c;

    struct Params { int trade_id; OciClob report; };
    Params p{100, OciClob("FRTB sensitivities report body")};
    REQUIRE(execute(c.conn, "INSERT INTO trades(trade_id, name) VALUES(:trade_id, :report)", p).status ==
            ExecStatus::Success);

    struct Row { int trade_id; OciClob report; };
    std::vector<Row> rows;
    auto r = select_rows<Row>(c.conn, "SELECT trade_id, name FROM trades", 10, 10,
        [&](const Row* batch, std::size_t n) { rows.insert(rows.end(), batch, batch + n); });

    REQUIRE(r.status == ExecStatus::Success);
    REQUIRE(rows.size() == 3);
    // Each row's LOB is its own locator, not a shared or reused one.
    CHECK(rows[0].report.text_data == "lob_row0_col1");
    CHECK(rows[1].report.text_data == "lob_row1_col1");
    CHECK(rows[2].report.text_data == "lob_row2_col1");
}

TEST_CASE("OCILob: temporary CLOB write and read-back", "[db]") {
    Connected c;

    OCILob lob(c.conn);
    lob.create_temporary(OCI_TEMP_CLOB);
    const std::string value = "FRTB sensitivities report body";
    lob.write(value.data(), value.size());
    CHECK(lob.read(/*is_char_lob=*/true) == value);
}

TEST_CASE("OciStatement: using it out of sequence throws instead of calling OCI", "[db]") {
    Connected c;

    OciStatement stmt(c.conn);
    stmt.prepare("SELECT trade_id FROM trades");

    // fetch() before execute() -- a real OCI would answer this with an opaque
    // ORA- error; the wrapper knows better and says what to do instead.
    REQUIRE_THROWS_AS(stmt.fetch(10), OciStatementStateError);
    try {
        stmt.fetch(10);
        FAIL("fetch() before execute() should have thrown");
    } catch (const OciStatementStateError& e) {
        CHECK(std::string(e.what()).find("execute()") != std::string::npos);
    }
}

TEST_CASE("OciStatement: a zero-row result is Success, not an error", "[db]") {
    Connected c;
    FetchCursorReset reset;

    OciStatement stmt(c.conn);
    stmt.prepare("SELECT trade_id FROM empty_table");
    int trade_id = -1;
    stmt.bindOutput(1, SQLT_INT, &trade_id, sizeof(trade_id), nullptr, nullptr);

    // Simulate an already-exhausted result set.
    mock::g_fetch_row = mock::MOCK_ROW_COUNT;
    auto r = stmt.execute(1);

    // OCI_NO_DATA means "SELECT found nothing" -- a normal outcome, not a
    // failure. Misclassifying it as one was a real bug in the older tree.
    CHECK(r.status == ExecStatus::Success);
    CHECK(r.call.status == OCI_NO_DATA);
    CHECK(stmt.state() == OciStatement::State::EndOfFetch);
}

TEST_CASE("OciStatement: bindNameArray re-binds per chunk on one prepared statement", "[db]") {
    Connected c;

    struct Row { int id; double notional; };
    std::vector<Row> rows = {{1, 1.5}, {2, 3.0}, {3, 4.5}, {4, 6.0}, {5, 7.5}};
    constexpr std::size_t chunk_size = 2; // 2 + 2 + 1: exercises a partial final chunk

    OciStatement stmt(c.conn);
    stmt.prepare("INSERT INTO trades VALUES(:id, :notional)");
    std::vector<sb2> indicators(chunk_size, OCI_IND_NOTNULL);

    std::size_t chunks_run = 0;
    for (std::size_t offset = 0; offset < rows.size(); offset += chunk_size) {
        const std::size_t this_chunk = std::min(chunk_size, rows.size() - offset);
        stmt.bindNameArray("id", SQLT_INT, &rows[offset].id, sizeof(int), sizeof(Row), indicators.data());
        stmt.bindNameArray("notional", SQLT_BDOUBLE, &rows[offset].notional, sizeof(double), sizeof(Row),
                           indicators.data());
        auto r = stmt.execute(static_cast<ub4>(this_chunk));
        REQUIRE(r.status == ExecStatus::Success);
        REQUIRE(stmt.state() == OciStatement::State::Executed);
        ++chunks_run;
    }
    CHECK(chunks_run == 3);
}

#endif // MARKETLIB_DB_HAS_REAL_OCI
