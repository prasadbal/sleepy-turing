// Demo for the config-binding side of binding/: XML -> boost::property_tree
// -> binding::FieldList (parser-independent) -> a config_schema struct, via
// boost::pfr field names matched case-insensitively.
//
// Shows: plain leaves, a nested struct from an attribute-only element
// (<pool size="10"/>), a repeated element collecting into vector<T>
// (<replica .../> x3), an absent optional field, mixed-case XML keys against
// lowercase struct field names, a genuinely self-referential tree (a Node
// whose children are more Nodes, 4 levels deep), and the error path for a
// missing required field.

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

// A tree node: attributes (name, id) plus children of the SAME type --
// genuine self-reference. Field named `node` (not `children`) because the
// XML nests children directly as repeated <node> tags under this one --
// bind_from_fields matches by name, so the field has to match the tag.
struct TreeNode {
    std::string name;
    int id;
    std::vector<TreeNode> node;
};
static_assert(binding::config_schema<TreeNode>);

void dump_tree(const TreeNode& n, int depth) {
    std::cout << std::string(depth * 2, ' ') << n.name << " (id=" << n.id << ")\n";
    for (const auto& child : n.node) dump_tree(child, depth + 1);
}

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

    std::cout << "\n--- self-referential tree: a Node whose children are more Nodes ---\n";
    std::string tree_xml = R"(
<node name="root" id="1">
    <node name="branch-a" id="2">
        <node name="leaf-a1" id="4"/>
        <node name="leaf-a2" id="5"/>
    </node>
    <node name="branch-b" id="3">
        <node name="leaf-b1" id="6">
            <node name="leaf-b1-deep" id="7"/>
        </node>
    </node>
</node>
)";
    std::istringstream tree_iss(tree_xml);
    boost::property_tree::ptree tree_pt;
    boost::property_tree::read_xml(tree_iss, tree_pt);

    auto tree_fields = binding::from_ptree(tree_pt);
    TreeNode root{};
    binding::bind_from_fields(tree_fields.front().as_struct(), root);
    dump_tree(root, 0);

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
