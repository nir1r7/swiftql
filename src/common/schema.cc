#include "type_id.h"
#include "schema.h"
#include <string>
#include <vector>
#include <stdexcept>

Schema::Schema(std::vector<ColumnDef> columns): columns_{std::move(columns)}{}

int Schema::size() const{
    return columns_.size();
}

const ColumnDef& Schema::column(int index) const {
    if (index < 0 || index >= size()){
        throw std::runtime_error("Index out of bounds");
    }

    return columns_[index];
}

const std::vector<ColumnDef>& Schema::columns() const {
    return columns_;
}

int Schema::indexOf(const std::string& name) const {
    for (int i = 0; i < size(); i++){
        if (columns_[i].name == name) return i;
    }
    return -1;
}

int Schema::indexOf(const std::string& name, int relation_slot) const {
    for (int i = 0; i < size(); i++){
        if (columns_[i].name == name && columns_[i].relation_slot == relation_slot) return i;
    }
    return -1;
}

bool Schema::hasColumn(const std::string& name) const {
    return indexOf(name) != -1;
}