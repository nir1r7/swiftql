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

    // return a ColumnDef's index by name
    int indexOf(const std::string& name) const;

    // has ColumnDef
    bool hasColumn(const std::string& name) const;

private:
    std::vector<ColumnDef> columns_;
};