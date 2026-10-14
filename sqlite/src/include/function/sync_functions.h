#pragma once

#include "function/table/table_function.h"

namespace lbug {
namespace sqlite_extension {

// CALL sqlite_sync('/path/to/file.db') -> one stats row per tick.
struct SqliteSyncFunction final {
    static constexpr const char* name = "sqlite_sync";

    static function::function_set getFunctionSet();
};

// CALL sqlite_register_node('/path/to/file.db', 'table'[, 'label'[, 'id_column']]).
struct SqliteRegisterNodeFunction final {
    static constexpr const char* name = "sqlite_register_node";

    static function::function_set getFunctionSet();
};

// CALL sqlite_register_edge('/path/to/file.db', 'table'[, 'label'[, 'from_col'[, 'to_col']]]).
struct SqliteRegisterEdgeFunction final {
    static constexpr const char* name = "sqlite_register_edge";

    static function::function_set getFunctionSet();
};

// CALL sqlite_sync_start('/path/to/file.db'[, interval_ms]).
struct SqliteSyncStartFunction final {
    static constexpr const char* name = "sqlite_sync_start";

    static function::function_set getFunctionSet();
};

// CALL sqlite_sync_stop('/path/to/file.db').
struct SqliteSyncStopFunction final {
    static constexpr const char* name = "sqlite_sync_stop";

    static function::function_set getFunctionSet();
};

// CALL sqlite_sync_status() (one row per daemon).
struct SqliteSyncStatusFunction final {
    static constexpr const char* name = "sqlite_sync_status";

    static function::function_set getFunctionSet();
};

} // namespace sqlite_extension
} // namespace lbug
