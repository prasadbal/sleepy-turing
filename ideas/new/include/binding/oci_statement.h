#pragma once
// OciStatement: a single statement handle with an explicit, enforced
// lifecycle -- the thing ideas/binding's free-function design never had,
// and whose absence produced real, confusing errors while working
// through this (ORA-24437 "statement handle not prepared" chief among
// them -- using a handle in a state it wasn't actually in).
//
//   Unprepared --prepare()--> Prepared --execute()--> Executed --fetch()--> Executed --fetch()--> ... EndOfFetch
//                                 |                    ^    |
//                             bindName()/bindOutput()  |  (iters>0 with zero
//                             valid in either           |  matching rows goes
//                             Prepared or Executed ------  straight to EndOfFetch)
//
// Executed covers both "just executed, ready to fetch" and "mid-batch-
// fetch-loop, more rows still available" -- real OCI doesn't distinguish
// those two either. OCI_ATTR_STMT_STATE (see below) only ever reports
// three values -- INITIALIZED, EXECUTED, END_OF_FETCH -- so Executed here
// is the honest reflection of that, not a state this class invented and
// then collapsed.
//
// The Prepared<->Executed cycle (bind, execute, rebind, execute again,
// never re-preparing) is deliberate, not a loophole: a chunked
// bindNameArray() insert (see below) runs exactly that loop, the same
// "one OCIStmtPrepare, then bind+execute per chunk" shape
// ideas/binding's insert_rows() uses. bindOutput() is allowed there too,
// for the opposite reason -- describeColumnPosition() needs execute()
// to have already run before a name can resolve to a position.
//
// Using a method in a state it doesn't support throws
// OciStatementStateError rather than making the underlying OCI call at
// all -- a deliberate choice, not a formality: this is a programmer
// error (a wrong call sequence, always avoidable by checking state()
// first or simply calling things in order), not a runtime/data outcome
// like a bad query or a lost connection -- which is exactly the
// distinction ExecResult already exists to carry for those. Folding
// "you called fetch() before execute()" into the same Success/
// ConnectionLost/QueryError vocabulary a real database outcome uses
// would blur a bug in the calling code together with things every
// caller has to handle as a matter of course.
//
// bindName()/bindOutput() are deliberately type-erased (an OCI type code
// + a raw pointer + a length, not a C++ type inferred via reflection the
// way ideas/binding's bind_one_param does) -- this is the lower layer
// something like that could be rebuilt on top of, not a replacement for
// it. See docs/oci_statement_lifecycle_notes.md for why column *names*
// specifically (OCIParamGet/OCI_ATTR_NAME) aren't available until after
// execute(), not just after prepare() -- bindOutput() here is positional
// only, which is why it has no such restriction and can be called
// between prepare() and execute() like bindName().

#include "binding/oci_call.h"
#include "binding/oci_connection.h"
#include "binding/oci_handle_guard.h"
#include "binding/oci_log.h"

#include <cctype>
#include <stdexcept>
#include <string>
#include <vector>

namespace binding {

// One result-set column's real, described metadata -- name, 1-based
// OCIDefineByPos position, the OCI type code Oracle itself reports for
// the column's *native* storage (SQLT_NUM for a NUMBER column, not
// whatever external type a caller might define it as -- see
// OciStatement::describeColumns()'s own comment), and its maximum size
// in bytes (OCI_ATTR_DATA_SIZE).
struct ColumnInfo {
    std::string name;
    ub4 position = 0;
    ub2 oracle_type = 0;
    ub4 data_size = 0;
};

class OciStatementStateError : public std::logic_error {
public:
    using std::logic_error::logic_error;
};

class OciStatement {
public:
    // Unprepared/Prepared are tracked here, not by OCI -- OCI_ATTR_STMT_STATE
    // reports the same INITIALIZED value both before prepare() and after
    // it, up until the first execute(), so there's no attribute to read
    // that distinction from. Executed/EndOfFetch, in contrast, are read
    // directly from OCI_ATTR_STMT_STATE after every execute()/fetch()
    // call (see sync_state_from_oci() below) rather than re-derived from
    // the call's own return status -- OCI already knows the handle's real
    // state; asking it directly is more honest than inferring the same
    // answer a second time from OCI_NO_DATA.
    enum class State { Unprepared, Prepared, Executed, EndOfFetch };

    explicit OciStatement(OciConnection& conn) : conn_(conn), handle_(conn.env()) {}

    void prepare(const std::string& sql) {
        require_state(State::Unprepared, "prepare");
        call_oci(OCIStmtPrepare, handle_.get(), conn_.err(),
                 reinterpret_cast<const text*>(sql.c_str()), static_cast<ub4>(sql.size()),
                 static_cast<ub4>(OCI_NTV_SYNTAX), static_cast<ub4>(OCI_DEFAULT));
        state_ = State::Prepared;
        sql_ = sql;
        params_log_.clear();
    }

    // Input parameter, by name. data_type is the raw OCI external type
    // code (SQLT_INT, SQLT_CHR, ...) -- the caller's job to get right,
    // same as ideas/binding's OciTypeBinder<T> does automatically per
    // C++ type; there's no reflection layer here to infer it.
    OciCallResult bindName(const std::string& field_name, ub2 data_type, void* data, sb4 len,
                           void* indicator = nullptr) {
        if (state_ != State::Prepared && state_ != State::Executed) {
            throw OciStatementStateError(
                "OciStatement::bindName(): statement is " + state_name(state_) +
                ", expected Prepared or Executed");
        }
        if (statement_logger()) {
            const bool is_null = indicator && *static_cast<const sb2*>(indicator) == OCI_IND_NULL;
            if (!params_log_.empty()) params_log_ += ", ";
            params_log_ += field_name + "=" + (is_null ? "NULL" : render_typed_value(conn_, data_type, data, len));
        }
        const std::string placeholder = ":" + field_name;
        OCIBind* bind_handle = nullptr;
        return call_oci(OCIBindByName, handle_.get(), &bind_handle, conn_.err(),
                        reinterpret_cast<const text*>(placeholder.c_str()), static_cast<sb4>(placeholder.size()),
                        data, len, data_type, indicator,
                        static_cast<ub2*>(nullptr), static_cast<ub2*>(nullptr),
                        static_cast<ub4>(0), static_cast<ub4*>(nullptr), static_cast<ub4>(OCI_DEFAULT));
    }

    // A chunked array bind: `first_element` is the address of the
    // *first* row's value in this chunk (not row 0 of the whole
    // vector -- callers advance this themselves between chunks, the
    // same "rebind fresh per chunk, pointed at that chunk's own start"
    // pattern ideas/binding's insert_rows() uses, not OCIStmtExecute's
    // own rowoff parameter -- see docs/oci_statement_lifecycle_notes.md
    // and ideas/binding's README for why rowoff specifically crashed
    // against a real database and isn't used anywhere in this codebase
    // either). `stride` is the byte distance from one row's value to the
    // next (`sizeof(RowStruct)` for an array-of-struct layout).
    // `indicators` must be a real, caller-owned array of `count`
    // `sb2` values (typically all `OCI_IND_NOTNULL`) that outlives the
    // following execute() call -- a null indicator pointer crashed
    // against a real database for an array bind specifically (safe for
    // bindName()'s single-row case, safe for bindOutput()'s array
    // fetch, not safe here), confirmed while building ideas/binding.
    //
    // alenp/alskip are for a variable-length field within a fixed-size
    // buffer -- a FixedString<N> row, whose real content length lives in
    // its own length_ref(), one whole row apart from the next row's (the
    // same alskip = sizeof(RowStruct) ideas/binding's own array-bind path
    // uses). Left at their defaults (nullptr/0) for a field whose bind
    // size already equals its real length, every other field type here.
    OciCallResult bindNameArray(const std::string& field_name, ub2 data_type, void* first_element,
                                sb4 elem_size, sb4 stride, sb2* indicators,
                                ub2* alenp = nullptr, ub4 alskip = 0) {
        if (state_ != State::Prepared && state_ != State::Executed) {
            throw OciStatementStateError(
                "OciStatement::bindNameArray(): statement is " + state_name(state_) +
                ", expected Prepared or Executed");
        }
        const std::string placeholder = ":" + field_name;
        OCIBind* bind_handle = nullptr;
        OciCallResult result = call_oci(OCIBindByName, handle_.get(), &bind_handle, conn_.err(),
                                        reinterpret_cast<const text*>(placeholder.c_str()), static_cast<sb4>(placeholder.size()),
                                        first_element, elem_size, data_type, indicators,
                                        alenp, static_cast<ub2*>(nullptr),
                                        static_cast<ub4>(0), static_cast<ub4*>(nullptr), static_cast<ub4>(OCI_DEFAULT));
        if (bind_handle) {
            OCIBindArrayOfStruct(bind_handle, conn_.err(), static_cast<ub4>(stride),
                                 static_cast<ub4>(sizeof(sb2)), static_cast<ub4>(alskip), 0);
        }
        return result;
    }

    // Output column, by position. elemSize > 0 additionally calls
    // OCIDefineArrayOfStruct with that as the row-to-row stride (pvskip)
    // -- pass sizeof(RowType) for an array-of-struct batch fetch, leave
    // it 0 (the default) for a single-row define, where no stride is
    // needed at all.
    //
    // Also valid in EndOfFetch, not just Prepared/Executed -- a real
    // finding, not a theoretical one: with prefetch_rows set higher than
    // the real row count, real Oracle can report OCI_ATTR_STMT_STATE ==
    // END_OF_FETCH from execute(0) itself, before this class's own
    // bindOutput() has even been called once to set up the output
    // columns -- prefetch runs during execute() regardless of iters, so
    // a small result set can already be "done" as far as the server is
    // concerned before a caller has defined anywhere to put the data.
    // Confirmed by a real select_rows()-shaped caller in
    // ideas/new/examples/live_oracle_client_demo.cpp throwing here the
    // first time this ran against a live database with only 2 real rows
    // and prefetch_rows=10 -- see docs/oci_statement_lifecycle_notes.md.
    // rlskip is the stride between one row's outsize/rlenp entry and the
    // next -- left at 0 (the default) to mean "same as elemSize," which
    // is correct whenever outsize lives inside the same per-row struct
    // as the value itself (FixedString<N>::length_ref(), one whole row
    // apart, same as pvskip) -- true of every caller before
    // select_generic() (details/oci_client.h). select_generic() passes a
    // real, different rlskip because its length array is its own
    // tightly-packed vector<ub2>, not embedded in a stride-elemSize
    // struct -- sizeof(ub2), not elemSize, is the real distance from one
    // row's reported length to the next there.
    OciCallResult bindOutput(std::size_t pos, ub2 data_type, void* data, sb4 len,
                             ub2* outsize, void* indicator, sb4 elemSize = 0, ub4 rlskip = 0) {
        if (state_ != State::Prepared && state_ != State::Executed && state_ != State::EndOfFetch) {
            throw OciStatementStateError(
                "OciStatement::bindOutput(): statement is " + state_name(state_) +
                ", expected Prepared, Executed, or EndOfFetch");
        }
        OCIDefine* define_handle = nullptr;
        OciCallResult result = call_oci(OCIDefineByPos, handle_.get(), &define_handle, conn_.err(),
                                        static_cast<ub4>(pos), data, len, data_type,
                                        indicator, outsize, static_cast<ub2*>(nullptr), static_cast<ub4>(OCI_DEFAULT));
        if (elemSize > 0 && define_handle) {
            OCIDefineArrayOfStruct(define_handle, conn_.err(),
                                   static_cast<ub4>(elemSize), static_cast<ub4>(sizeof(sb2)),
                                   rlskip > 0 ? rlskip : static_cast<ub4>(elemSize), 0);
        }
        return result;
    }

    void set_prefetch_rows(ub4 rows) {
        require_state(State::Prepared, "set_prefetch_rows");
        OCIAttrSet(handle_.get(), OCI_HTYPE_STMT, &rows, 0, OCI_ATTR_PREFETCH_ROWS, conn_.err());
    }

    // Resolves a column's real, fully-resolved name (through any CTE/
    // subquery/UNION/expression alias) to its 1-based OCIDefineByPos
    // position, via OCIParamGet/OCIAttrGet(OCI_ATTR_NAME) -- the same
    // mechanism ideas/binding's select_rows() uses for by-name matching,
    // here exposed directly rather than hidden behind reflection. Only
    // valid after execute() -- see docs/oci_statement_lifecycle_notes.md
    // for why describe info isn't available any earlier, confirmed
    // empirically against a real database while building that feature.
    // Returns 0 if no column matches (case-insensitively) -- callers
    // should treat that as an error, not silently bind position 0. Also
    // returns 0 uniformly against a backend that can't describe columns
    // at all (the mock, which has no notion of a query's actual column
    // names) -- there's no way to distinguish "genuinely no such column"
    // from "this backend can't tell me" from the return value alone; the
    // mock is documented as a call-shape simulator for exactly this
    // reason (see docs/oci_statement_lifecycle_notes.md and
    // ideas/binding's own oci_mock.h).
    ub4 describeColumnPosition(const std::string& column_name) const {
        if (state_ != State::Executed && state_ != State::EndOfFetch) {
            throw OciStatementStateError(
                "OciStatement::describeColumnPosition(): statement is " + state_name(state_) +
                ", expected Executed -- call execute() first");
        }
        ub4 column_count = 0;
        ub4 attr_size = sizeof(column_count);
        OCIAttrGet(handle_.get(), OCI_HTYPE_STMT, &column_count, &attr_size, OCI_ATTR_PARAM_COUNT, conn_.err());

        const std::string target = uppercased(column_name);
        for (ub4 pos = 1; pos <= column_count; ++pos) {
            void* parmdp = nullptr;
            OCIParamGet(handle_.get(), OCI_HTYPE_STMT, conn_.err(), &parmdp, pos);
            text* name_ptr = nullptr;
            ub4 name_len = 0;
            OCIAttrGet(parmdp, OCI_DTYPE_PARAM, &name_ptr, &name_len, OCI_ATTR_NAME, conn_.err());
            const std::string name(reinterpret_cast<const char*>(name_ptr), name_len);
            OCIDescriptorFree(parmdp, OCI_DTYPE_PARAM);
            if (uppercased(name) == target) return pos;
        }
        return 0;
    }

    // Every column's real, described metadata in one pass -- for a
    // caller that doesn't know (or doesn't want to declare) a row struct
    // ahead of time, e.g. a generic "run arbitrary SQL and see what's
    // there" tool, or select_generic() below. Same OCIParamGet loop and
    // same restrictions as describeColumnPosition() (only valid after
    // execute(), returns an empty vector against a backend that can't
    // describe at all -- the mock's OCI_ATTR_PARAM_COUNT always reports
    // 0), just returning everything about every column instead of one
    // position for one named column.
    //
    // oracle_type is the column's *native* SQLT_* code as Oracle itself
    // describes it -- SQLT_NUM for a NUMBER column, not SQLT_INT/
    // SQLT_BDOUBLE the way a caller binding it explicitly would choose.
    // Defining a column using its own described type/size, unconverted,
    // is exactly what select_generic() does below -- the fastest
    // possible generic fetch path, since there is no representation
    // change between what Oracle hands back and what a caller receives.
    std::vector<ColumnInfo> describeColumns() const {
        if (state_ != State::Executed && state_ != State::EndOfFetch) {
            throw OciStatementStateError(
                "OciStatement::describeColumns(): statement is " + state_name(state_) +
                ", expected Executed -- call execute() first");
        }
        ub4 column_count = 0;
        ub4 attr_size = sizeof(column_count);
        OCIAttrGet(handle_.get(), OCI_HTYPE_STMT, &column_count, &attr_size, OCI_ATTR_PARAM_COUNT, conn_.err());

        std::vector<ColumnInfo> columns;
        columns.reserve(column_count);
        for (ub4 pos = 1; pos <= column_count; ++pos) {
            void* parmdp = nullptr;
            OCIParamGet(handle_.get(), OCI_HTYPE_STMT, conn_.err(), &parmdp, pos);

            ColumnInfo col;
            col.position = pos;

            text* name_ptr = nullptr;
            ub4 name_len = 0;
            OCIAttrGet(parmdp, OCI_DTYPE_PARAM, &name_ptr, &name_len, OCI_ATTR_NAME, conn_.err());
            col.name.assign(reinterpret_cast<const char*>(name_ptr), name_len);

            ub2 data_type = 0;
            attr_size = sizeof(data_type);
            OCIAttrGet(parmdp, OCI_DTYPE_PARAM, &data_type, &attr_size, OCI_ATTR_DATA_TYPE, conn_.err());
            col.oracle_type = data_type;

            ub2 data_size = 0;
            attr_size = sizeof(data_size);
            OCIAttrGet(parmdp, OCI_DTYPE_PARAM, &data_size, &attr_size, OCI_ATTR_DATA_SIZE, conn_.err());
            col.data_size = data_size;

            OCIDescriptorFree(parmdp, OCI_DTYPE_PARAM);
            columns.push_back(std::move(col));
        }
        return columns;
    }

    // iters=0 for a query you're about to fetch from in a loop (also
    // what actually resolves any DESCRIBE information, if a caller
    // layers name-based column matching on top of this -- see
    // docs/oci_statement_lifecycle_notes.md for why that has to happen
    // here, not before); iters>0 to also fetch that many rows as part of
    // this same call. Zero matching rows (iters>0) transitions straight
    // to EndOfFetch, classified Success -- not a failure, see
    // OciConnection::classify.
    ExecResult execute(ub4 iters = 0) {
        if (state_ != State::Prepared && state_ != State::Executed) {
            throw OciStatementStateError(
                "OciStatement::execute(): statement is " + state_name(state_) +
                ", expected Prepared or Executed");
        }
        if (StatementLogger& logger = statement_logger(); logger) {
            logger(params_log_.empty() ? ("SQL: " + sql_) : ("SQL: " + sql_ + " | " + params_log_));
        }
        const OciCallResult call = call_oci(OCIStmtExecute, conn_.svc(), handle_.get(), conn_.err(),
                                            iters, static_cast<ub4>(0),
                                            nullptr, nullptr,
                                            static_cast<ub4>(OCI_DEFAULT));
        sync_state_from_oci();
        return {conn_.classify(call), call};
    }

    // Deliberately valid in EndOfFetch too, not just Executed: with
    // set_prefetch_rows() set higher than the real row count, real Oracle
    // reports OCI_ATTR_STMT_STATE == END_OF_FETCH immediately after
    // execute() -- before this class's own fetch() has ever run -- once
    // the server-side cursor has no more rows to send. That's a
    // statement-level fact about the SERVER, not about whether this
    // class has drained its own prefetch buffer into the caller's bind
    // variables yet; the prefetched rows still need at least one real
    // fetch() call to actually land in them. Confirmed against a real
    // database (see docs/oci_statement_lifecycle_notes.md): a fetch()
    // called while already EndOfFetch is exactly what a prefetching
    // batch-fetch loop's very next iteration does, and it works -- it
    // either returns real rows still sitting in the prefetch cache, or
    // OCI_NO_DATA with zero new rows if the cache was already drained.
    ExecResult fetch(ub4 nrows) {
        if (state_ != State::Executed && state_ != State::EndOfFetch) {
            throw OciStatementStateError(
                "OciStatement::fetch(): statement is " + state_name(state_) +
                ", expected Executed or EndOfFetch -- call execute() first");
        }
        const OciCallResult call = call_oci(OCIStmtFetch2, handle_.get(), conn_.err(),
                                            nrows, static_cast<ub2>(OCI_FETCH_NEXT),
                                            static_cast<sb4>(0), static_cast<ub4>(OCI_DEFAULT));
        sync_state_from_oci();
        return {conn_.classify(call), call};
    }

    // Valid any time after execute()/fetch() has actually run -- callers
    // fetching in a batch loop call this once per fetch() to find out how
    // many of that batch's rows are real (the last batch of a result set
    // is usually partial).
    ub4 rows_fetched() const {
        ub4 count = 0;
        ub4 size = sizeof(count);
        OCIAttrGet(handle_.get(), OCI_HTYPE_STMT, &count, &size, OCI_ATTR_ROWS_FETCHED, conn_.err());
        return count;
    }

    State state() const noexcept { return state_; }
    OCIStmt* handle() const noexcept { return handle_.get(); }

private:
    void require_state(State expected, const char* op) const {
        if (state_ != expected) {
            throw OciStatementStateError(
                std::string("OciStatement::") + op + "(): statement is " + state_name(state_) +
                ", expected " + state_name(expected));
        }
    }

    // Reads OCI's own OCI_ATTR_STMT_STATE and folds it into state_,
    // called after every execute()/fetch(). OCI only ever reports
    // INITIALIZED/EXECUTED/END_OF_FETCH; INITIALIZED is left unmapped
    // here on purpose -- it shouldn't occur after a call that already
    // required Prepared/Executed/Executed-only to run at all, and if it
    // somehow did, silently forcing state_ back to Prepared would hide
    // a real problem more than it would help.
    void sync_state_from_oci() {
        ub4 oci_state = 0;
        ub4 size = sizeof(oci_state);
        OCIAttrGet(handle_.get(), OCI_HTYPE_STMT, &oci_state, &size, OCI_ATTR_STMT_STATE, conn_.err());
        if (oci_state == OCI_STMT_STATE_EXECUTED) {
            state_ = State::Executed;
        } else if (oci_state == OCI_STMT_STATE_END_OF_FETCH) {
            state_ = State::EndOfFetch;
        }
    }

    static std::string uppercased(std::string_view sv) {
        std::string result(sv);
        for (char& c : result) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        return result;
    }

    static std::string state_name(State s) {
        switch (s) {
            case State::Unprepared: return "Unprepared";
            case State::Prepared:   return "Prepared";
            case State::Executed:   return "Executed";
            case State::EndOfFetch: return "EndOfFetch";
        }
        return "?";
    }

    OciConnection& conn_;
    OCIStmtHandle handle_;
    State state_ = State::Unprepared;
    std::string sql_;
    std::string params_log_;
};

} // namespace binding
