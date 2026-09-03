// Demo for oci_collection_bind.h -- the Oracle collection-type alternative
// to oci_client.h's select_with_in_list(): one bound collection object
// instead of a generated ":1,:2,...,:N" placeholder list, so the SQL text
// stays fixed regardless of how many elements are in the list (and isn't
// subject to select_with_in_list()'s 1000-element ORA-01795 cap).
//
// See the warning banner at the top of oci_collection_bind.h: this exercises
// the C++ template plumbing against this repo's own mock, which proves the
// logic hangs together -- it does NOT prove the mock's OCIType/OCIObjectNew/
// OCICollAppend/OCIBindObject signatures faithfully match a real oci.h,
// which hasn't been checked against a real Oracle client.

#include <chrono>
#include <iostream>
#include <set>
#include <string>
#include <valarray>
#include <vector>

#include "binding/oci_client.h"
#include "binding/oci_collection_bind.h"
#include "binding/oci_connection.h"
#include "binding/oci_object_mock.h"

struct TradeRow {
    int trade_id;
    double notional;
};
static_assert(binding::bindable<TradeRow>);

int main() {
    binding::OciConnection conn("orcl", "app_user", "secret", 3, std::chrono::milliseconds(200));
    conn.connect();

    std::set<std::string> reference_codes = {"REF-C", "REF-A", "REF-A", "REF-B"}; // one duplicate, on purpose

    std::vector<TradeRow> rows;
    bool ok = binding::select_with_in_collection(
        conn,
        "SELECT trade_id, notional FROM trades "
        "WHERE ref_code IN (SELECT column_value FROM TABLE(:1))",
        reference_codes, rows);

    std::cout << "reference_codes has " << reference_codes.size() << " unique elements "
              << "(std::set already deduped REF-A)\n";
    std::cout << "OCITypeByName was asked for: "
              << binding::mock::g_last_type_lookup_schema << "."
              << binding::mock::g_last_type_lookup_name
              << " (expected SYS.ODCIVARCHAR2LIST for a std::string element type)\n";
    std::cout << "select_with_in_collection result=" << (ok ? "success" : "failed")
              << ", rows returned=" << rows.size()
              << " (the mock always returns its canned rows -- it doesn't actually filter)\n";
    for (const auto& r : rows) {
        std::cout << "  trade_id=" << r.trade_id << " notional=" << r.notional << "\n";
    }

    std::cout << "\n--- vector<int>/valarray<int> convenience overloads (still collection-bound) ---\n";
    std::vector<int> vector_ids = {305, 101, 305, 210, 101};
    std::vector<TradeRow> vector_rows;
    bool vector_ok = binding::select_with_in_collection(
        conn, "SELECT trade_id, notional FROM trades WHERE trade_id IN (SELECT column_value FROM TABLE(:1))",
        vector_ids, vector_rows);
    std::cout << "select_with_in_collection(vector<int>) result=" << (vector_ok ? "success" : "failed")
              << ", rows=" << vector_rows.size() << "\n";

    std::valarray<int> valarray_ids = {305, 101, 210};
    std::vector<TradeRow> valarray_rows;
    bool valarray_ok = binding::select_with_in_collection(
        conn, "SELECT trade_id, notional FROM trades WHERE trade_id IN (SELECT column_value FROM TABLE(:1))",
        valarray_ids, valarray_rows);
    std::cout << "select_with_in_collection(valarray<int>) result=" << (valarray_ok ? "success" : "failed")
              << ", rows=" << valarray_rows.size() << "\n";

    std::cout << "\n--- execute_with_in_collection: DML side of the same mechanism ---\n";
    bool delete_ok = binding::execute_with_in_collection(
        conn, "DELETE FROM trades WHERE trade_id IN (SELECT column_value FROM TABLE(:1))", vector_ids);
    std::cout << "execute_with_in_collection(vector<int>) result=" << (delete_ok ? "success" : "failed") << "\n";

    conn.disconnect();
}
