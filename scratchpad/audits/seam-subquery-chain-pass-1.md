# Seam audit: subquery chain (weeks 30 -> 34), pass 1

Scope: the SEAM between week 30 (nested scopes / SubqueryExpr / ColumnRef::query_level),
week 31 (materialize-then-substitute), week 32 (IN/NOT IN -> semi/anti), week 33
(EXISTS decorrelation + ColumnId), week 34 (derived tables + correlated scalar).
Not a re-audit of any single week.

Targets:
1. Routing totality/disjointness across the four lowerings.
2. Scalar cardinality guarantee (week 31 "more than one row" error) surviving the week 34 rewrite.
3. NULL semantics: NOT IN three-valued, NOT EXISTS two-valued, COUNT-vs-SUM over empty group.
4. ColumnId containment -- no bare slot where qualified required, esp. derived-table RTEs.
5. Pass-pair asymmetry: one pass recurses into a construct, its sibling does not.

Status: IN PROGRESS

## Findings

(appended as confirmed)
