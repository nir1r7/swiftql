#pragma once

#include "parser/ast.h"
#include "catalog/catalog.h"
#include <memory>
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
            // Week 34. Stable storage for a DERIVED entry's schema. A catalog
            // table's Schema outlives the query, so RangeEntry can hold a raw
            // pointer at it; a derived table's is computed here and must be
            // OWNED. unique_ptr, never vector<Schema>: growing the vector moves
            // the POINTERS, not the Schema objects, so every earlier entry stays
            // valid — with `vector<Schema>` a reallocation on the SECOND derived
            // table in one block would dangle every earlier entry, and only then.
            //
            // LIFETIME, traced end to end (Week 34 audit round 1 left this
            // unreached, so it is written down rather than assumed):
            //   - a Scope is a stack local of bindQuery and nothing stores a
            //     Scope* beyond that frame; resolveColumnRef and
            //     checkCorrelatedAggregateArg walk the `parent` chain, and every
            //     link in it is a caller's live frame;
            //   - RangeEntry::schema is read only through range_table, which is a
            //     member of the same Scope, so it cannot outlive its owner;
            //   - a DERIVED body's own Scope dies inside relationSchema, but the
            //     schema handed back was pushed onto the ENCLOSING scope's
            //     owned_schemas, which outlives it. A nested derived table
            //     therefore stores its schema one level out from where it was
            //     computed, deliberately;
            //   - a ColumnRef keeps only a ColumnId (two ints), never a pointer,
            //     so nothing survives binding at all.
            // Exercised: two derived relations in one FROM/JOIN list (the
            // reallocation case) and a derived table nested inside another.
            std::vector<std::unique_ptr<Schema>> owned_schemas;
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
        // Week 34. The schema of one FROM/JOIN relation — a catalog table's, or
        // a DERIVED TABLE's, in which case the body is bound here first.
        //
        // !! `parent`, not `&scope`. A derived table is NOT LATERAL: its body may
        // not reference the enclosing query's own FROM items. Binding it against
        // the block's PARENT makes sibling relations invisible by construction —
        // the argument IS the scoping rule, there is no separate check for it —
        // and a reference reaching further out still marks the body correlated,
        // which is refused by name. SwiftQL has no dependent-join operator to run
        // a lateral join on, which is the same reason Week 33 refuses rather than
        // falling back.
        static const Schema* relationSchema(TableRef& ref, Scope& scope,
                                            const Catalog& catalog, Scope* parent);
};
