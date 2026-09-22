#pragma once
// call_oci(): one generic wrapper around every raw OCI function call in
// this codebase, so error retrieval is written exactly once instead of
// once per call site.
//
// Every OCI function follows the same shape: it takes an OCIError*
// somewhere in its argument list and returns a sword status. call_oci
// finds that OCIError* itself (by scanning the actual argument types at
// compile time, not by asking the caller to name it separately -- it's
// already being passed to the underlying call anyway) and, whenever the
// call doesn't return OCI_SUCCESS, retrieves whatever detail OCIErrorGet
// has to offer. That covers all three of OCI_SUCCESS_WITH_INFO (there
// really is something to read -- ORA-28002 password-about-to-expire is
// the practical case), OCI_ERROR (an ORA-##### with real detail), and
// OCI_INVALID_HANDLE (may come back with nothing at all -- see this
// project's own docs/oci_statement_lifecycle_notes.md for why that
// specific code can't be trusted to have retrievable detail) -- call_oci
// doesn't need to special-case any of them differently: it just always
// asks, and the result carries whatever came back, empty or not.
//
// What call_oci deliberately does NOT do: decide whether a given status
// is "an error" for the call that produced it. OCI_NO_DATA means
// something different depending on which function returned it (end of
// a SELECT's rows vs. "position beyond OCIParamGet's actual column
// count"), and that interpretation belongs to whoever is calling
// call_oci for that specific operation -- OciStatement's execute()/
// fetch(), not this file. call_oci's contract is uniform and mechanical:
// run the call, and if the raw status isn't OCI_SUCCESS, attach whatever
// error text/code OCIErrorGet has. Nothing more.

#include "binding/oci_compat.h"

#include <array>
#include <string>
#include <type_traits>

namespace binding {

struct OciCallResult {
    sword status = OCI_SUCCESS;
    sb4 error_code = 0;
    std::string error_text; // only ever non-empty when status != OCI_SUCCESS,
                             // and only then if OCIErrorGet actually had something
};

namespace detail {

// Picks out the one argument (if any) whose type is OCIError* -- every
// OCI function takes exactly one, so "the first one found" is also "the
// only one there is." Passing an lvalue through here (rather than
// forwarding) is deliberate: call_oci below needs the *value* of the
// pointer, not a forwarding reference to whatever the caller passed --
// OCIError* is a plain pointer, trivially copied, no ownership question.
template <typename Arg>
OCIError* extract_error_handle(const Arg& arg) {
    if constexpr (std::is_same_v<std::decay_t<Arg>, OCIError*>) {
        return arg;
    } else {
        return nullptr;
    }
}

} // namespace detail

// F is any OCI function pointer (or anything callable the same way --
// a lambda wrapping one works too, useful for a call whose real name is
// hidden behind a macro like OCIParamGet/ocigparm). Args are forwarded
// to it exactly as given; call_oci never changes what gets passed to the
// underlying OCI call, it only observes the result afterward.
template <typename F, typename... Args>
OciCallResult call_oci(F&& func, Args&&... args) {
    const sword status = func(args...);
    OciCallResult result;
    result.status = status;
    if (status != OCI_SUCCESS) {
        OCIError* errhp = nullptr;
        ((errhp = errhp ? errhp : detail::extract_error_handle(args)), ...);
        if (errhp) {
            std::array<unsigned char, 512> buf{};
            OCIErrorGet(errhp, 1, nullptr, &result.error_code, buf.data(),
                        static_cast<ub4>(buf.size()), OCI_HTYPE_ERROR);
            result.error_text.assign(reinterpret_cast<const char*>(buf.data()));
        }
    }
    return result;
}

} // namespace binding
