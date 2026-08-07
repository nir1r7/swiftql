#pragma once

#include "common/schema.h"
#include <string>

// Week 35 — a data file's SHAPE is a property of the table, not of the caller.
//
// Before this, CSVLoader::load assumed both halves: it split on ',' and it ate
// line 1 unconditionally as a header. A TPC-H .tbl file has NEITHER — no header
// row, '|' as the delimiter, and each field TERMINATED rather than separated, so
// a correct split leaves one trailing empty field. The header assumption is the
// dangerous one: it silently discards the first real row of every table (region
// loses r_regionkey 0, and every aggregate is then wrong by a hair with nothing
// to show for it).
//
// Every default here IS the pre-Week-35 behaviour, so a catalog entry with no
// "format" object, and every tests/ fixture that calls CSVLoader::load without
// one, loads byte-identically.
struct FileFormat {
    char delimiter          = ',';
    bool has_header         = true;
    bool trailing_delimiter = false;

    static FileFormat csv() { return FileFormat{}; }
    static FileFormat tbl() { return FileFormat{'|', false, true}; }
};

struct TableMetadata {
    std::string name;
    std::string filepath;
    Schema schema;
    // Defaulted, so the F1 catalog.json and the ~20 CSVLoader::load call sites
    // in tests/ are untouched. main.cc is the ONE production loader call and it
    // passes this field — chosen deliberately over breaking every call site the
    // way Weeks 26 and 34 did for TableRef/joins, because here the site that
    // matters is a single line and 20 fixtures genuinely are CSV.
    FileFormat format {};
};
