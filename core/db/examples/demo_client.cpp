// Demo for oci_client.h -- the reflection layer built on top of
// OciStatement (mock backend, same one demo.cpp uses).
#include <cstdio>
#include <optional>
#include <vector>

#include "binding/oci_client.h"
#include "binding/oci_connection.h"

using namespace binding;

int main() {
    OciConnection conn("mockdb", "user", "pass");
    conn.connect();

    std::printf("--- execute() -- no bind ---\n");
    {
        auto r = execute(conn, "CREATE TABLE trades (trade_id NUMBER, notional NUMBER, name VARCHAR2(16))");
        std::printf("result=%s\n", r.status == ExecStatus::Success ? "Success" : "Fail");
    }

    std::printf("--- execute() with bind params (optional field) ---\n");
    {
        struct Params { int trade_id; std::optional<double> notional; };
        Params p{100, 42.5};
        auto r = execute(conn, "UPDATE trades SET notional=:notional WHERE trade_id=:trade_id", p);
        std::printf("result=%s\n", r.status == ExecStatus::Success ? "Success" : "Fail");
    }

    std::printf("--- select_rows() -- batch fetch, plain scalar row (apply loop elided) ---\n");
    {
        struct Row { int trade_id; double notional; };
        std::vector<Row> collected;
        auto r = select_rows<Row>(conn, "SELECT trade_id, notional FROM trades", 100, 10,
            [&](const Row* rows, std::size_t n) {
                for (std::size_t i = 0; i < n; ++i) collected.push_back(rows[i]);
            });
        std::printf("result=%s rows=%zu\n", r.status == ExecStatus::Success ? "Success" : "Fail", collected.size());
        for (auto& row : collected) std::printf("  trade_id=%d notional=%f\n", row.trade_id, row.notional);
    }

    std::printf("--- select_rows() -- optional field (apply loop engaged) ---\n");
    {
        struct Row { int trade_id; std::optional<double> notional; };
        std::vector<Row> collected;
        auto r = select_rows<Row>(conn, "SELECT trade_id, notional FROM trades", 100, 10,
            [&](const Row* rows, std::size_t n) {
                for (std::size_t i = 0; i < n; ++i) collected.push_back(rows[i]);
            });
        std::printf("result=%s rows=%zu\n", r.status == ExecStatus::Success ? "Success" : "Fail", collected.size());
        for (auto& row : collected)
            std::printf("  trade_id=%d notional=%s\n", row.trade_id,
                        row.notional ? std::to_string(*row.notional).c_str() : "NULL");
    }

    std::printf("--- select() -- struct-free positional fetch ---\n");
    {
        int trade_id = 0;
        double notional = 0.0;
        auto r = select(conn, "SELECT trade_id, notional FROM trades WHERE trade_id=100", trade_id, notional);
        std::printf("result=%s trade_id=%d notional=%f\n", r.status == ExecStatus::Success ? "Success" : "Fail",
                    trade_id, notional);
    }

    std::printf("--- insert_rows() -- array bind, plain scalar rows ---\n");
    {
        struct Row { int trade_id; double notional; };
        std::vector<Row> rows = {{1, 1.5}, {2, 3.0}, {3, 4.5}, {4, 6.0}, {5, 7.5}};
        auto r = insert_rows(conn, "INSERT INTO trades(trade_id, notional) VALUES(:trade_id, :notional)", rows, 2);
        std::printf("result=%s\n", r.status == ExecStatus::Success ? "Success" : "Fail");
    }

    std::printf("--- execute()/select_rows() with a LOB field (OciClob) ---\n");
    {
        struct Params { int trade_id; OciClob report; };
        Params p{100, OciClob("FRTB sensitivities report body")};
        auto r1 = execute(conn, "INSERT INTO trades(trade_id, name) VALUES(:trade_id, :report)", p);
        std::printf("insert result=%s\n", r1.status == ExecStatus::Success ? "Success" : "Fail");

        struct Row { int trade_id; OciClob report; };
        std::vector<Row> collected;
        auto r2 = select_rows<Row>(conn, "SELECT trade_id, name FROM trades WHERE trade_id=100", 10, 10,
            [&](const Row* rows, std::size_t n) {
                for (std::size_t i = 0; i < n; ++i) collected.push_back(rows[i]);
            });
        std::printf("select result=%s rows=%zu\n", r2.status == ExecStatus::Success ? "Success" : "Fail",
                    collected.size());
        for (auto& row : collected)
            std::printf("  trade_id=%d report=[%s]\n", row.trade_id, row.report.text_data.c_str());
    }

    conn.disconnect();
    return 0;
}
