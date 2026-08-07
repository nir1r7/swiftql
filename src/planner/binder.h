#pragma once

#include "parser/ast.h"
#include "catalog/catalog.h"
#include <vector>

// resolves table aliases and qualified/unqualified column references to a stable relation identity (the catalog table name)
// occurs before validation and planning run
// table existence errors are intentionally left to Validator (see bind()) so
// existing error messages for a missing FROM/JOIN table are unchanged
//
// Week 30 — NESTED SCOPES. A subquery opens a new query block with its own
// range table, so resolution is no longer a lookup in one vector: it walks OUT
// from the innermost block. A name that resolves at step k is stamped
// query_level = k, and k > 0 means the reference is CORRELATED.
class Binder {
    public:
        static void bind(SelectStatement& stmt, const Catalog& catalog);

    private:
        struct RangeEntry {
            std::string ref_name; // alias if present, else the table name
            std::string table_name; // canonical catalog table name
            const Schema* schema;
        };

        // One Scope per query block. `parent` is the LEXICALLY enclosing block,
        // nullptr at the top level, so resolution walks out exactly as SQL
        // scoping requires (inner shadows outer, and that is not an ambiguity).
        //
        // Slots are PER SCOPE: (query_level, relation_slot) is the identity, and
        // a slot read without its level compares two numbering domains. Global
        // numbering was rejected deliberately — ChunkPruner's `relation_slot < 1`
        // scan-local test, a leaf schema's slot-0 stamping and restampSlots' 0
        // are all per-scope facts, so one counter across all blocks would put an
        // inner query's leading relation at a non-zero slot and silently disable
        // chunk pruning inside every subquery.
        struct Scope {
            std::vector<RangeEntry> range_table;
            Scope* parent = nullptr;
            SelectStatement* stmt = nullptr;  // to set has_subquery
            bool correlated = false;          // some ref here resolved further out
        };

        // Binds one query block against `parent`. Returns true when the block
        // turned out to be correlated, which is what SubqueryExpr::correlated
        // records.
        static bool bindQuery(SelectStatement& stmt, const Catalog& catalog, Scope* parent);
        static void bindExpr(Expr* expr, Scope& scope, const Catalog& catalog);
        static void resolveColumnRef(ColumnRef* col, Scope& scope);
        // A ref resolving `level` scopes out makes THIS scope correlated, and
        // every scope between it and the resolving one: none of them can be
        // evaluated independently of the scope that supplies the value.
        static void markCorrelated(Scope& scope, int level);
        // SUM/AVG over a STRING column, for an argument that resolved to an
        // ENCLOSING block. Validator owns this check for every local argument
        // and cannot own it for a correlated one: it holds one statement, while
        // the column's type lives in a range table `query_level` blocks out.
        // The Binder is the only layer with that chain, so the check lands here.
        static void checkCorrelatedAggregateArg(const AggregateExpr* agg, const Scope& scope);
};
