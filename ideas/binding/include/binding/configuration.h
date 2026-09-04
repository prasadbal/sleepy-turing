#pragma once
#include <optional>
#include <string_view>
#include <utility>

#include "binding/config_bind.h"
#include "binding/config_field.h"

// ============================================================================
// A facade over FieldList (config_field.h): once a source has been parsed
// into one (via from_ptree()/from_toml(), or any future from_X() a new
// backend adds), nothing past this point needs to see Field/FieldList
// itself. A Configuration is built once from that FieldList and answers
// exactly the two things a caller actually wants -- read one ad hoc value
// by path, or bind the whole thing onto a config_schema struct -- both
// already implemented by get_leaf/try_get_leaf/bind_from_fields in
// config_bind.h; this only hides the FieldList parameter they'd otherwise
// all take directly.
//
// Deliberately backend-agnostic: this header names neither ptree nor
// toml::table, so which parser produced the FieldList a Configuration
// wraps is invisible here -- construct one with binding::from_ptree(pt) or
// binding::from_toml(tbl) (or both, e.g. layering an override file's
// FieldList on top isn't supported today, but nothing here would need to
// change to add it: it's still just a FieldList going into the same
// constructor).
// ============================================================================

namespace binding {

class Configuration {
public:
    explicit Configuration(FieldList fields) : fields_(std::move(fields)) {}

    // Reads one ad hoc scalar at a dot-separated path (e.g. "pool.size").
    // See get_leaf's own comment (config_bind.h) for the exact failure
    // modes -- a missing path, a non-leaf at the terminal segment, or a
    // value that doesn't parse as T all throw, naming the path.
    template <is_bindable_leaf T>
    T get(std::string_view path) const {
        return get_leaf<T>(fields_, path);
    }

    // Same, but returns std::nullopt instead of throwing when `path`
    // itself is absent. A present-but-malformed value still throws --
    // that's a real data error, not absence.
    template <is_bindable_leaf T>
    std::optional<T> try_get(std::string_view path) const {
        return try_get_leaf<T>(fields_, path);
    }

    // Binds the whole document onto a config_schema struct, matching each
    // field by name (case-insensitive), same rules as bind_from_fields.
    // Callable on a const Configuration you intend to keep using
    // afterward (e.g. bind() once, then get() a few more ad hoc values) --
    // this overload only reads fields_, never touches it.
    template <config_schema T>
    T bind(bool strict = false) const& {
        T out{};
        bind_from_fields(fields_, out, strict);
        return out;
    }

    template <config_schema T>
    void bind_into(T& out, bool strict = false) const& {
        bind_from_fields(fields_, out, strict);
    }

    // Rvalue-qualified counterparts -- for a Configuration this caller is
    // done with right after (binding it into the one struct it exists
    // for, e.g. Configuration(from_toml(parsed)).bind<AppConfig>()), these
    // route through bind_from_fields' FieldList&& overload instead, so a
    // std::string leaf value moves into the target struct field rather
    // than copying, all the way down (see config_bind.h). Calling get()/
    // try_get() on a Configuration after moving from it this way is legal
    // but reads back whatever bind_from_fields' moves left behind (empty
    // strings for any leaf that was actually moved) -- fine for a
    // Configuration about to be discarded anyway, same caveat any
    // moved-from object carries.
    template <config_schema T>
    T bind(bool strict = false) && {
        T out{};
        bind_from_fields(std::move(fields_), out, strict);
        return out;
    }

    template <config_schema T>
    void bind_into(T& out, bool strict = false) && {
        bind_from_fields(std::move(fields_), out, strict);
    }

private:
    FieldList fields_;
};

} // namespace binding
