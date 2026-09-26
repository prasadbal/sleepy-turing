#pragma once
// QuerySqlRegistry: loads QueryDescriptor SQL text from an external XML
// file, keyed by name, so a query's actual text can be edited (by a DBA, or
// anyone who isn't rebuilding the C++) without a rebuild. A QueryDescriptor
// (query_descriptor.h) keeps bind_type/define_type/key_type in code -- those
// are structural, not something a config file should own -- and declares a
// query_name naming its entry here instead of embedding SQL text directly:
//
//   struct ObjectsByName {
//       using bind_type   = NoBind;
//       using define_type = ObjRow;
//       static constexpr std::string_view query_name = "objects_by_name";
//       using key_type    = std::index_sequence<0>;
//   };
//
// XML shape (see config/db_queries.xml for the real file):
//   <queries>
//     <query name="objects_by_name">
//       <sql><![CDATA[ SELECT object_name, object_type FROM all_objects ]]></sql>
//     </query>
//   </queries>
//
// Deliberately not a singleton and not auto-loaded: a caller constructs one
// (from_file() at startup, or from_string() in a test -- see below) and
// passes it to run_and_measure() explicitly, same as OciConnection/
// QueryMeter are passed explicitly rather than reached for through global
// state elsewhere in this module.
#include <pugixml.hpp>

#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>

namespace marketlib::db::oracle {

class QuerySqlRegistry {
public:
    // Parses the file at `path`. Throws std::runtime_error on a file that
    // doesn't exist or doesn't parse as XML, or a <query> missing its name
    // attribute or <sql> child -- a malformed queries file is a startup-time
    // configuration error, not something to discover later as a confusing
    // ORA- error from a query that silently ran with empty SQL text.
    [[nodiscard]] static QuerySqlRegistry from_file(const std::string& path) {
        pugi::xml_document doc;
        const pugi::xml_parse_result result = doc.load_file(path.c_str());
        if (!result) {
            throw std::runtime_error("QuerySqlRegistry: failed to parse '" + path +
                                      "': " + result.description());
        }
        return QuerySqlRegistry(doc, path);
    }

    // Same, from an in-memory XML string rather than a file -- what tests
    // use, so a test doesn't need a real file on disk with a path a test
    // runner has to get right; `source_label` is just what error messages
    // name, not read as a real path.
    [[nodiscard]] static QuerySqlRegistry from_string(std::string_view xml_text,
                                                       std::string_view source_label = "<string>") {
        pugi::xml_document doc;
        const pugi::xml_parse_result result = doc.load_string(std::string(xml_text).c_str());
        if (!result) {
            throw std::runtime_error("QuerySqlRegistry: failed to parse " + std::string(source_label) +
                                      ": " + result.description());
        }
        return QuerySqlRegistry(doc, source_label);
    }

    // The SQL text for `name`. Throws std::out_of_range if there's no
    // <query name="..."> entry for it -- a descriptor whose query_name
    // doesn't resolve is a configuration bug to surface clearly and
    // immediately, not to paper over with an empty string that would go on
    // to fail as an opaque "ORA-00900: invalid SQL statement" with no
    // indication of which descriptor or which missing entry caused it.
    [[nodiscard]] const std::string& sql_for(std::string_view name) const {
        const auto it = queries_.find(std::string(name));
        if (it == queries_.end()) {
            throw std::out_of_range("QuerySqlRegistry: no <query name=\"" + std::string(name) +
                                     "\"> entry (loaded from " + source_ + ")");
        }
        return it->second;
    }

    [[nodiscard]] std::size_t size() const noexcept { return queries_.size(); }

private:
    QuerySqlRegistry(const pugi::xml_document& doc, std::string_view source_label) : source_(source_label) {
        for (const pugi::xml_node query : doc.child("queries").children("query")) {
            const pugi::xml_attribute name_attr = query.attribute("name");
            const pugi::xml_node sql_node = query.child("sql");
            if (!name_attr || !sql_node) {
                throw std::runtime_error("QuerySqlRegistry: '" + source_ +
                                          "' has a <query> missing its name attribute or <sql> child");
            }
            std::string sql_text = sql_node.text().get();
            // Trim leading/trailing whitespace a CDATA block's own source
            // indentation adds -- cosmetic (Oracle doesn't care), but keeps
            // logged/printed SQL from being padded with blank lines.
            const auto first = sql_text.find_first_not_of(" \t\r\n");
            const auto last = sql_text.find_last_not_of(" \t\r\n");
            sql_text = (first == std::string::npos) ? std::string{} : sql_text.substr(first, last - first + 1);

            const std::string name = name_attr.value();
            if (!queries_.emplace(name, std::move(sql_text)).second) {
                throw std::runtime_error("QuerySqlRegistry: '" + source_ + "' has a duplicate <query name=\"" +
                                          name + "\">");
            }
        }
    }

    std::string source_; // the path or label this was loaded from, for error messages only
    std::unordered_map<std::string, std::string> queries_;
};

} // namespace marketlib::db::oracle
