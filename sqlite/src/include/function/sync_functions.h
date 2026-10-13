#pragma once

#include "function/table/table_function.h"

namespace lbug {
namespace sqlite_extension {

// CALL sqlite_sync('alias') -> one stats row per tick.
struct SqliteSyncFunction final {
    static constexpr const char* name = "sqlite_sync";

    static function::function_set getFunctionSet();
};

// CALL sqlite_register_node('alias', 'table'[, 'label'[, 'id_column']]).
struct SqliteRegisterNodeFunction final {
    static constexpr const char* name = "sqlite_register_node";

    static function::function_set getFunctionSet();
};

// CALL sqlite_register_edge('alias', 'table'[, 'label'[, 'from_col'[, 'to_col']]]).
struct SqliteRegisterEdgeFunction final {
    static constexpr const char* name = "sqlite_register_edge";

    static function::function_set getFunctionSet();
};

} // namespace sqlite_extension
} // namespace lbug
