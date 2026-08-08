#pragma once

#include "type_id.h"
#include "value.h"
#include <string>
#include <vector>

// type alias
// just a vector of values
using Row = std::vector<Value>;

// public class
struct ColumnDef {
    std::string name;
    TypeId type;
    // Relation identity: the query's range-table position — 0 = FROM (the
    // default, so all single-relation schemas are slot 0 automatically), then
    // one slot per JOIN in written order (joins[i] -> i+1). Stamped on the
    // merged join schema so qualified column references resolve to the correct
    // relation even when several share a column name (incl. self-joins).
    int relation_slot = 0;
    // "in the plan but not in the query's output list": the column is computed
    // and flows through every operator normally, and only SELECT * synthesis
    // skips it. Nothing about RESOLUTION consults this flag — indexOf matches on
    // name (and slot) alone — so a hidden column is still readable by name from
    // any expression above it. TWO producers:
    //   1. aggregate outputs referenced only in HAVING / ORDER BY
    //      (extractAggregates), which are computed for those clauses only;
    //   2. the columns of the synthetic `$scalarN` relation a correlated scalar
    //      subquery is lowered to (subquery_decorrelation.cc) — the outer
    //      predicate reads the aggregate column by name, but the user wrote no
    //      such relation, so `*` must not expand over it. Seam audit pass 2,
    //      B-1: without this, `SELECT * FROM drivers d WHERE d.age > (SELECT
    //      COUNT(*) ...)` returned 7 columns where SQLite returns 5.
    // Read in exactly three places, and they must agree: the star expansion in
    // LogicalPlanBuilder::build, the one in Planner::plan, and step 3 of
    // blockOutputSchema (which is what makes a derived body's bound schema match
    // its planned one).
    bool hidden = false;
};

class Schema {
    public:
        explicit Schema(std::vector<ColumnDef> columns);

        // number of columns
        int size() const;

        // return a ColumnDef by index
        const ColumnDef& column(int index) const;

        // return all ColumnDef(s)
        const std::vector<ColumnDef>& columns() const;

        // return a ColumnDef's index by name (first match; slot-agnostic)
        int indexOf(const std::string& name) const;

        // return a ColumnDef's index by (relation_slot, name).
        // Used for qualified column resolution where a bare name may be
        // ambiguous across join sides. Returns -1 if no column matches both.
        int indexOf(const std::string& name, int relation_slot) const;

        // has ColumnDef
        bool hasColumn(const std::string& name) const;

    private:
        std::vector<ColumnDef> columns_;
};
// Render a column for a plan's explain string, qualified with its relation slot
// (`team@1`) only when the schema holds that name more than once.
//
// A merged join schema can carry `team` from several relations, and that is
// precisely where resolving a join key by name rather than by slot picks the
// wrong relation and still returns rows. A plan that prints `team = team` for
// both the correct and the incorrect resolution hides the defect on the surface
// used to debug it. Unambiguous names stay bare, so every pre-existing explain
// string is unchanged.
std::string qualifyIfAmbiguous(const Schema& schema, int index);
