#pragma once

#include "parser/ast.h"
#include "catalog/catalog.h"
#include <string>
#include <utility>
#include <vector>

struct LogicalPlanNode;   // validateJoinKeyTypes only; the .cc has logical_plan.h

class Validator {
    public:
        static void validate(const SelectStatement& stmt, const Catalog& catalog);

        // THE JOIN-KEY TYPE RULE, applied where all four JoinKey producers
        // converge: the finished logical plan. Called once at the end of
        // LogicalPlanBuilder::build, which every producer's node passes through.
        //
        // Seam audit pass 3, B3-2. Week 29 wrote this rule into
        // `Validator::validate`'s `for (stmt.joins)` loop, which is ONE of the
        // four things that produce a JoinKey. `subquery_lowering.cc`
        // (IN / NOT IN), `splitCorrelation` (EXISTS / NOT EXISTS) and the
        // correlated-scalar rewrite all shipped later and were uncovered, so the
        // text encoding decided and half-matched: on a zero-padded STRING id
        // against an INT, exactly the row whose text is already canonical
        // matched ('16' with 16, but not '016'), and the NOT forms returned the
        // complement. Both directions disagreed with SQLite, with no error.
        //
        // WHY A PLAN WALK AND NOT A CHECK AT EACH PRODUCER: writing it at the
        // producers is writing it four times, which is how it came to be missing
        // three times already. It also has to be here rather than at the
        // producers to be RIGHT: a semi/anti join resolves its right key
        // POSITIONALLY and a standard one BY NAME, and only the finished node
        // knows which — `semantics` is set after construction.
        //
        // The AST loop in validate() is KEPT, and is not a duplicate rule: it is
        // the same function below, called with the range table instead of a
        // plan, because Planner::plan (Volcano) builds a HashJoinNode straight
        // from `stmt.joins` and never builds a logical plan at all. It is the
        // only cover that path has. Both call sites raise one message.
        static void validateJoinKeyTypes(const LogicalPlanNode& plan);
    private:
        // Week 30. The body of validate(), which used to be everything except
        // the final "not yet executable" refusal — a refusal Week 33 deleted, so
        // the two now differ only in that validate() is the entry point. The
        // split is KEPT rather than collapsed: a NESTED query is validated
        // through validateQuery, and the distinction between "runs once for the
        // whole statement" and "runs per query block" is the shape any future
        // statement-level rule needs. There is no statement-level rule today.
        static void validateQuery(const SelectStatement& stmt, const Catalog& catalog);
        // `catalog` is needed since Week 30: a SubqueryExpr's body is a whole
        // query and is validated against its own FROM schema.
        // `allow_subqueries` defaults to FALSE — a new call site must opt in,
        // the same fail-closed discipline distribute() uses with `== INNER`.
        static void validateExpr(const Expr* expr, const Schema& schema, const std::string& context,
                                 const Catalog& catalog,
                                 bool allow_aggregates = true,
                                 bool allow_subqueries = false);
        // `relations` is (ref table name, schema) for every relation in the
        // query, in range-table order — the N-relation replacement for the
        // (left, right) pair Phase 4 passed.
        static void validateJoinCondition(const Expr* expr,
            const std::vector<std::pair<std::string, const Schema*>>& relations);
};