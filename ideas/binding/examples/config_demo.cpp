// Demo for the config-binding side of binding/: XML -> boost::property_tree
// -> binding::FieldList (parser-independent) -> a config_schema struct, via
// boost::pfr field names matched case-insensitively.
//
// Shows: plain leaves, a nested struct from an attribute-only element
// (<pool size="10"/>), a repeated element collecting into vector<T>
// (<replica .../> x3), an absent optional field, mixed-case XML keys against
// lowercase struct field names, and the error path for a missing required
// field.

#include <boost/property_tree/ptree.hpp>
#include <boost/property_tree/xml_parser.hpp>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "binding/config_bind.h"
#include "binding/field_tree.h"
#include "binding/ptree_bridge.h"
#include "binding/reflect.h"

struct Replica {
    std::string name;
    int priority;
};
static_assert(binding::config_schema<Replica>);

struct Pool {
    int size;
};
static_assert(binding::config_schema<Pool>);

struct DbConfig {
    std::string host;
    int port;
    Pool pool;
    double timeout_seconds;
    std::vector<Replica> replica;
    std::optional<std::string> description; // not present in the sample XML
};
static_assert(binding::config_schema<DbConfig>);

int main() {
    // Mixed-case keys on purpose, to demonstrate case-insensitive matching.
    std::string xml = R"(
<config HOST="db.example.com" Port="5432">
    <Pool SIZE="10"/>
    <Timeout_Seconds>30.5</Timeout_Seconds>
    <REPLICA name="r1" PRIORITY="1"/>
    <replica NAME="r2" priority="2"/>
    <Replica name="r3" priority="3"/>
</config>
)";
    std::istringstream iss(xml);
    boost::property_tree::ptree pt;
    boost::property_tree::read_xml(iss, pt);

    auto fields = binding::from_ptree(pt.get_child("config"));

    DbConfig cfg{};
    binding::bind_from_fields(fields, cfg);

    std::cout << "host=" << cfg.host << "\n";
    std::cout << "port=" << cfg.port << "\n";
    std::cout << "pool.size=" << cfg.pool.size << "\n";
    std::cout << "timeout_seconds=" << cfg.timeout_seconds << "\n";
    std::cout << "description=" << (cfg.description ? *cfg.description : std::string("(nullopt)")) << "\n";
    std::cout << "replica count=" << cfg.replica.size() << "\n";
    for (const auto& r : cfg.replica) {
        std::cout << "  replica name=" << r.name << " priority=" << r.priority << "\n";
    }

    std::cout << "\n--- bind_flat_fields: same binder, restricted to flat_schema ---\n";
    // A plain record shape -- a database row being the common case -- where
    // nesting/vector<U> would be a mistake, not a feature. bind_flat_fields
    // requires flat_schema<T> instead of config_schema<T>, so accidentally
    // adding a nested or vector<U> field to DbRow later is a compile error
    // right here, rather than silently compiling under the more permissive
    // config_schema.
    struct DbRow {
        int id;
        std::string name;
        double amount;
    };
    static_assert(binding::flat_schema<DbRow>);

    binding::FieldList row_fields{
        binding::Field{"id", std::string("42")},
        binding::Field{"name", std::string("widget")},
        binding::Field{"amount", std::string("19.99")},
    };
    DbRow row{};
    binding::bind_flat_fields(row_fields, row);
    std::cout << "id=" << row.id << " name=" << row.name << " amount=" << row.amount << "\n";

    std::cout << "\n--- missing required field: error path ---\n";
    try {
        struct Strict { int required_thing; };
        Strict s{};
        binding::FieldList empty;
        binding::bind_from_fields(empty, s);
        std::cout << "ERROR: expected an exception for a missing required field\n";
    } catch (const std::exception& e) {
        std::cout << "threw as expected: " << e.what() << "\n";
    }
}
