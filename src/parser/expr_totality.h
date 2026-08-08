#pragma once

#include "ast.h"
#include "expr_utils.h"        // aggregateOutputName
#include "common/schema.h"
#include <memory>
#include <vector>

// ── THE TOTALITY SCREEN — ONE FUNCTION, FOUR CONSUMERS ──────────────────────
//
// THE RULE THIS FILE SERVES (seam audit pass 4; see predicate_pushdown.cc for
// the full statement and the reproductions):
//
//   PER-ROW EVALUATION IS NOT TOTAL. `evaluate()` can THROW on a row. So the
//   SET OF ROWS an expression is evaluated on decides whether a query errors,
//   and SwiftQL fixes that set in the PLAN rather than leaving it to whichever
//   engine runs it:
//
//     * a conjunct of a filter is evaluated on the rows for which every
//       conjunct WRITTEN BEFORE IT evaluated TRUE (evaluatePredicate() in
//       evaluator.cc and evalPredicate() in columnar_eval.cc both implement
//       exactly this cascade);
//     * AN OUTER JOIN'S `ON` RESIDUAL is evaluated on every CANDIDATE PAIR the
//       join keys matched — not on rows of either input, and not on rows of the
//       join's output, since a pair the residual rejects never becomes one.
//       Both engines evaluate it EAGERLY over the whole residual conjunction, in
//       the probe loop, against the assembled merged row (plan_nodes.cc's
//       HashJoinNode::next and vec_hash_join_node.cc's probe both hold the same
//       `passes` lambda);
//     * every other expression is evaluated on every row that reaches its node.
//
//   THE SECOND BULLET IS NEW TEXT AND IT IS A CORRECTION, not an addition. This
//   file was written to be THE single statement of the rule and it had two
//   sentences that were false of the same construct: it enumerated three
//   consumers, and it said "every other expression is evaluated on every row
//   that reaches its node" — which an `ON` residual is not, in either engine.
//   Seam audit pass 5 (engine, A-1) found it by running the shape rather than
//   reading it: `LEFT JOIN ... ON k AND age > 30 AND SUBSTRING(name, age-30, 3)
//   = 'er_'` errors in all six modes, where the SAME two conjuncts written in a
//   WHERE answer in all six. That is not an engine divergence — the two agree —
//   but a reader who trusted this file would have concluded the residual was a
//   filter conjunct and inherited the cascade. It does not. An INNER join has no
//   such case: its residuals are folded into the WHERE conjunction, residuals
//   first and the user's WHERE after, by BOTH planners (logical_plan.cc and
//   planner.cc), so they ARE filter conjuncts and the first bullet governs them.
//
//   Nothing may change that set for an expression that CAN RAISE: not a plan
//   rewrite (predicate_pushdown.cc), not a storage-level chunk skip
//   (chunk_pruner.h), not an engine's choice of when to be lazy.
//
// `exprMayRaise` is what "CAN RAISE" means, in one place. It is a CONSERVATIVE
// over-approximation of "evaluate() can throw on some row": every answer of
// FALSE must be a proof, every uncertainty answers TRUE. The two errors are not
// symmetric — a false TRUE costs plan quality on that query, a false FALSE is a
// divergence — but they are not free either: pass 4's P4-2 measured 87x on a
// three-conjunct WHERE, caused entirely by screening arithmetic BY OPERATOR
// when the operand TYPES decide it. Hence the schema parameter. There is one
// screen and it is precise; there is no second, coarser copy anywhere.
//
// THE FOUR CONSUMERS, and why each needs the same answer:
//   1. predicate_pushdown.cc — a conjunct that may raise is frozen: it does not
//      move, and nothing may move across it.
//   2. chunk_pruner.h — a zone-map skip removes rows before the filter runs, so
//      only a conjunct written AHEAD of every conjunct that may raise is allowed
//      to prove a skip. IT MUST BE HANDED THE SCHEMA ITS HINT WAS WRITTEN
//      AGAINST, not the scanning relation's: staticTypeOf's ColumnRef arm falls
//      back to a bare name, so the wrong schema does not make it answer "I
//      cannot type this" — it makes it answer "I typed it", off a same-named
//      column of another relation. That is the one answer a conservative screen
//      must never give, and it was a live divergence in three seams at once
//      (pass 5's E-20 / S-13 / P5-2). See chunk_pruner.h.
//   3. logical_plan.cc — a LIMIT above a projection that may raise is placed
//      BELOW it, so the plan (not the engine's laziness) says how many rows the
//      projection is evaluated on.
//   4. an OUTER JOIN'S `ON` RESIDUAL — the consumer the enumeration used to
//      omit, together with the sentence that would have told a reader it did not
//      exist. Its row set is the key-matched candidate pairs, and BOTH engines
//      evaluate the whole residual eagerly over them, so the two agree today by
//      symmetry rather than by rule: nothing screens the residual and nothing
//      states which rows it is owed. That makes it a dialect fact rather than a
//      seam defect right now, and a rule this file states absolutely is not
//      allowed to have a construct it does not describe.
//
// Same dispatch as collectSlots / forEachLocalColumnRef in
// predicate_pushdown.cc. A MISSED Expr subtype here must answer TRUE, which the
// final `return true` delivers. Keep the three in lockstep.

// The static type of an expression, as far as `schema` can decide it. Returns
// false when it cannot be decided HERE — an unresolvable ColumnRef, an Expr
// subtype this walker does not know — and every caller reads that as "may
// raise". This is deliberately NOT inferExprType: that one THROWS on an
// ill-typed tree (which is right for its job, deciding a query at plan time)
// and lives in the planner, above two of the four consumers. This one is total
// and answers "don't know" instead.
inline bool staticTypeOf(const Expr* expr, const Schema& schema, TypeId& out) {
    if (!expr) return false;
    if (auto* lit = dynamic_cast<const Literal*>(expr)) {
        // A NULL literal never raises in a comparison (Value's operators return
        // false when either side is null, before the type check), but its
        // declared type is what a consumer would compare against, so report it.
        out = lit->value.isNull() ? lit->null_type : lit->value.type();
        return true;
    }
    if (auto* col = dynamic_cast<const ColumnRef*>(expr)) {
        // slot-first with bare-name fallback — the one resolution rule, shared
        // with evaluator.cc's resolveColumnIndex and inferExprType.
        int idx = (col->id.isResolved() && col->id.isLocal())
            ? schema.indexOf(col->column_name, col->id.localSlot("staticTypeOf"))
            : -1;
        if (idx < 0) idx = schema.indexOf(col->column_name);
        if (idx < 0) return false;
        out = schema.column(idx).type;
        return true;
    }
    if (auto* bin = dynamic_cast<const BinaryExpr*>(expr)) {
        const std::string& op = bin->op;
        if (op == "+" || op == "-" || op == "*" || op == "/") {
            TypeId l, r;
            if (!staticTypeOf(bin->left.get(), schema, l)) return false;
            if (!staticTypeOf(bin->right.get(), schema, r)) return false;
            if (l == TypeId::STRING || r == TypeId::STRING) return false;
            out = (l == TypeId::INT && r == TypeId::INT) ? TypeId::INT : TypeId::DOUBLE;
            return true;
        }
        out = TypeId::INT;   // comparison / AND / OR, boolean-as-INT
        return true;
    }
    if (auto* un = dynamic_cast<const UnaryExpr*>(expr)) {
        return staticTypeOf(un->operand.get(), schema, out);
    }
    if (dynamic_cast<const IsNullExpr*>(expr)
        || dynamic_cast<const InExpr*>(expr)
        || dynamic_cast<const LikeExpr*>(expr)) {
        out = TypeId::INT;
        return true;
    }
    if (dynamic_cast<const SubstringExpr*>(expr)) {
        out = TypeId::STRING;
        return true;
    }
    if (auto* agg = dynamic_cast<const AggregateExpr*>(expr)) {
        // A post-aggregate schema carries this aggregate's output column with
        // the type the aggregate node produced; that is the only place the type
        // is decided (buildAggregateSchema), and evaluate() reads that very
        // column rather than recomputing anything.
        int idx = schema.indexOf(aggregateOutputName(agg));
        if (idx < 0) return false;
        out = schema.column(idx).type;
        return true;
    }
    if (auto* c = dynamic_cast<const CaseExpr*>(expr)) {
        // Unify exactly as inferExprType does; any disagreement it would refuse
        // at plan time is reported here as "don't know" instead of thrown.
        if (c->when_clauses.empty()) return false;
        TypeId result;
        if (!staticTypeOf(c->when_clauses[0].result.get(), schema, result)) return false;
        for (size_t i = 1; i < c->when_clauses.size(); ++i) {
            TypeId t;
            if (!staticTypeOf(c->when_clauses[i].result.get(), schema, t)) return false;
            if (t == result) continue;
            if (t == TypeId::STRING || result == TypeId::STRING) return false;
            result = TypeId::DOUBLE;
        }
        if (c->else_expr) {
            TypeId t;
            if (!staticTypeOf(c->else_expr.get(), schema, t)) return false;
            if (t != result) {
                if (t == TypeId::STRING || result == TypeId::STRING) return false;
                result = TypeId::DOUBLE;
            }
        }
        out = result;
        return true;
    }
    return false;   // IntervalLiteral, SubqueryExpr, and anything added later
}

// True when `evaluate(expr, row, schema)` can throw for SOME row.
//
// Every branch below is an entry in the enumeration of `throw` sites reachable
// per row (evaluator.cc, common/value.cc, execution/checked_arith.h). The
// entries that answer FALSE are the ones whose throw is decided at PLAN time by
// inferExprType, in both legs, before any consumer of this file runs — and each
// says WHICH type fact makes that true, because pass 4 found the previous
// version of this screen resting on a four-item list of what inferExprType
// decides that was short by one, and the missing item (a comparison across the
// STRING boundary) was the one it does NOT decide.
inline bool exprMayRaise(const Expr* expr, const Schema& schema) {
    if (!expr) return false;
    if (dynamic_cast<const Literal*>(expr)) return false;
    if (auto* col = dynamic_cast<const ColumnRef*>(expr)) {
        // "Column not found in schema" (evaluator.cc:92). Not data-dependent,
        // but a throw on every row is still a throw that moving to a zero-row
        // position would mask.
        TypeId t;
        return !staticTypeOf(col, schema, t);
    }
    if (auto* bin = dynamic_cast<const BinaryExpr*>(expr)) {
        const std::string& op = bin->op;
        if (exprMayRaise(bin->left.get(), schema)
            || exprMayRaise(bin->right.get(), schema)) return true;
        TypeId l, r;
        if (!staticTypeOf(bin->left.get(), schema, l)) return true;
        if (!staticTypeOf(bin->right.get(), schema, r)) return true;
        if (op == "+" || op == "-" || op == "*" || op == "/") {
            // checkedAdd/Sub/Mul/Div (checked_arith.h) run ONLY on the INT/INT
            // branch (evaluator.cc:140-148); the DOUBLE branch is plain IEEE
            // arithmetic and cannot raise. Integer division by zero yields NULL,
            // and the one division that does throw (INT64_MIN / -1) is inside
            // the same INT/INT branch. A STRING operand is refused by
            // inferExprType at plan time, so it cannot reach a built plan — but
            // it is answered TRUE rather than argued about, since screening it
            // costs nothing.
            //
            // THIS IS THE FIX FOR P4-2's 87x. `l.speed * 2` with speed DOUBLE
            // took the double branch and could never overflow; the old screen
            // answered by OPERATOR and froze every conjunct written after it.
            if (l == TypeId::STRING || r == TypeId::STRING) return true;
            return l == TypeId::INT && r == TypeId::INT;
        }
        if (op == "AND" || op == "OR") {
            // asInt() on a non-INT operand raises std::bad_variant_access
            // (evaluator.cc:112-113, value.cc:28).
            return l != TypeId::INT || r != TypeId::INT;
        }
        // = != < > <= >= : Value's NUMERIC_COERCE (value.cc:52-58) coerces INT
        // against DOUBLE and THROWS "Type mismatch in Value comparison" across
        // the STRING boundary. inferExprType types a comparison as INT without
        // ever comparing its operands (logical_plan.cc's BinaryExpr branch), so
        // this is the ONE type error it does not decide at plan time, and it is
        // the whole of pass 4's P4-1 / P4-B2.
        return (l == TypeId::STRING) != (r == TypeId::STRING);
    }
    if (auto* un = dynamic_cast<const UnaryExpr*>(expr)) {
        if (exprMayRaise(un->operand.get(), schema)) return true;
        TypeId t;
        if (!staticTypeOf(un->operand.get(), schema, t)) return true;
        // checkedNegate on INT; the DOUBLE branch cannot raise; STRING throws
        // but inferExprType refuses it at plan time.
        return t != TypeId::DOUBLE;
    }
    if (auto* isn = dynamic_cast<const IsNullExpr*>(expr)) {
        return exprMayRaise(isn->operand.get(), schema);   // the operand IS evaluated
    }
    if (auto* in = dynamic_cast<const InExpr*>(expr)) {
        if (exprMayRaise(in->operand.get(), schema)) return true;
        TypeId t;
        if (!staticTypeOf(in->operand.get(), schema, t)) return true;
        // `v == c` throws across the STRING boundary exactly as a comparison
        // does. inferExprType already refuses a mixed list at plan time
        // (logical_plan.cc's InExpr branch); screened here too, for the same
        // zero cost as the arithmetic case. InExpr::values is vector<Value>, so
        // the list can hide no expression.
        for (const Value& c : in->values) {
            if (c.isNull()) continue;
            if ((c.type() == TypeId::STRING) != (t == TypeId::STRING)) return true;
        }
        return false;
    }
    if (auto* lk = dynamic_cast<const LikeExpr*>(expr)) {
        if (exprMayRaise(lk->operand.get(), schema)) return true;
        TypeId t;
        // "LIKE requires a STRING operand" (evaluator.cc:205). A column's type
        // is fixed by the schema, so this cannot vary per row — and
        // inferExprType refuses the non-STRING shape at plan time.
        return !staticTypeOf(lk->operand.get(), schema, t) || t != TypeId::STRING;
    }
    if (auto* c = dynamic_cast<const CaseExpr*>(expr)) {
        // evaluate() short-circuits an untaken branch, but ExpressionExecutor
        // declines to compile CaseExpr at all for exactly that reason, so which
        // branches run is an ENGINE detail. Screen every arm, and the conditions
        // for the asInt() the CASE itself performs (evaluator.cc:218).
        for (const auto& w : c->when_clauses) {
            if (exprMayRaise(w.condition.get(), schema)) return true;
            if (exprMayRaise(w.result.get(), schema)) return true;
            TypeId ct;
            if (!staticTypeOf(w.condition.get(), schema, ct) || ct != TypeId::INT) return true;
        }
        return exprMayRaise(c->else_expr.get(), schema);
    }
    if (auto* sub = dynamic_cast<const SubstringExpr*>(expr)) {
        if (exprMayRaise(sub->operand.get(), schema)
            || exprMayRaise(sub->start.get(), schema)
            || exprMayRaise(sub->length.get(), schema)) return true;
        TypeId t;
        if (!staticTypeOf(sub->operand.get(), schema, t) || t != TypeId::STRING) return true;
        // substringOf's two preconditions (evaluator.cc:61,63) are reachable
        // ONLY with a COMPUTED position: inferExprType refuses a constant
        // out-of-domain start or length at plan time, in both legs, and skips a
        // NULL literal (which evaluate() answers NULL for rather than raising).
        // That carve-out is what keeps TPC-H Q22's SUBSTRING(c_phone, 1, 2)
        // total.
        //
        // This tests Literal-ness in a position the user could have written an
        // expression in, downstream of foldConstants — the shape binder.cc's
        // folding census warns about. It is safe for the third reason the census
        // does not list: a folded literal start reaches here only after
        // inferExprType has already proven it in-domain, so the fold cannot turn
        // a raising SUBSTRING into a total one.
        const bool const_start = dynamic_cast<const Literal*>(sub->start.get()) != nullptr;
        const bool const_len = !sub->length
                             || dynamic_cast<const Literal*>(sub->length.get()) != nullptr;
        if (!const_start || !const_len) return true;
        TypeId st;
        if (!staticTypeOf(sub->start.get(), schema, st) || st != TypeId::INT) return true;
        if (sub->length) {
            TypeId lt;
            if (!staticTypeOf(sub->length.get(), schema, lt) || lt != TypeId::INT) return true;
        }
        return false;
    }
    if (auto* agg = dynamic_cast<const AggregateExpr*>(expr)) {
        // evaluate() does not re-evaluate the argument; it reads the aggregate's
        // pre-computed output column and throws only when that column is absent.
        TypeId t;
        return !staticTypeOf(agg, schema, t);
    }
    // IntervalLiteral and SubqueryExpr throw unconditionally in evaluate();
    // neither can reach a built plan (foldConstants removes the first,
    // materializeSubqueries the second). Screened rather than argued about.
    // Any Expr subtype added later lands here too, which is the safe direction.
    return true;
}

// A CONJUNCT is evaluated for its TRUTH VALUE as well as its value: every
// filter does `!v.isNull() && v.asInt() != 0` (plan_nodes.cc's FilterNode,
// columnar_eval.cc's evalFallback), and `asInt()` on a DOUBLE or STRING is
// std::bad_variant_access from value.cc:28. `WHERE speed` is the shape. So a
// conjunct whose static type is not INT can raise even when the expression
// itself is total.
inline bool conjunctMayRaise(const Expr* conjunct, const Schema& schema) {
    if (exprMayRaise(conjunct, schema)) return true;
    TypeId t;
    return !staticTypeOf(conjunct, schema, t) || t != TypeId::INT;
}

// Index of the first conjunct that can raise, or conjuncts.size() when none can.
// Everything from there on is FROZEN — see predicate_pushdown.cc's screen and
// ChunkPruner::shouldSkip, which apply the same index to two different
// mechanisms.
inline size_t firstMayRaise(const std::vector<std::unique_ptr<Expr>>& conjuncts,
                            const Schema& schema) {
    for (size_t i = 0; i < conjuncts.size(); ++i) {
        if (conjunctMayRaise(conjuncts[i].get(), schema)) return i;
    }
    return conjuncts.size();
}
