#pragma once
// Mock stand-ins for Oracle's object/collection OCI API -- OCIType,
// OCIObjectNew, OCICollAppend, OCIBindObject, OCINumber conversions. Used
// only by oci_collection_bind.h, kept in its own file (rather than folded
// into oci_mock.h) so that file -- covering the scalar bind/define/reconnect
// path that's been checked against real OCI documentation fairly carefully
// -- stays untouched by this newer, less-verified addition.
//
// IMPORTANT: unlike oci_mock.h, the signatures here have NOT been
// cross-checked against a real oci.h/ociap.h -- there's no Oracle client on
// this machine to compare against, only documentation recall of Oracle's
// Object-Relational OCI API, which is a notoriously fiddly corner of OCI
// even with the real headers in hand. Treat every signature in this file as
// "best effort, needs verification against the actual client headers"
// before relying on it against a real database -- more so than anything
// else in this directory.
#include "binding/oci_compat.h"

#if !BINDING_HAS_REAL_OCI

#include <cstring>
#include <vector>

using uword = unsigned int;

struct OCIType;
struct OCIColl;

// Oracle's real OCINumber is a small fixed-size opaque buffer holding its
// internal decimal representation; the exact byte layout doesn't matter
// here since this mock only needs OCINumberFromInt/OCINumberToInt to round
// trip consistently with each other, not to match Oracle's actual format.
struct OCINumber { unsigned char data[22]; };

#define OCI_TYPECODE_NAMEDCOLLECTION 122
#define OCI_DURATION_SESSION 10
#define OCI_TYPEGET_HEADER 1
#define OCI_TYPEGET_ALL 2
#define OCI_NUMBER_SIGNED 2
#define OCI_OBJECTFREE_FORCE 1
#define SQLT_NTY 108

namespace binding::mock {

// Records what the last OCITypeByName lookup was asked for, so a demo can
// confirm the right collection type name was requested for a given ElemType.
inline std::string g_last_type_lookup_schema;
inline std::string g_last_type_lookup_name;

} // namespace binding::mock

extern "C" {

inline sword OCITypeByName(OCIEnv*, OCIError*, OCISvcCtx*,
                            const text* schema_name, ub4 s_length,
                            const text* type_name, ub4 t_length,
                            const text* /*version_name*/, ub4 /*v_length*/,
                            ub4 /*pin_duration*/, ub4 /*get_option*/,
                            OCIType** tdo) {
    binding::mock::g_last_type_lookup_schema.assign(
        reinterpret_cast<const char*>(schema_name), s_length);
    binding::mock::g_last_type_lookup_name.assign(
        reinterpret_cast<const char*>(type_name), t_length);
    *tdo = reinterpret_cast<OCIType*>(1); // non-null sentinel; never dereferenced
    return OCI_SUCCESS;
}

inline sword OCIObjectNew(OCIEnv*, OCIError*, const OCISvcCtx*,
                           ub2 /*typecode*/, OCIType*, dvoid* /*table*/,
                           ub4 /*duration*/, ub1 /*value*/, dvoid** instance) {
    // A real collection instance is a live OCIColl the server can append
    // into; the mock just needs *some* stable, freeable heap object so
    // OCICollAppend/OCIObjectFree below have somewhere to record state.
    *instance = new std::vector<std::string>();
    return OCI_SUCCESS;
}

inline sword OCINumberFromInt(OCIError*, const dvoid* inum, uword inum_length,
                               uword /*inum_s_flag*/, OCINumber* number) {
    std::memset(number->data, 0, sizeof(number->data));
    std::memcpy(number->data, inum, inum_length < sizeof(number->data) ? inum_length : sizeof(number->data));
    return OCI_SUCCESS;
}

inline sword OCICollAppend(OCIEnv*, OCIError*, const dvoid* elem, const dvoid* /*elemind*/, OCIColl* coll) {
    // The mock can't know here whether `elem` is an OCINumber* or a
    // NUL-terminated string -- collection_bind.h always passes something
    // it can safely round-trip to a debug string, since that's all this
    // mock's OCIStmtExecute/OCIStmtFetch2 need to demonstrate the flow.
    auto* values = reinterpret_cast<std::vector<std::string>*>(coll);
    values->push_back(reinterpret_cast<const char*>(elem));
    return OCI_SUCCESS;
}

inline sword OCIBindObject(OCIBind*, OCIError*, const OCIType*, dvoid** /*pgvpp*/,
                            ub4* /*pvlpp*/, dvoid** /*indpp*/, ub4* /*indpp_size*/) {
    return OCI_SUCCESS;
}

inline sword OCIObjectFree(OCIEnv*, OCIError*, dvoid* instance, ub2 /*flags*/) {
    delete reinterpret_cast<std::vector<std::string>*>(instance);
    return OCI_SUCCESS;
}

} // extern "C"

#endif // !BINDING_HAS_REAL_OCI
