#pragma once

#include "columnar_table.h"
#include "common/schema.h"
#include "common/value.h"
#include <vector>

class CSVToColumnar {
    public:
        // transpose already loaded rows
        // load rows using CSVLoader::load()
        static ColumnarTable convert(const std::vector<Row>& rows, const Schema& schema);
};