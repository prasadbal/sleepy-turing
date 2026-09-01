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

    conn.disconnect();
}
