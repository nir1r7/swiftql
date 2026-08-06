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
    // true for aggregate outputs referenced only in HAVING/ORDER BY: they are
    // computed and flow through Filter/Sort, but SELECT * synthesis skips them
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