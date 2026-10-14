#include "function/sync_functions.h"

#include <mutex>

#include "binder/binder.h"
#include "common/exception/runtime.h"
#include "common/types/types.h"
#include "function/sync_daemon.h"
#include "function/sync_engine.h"
#include "function/table/bind_data.h"
#include "function/table/bind_input.h"
#include "function/table/table_function.h"
#include "main/client_context.h"
#include "main/database.h"
#include "processor/execution_context.h"

using namespace lbug::function;
using namespace lbug::common;

namespace lbug {
namespace sqlite_extension {

namespace {

struct SyncArgsBindData : TableFuncBindData {
    // Positional literal args of the CALL.
    std::vector<std::string> args;

    // Optional integer arg (e.g. sync interval ms); set by bind funcs
    // that declare one, since initSharedState only sees bind data.
    int64_t intArg = 0;
    bool hasIntArg = false;

    explicit SyncArgsBindData(binder::expression_vector columns, std::vector<std::string> args)
        : TableFuncBindData{std::move(columns), 1}, args{std::move(args)} {}

    SyncArgsBindData(const SyncArgsBindData& other)
        : TableFuncBindData{other}, args{other.args}, intArg{other.intArg},
          hasIntArg{other.hasIntArg} {}

    std::unique_ptr<TableFuncBindData> copy() const override {
        return std::make_unique<SyncArgsBindData>(*this);
    }
};

struct SyncSharedState : TableFuncSharedState {
    bool done = false;
    SyncStats stats;
    std::mutex mtx;

    SyncSharedState() : TableFuncSharedState{1} {}
};

std::vector<std::string> readArgs(const TableFuncBindInput* input, size_t maxArgs) {
    std::vector<std::string> args;
    for (size_t i = 0; i < input->params.size() && i < maxArgs; i++) {
        args.push_back(input->getLiteralVal<std::string>(i));
    }
    return args;
}

std::string optionalArg(const std::vector<std::string>& args, size_t idx) {
    return idx < args.size() ? args[idx] : "";
}

// Runs the engine call and stores stats; throws with the engine error.
void runEngine(const TableFuncInitSharedStateInput& input,
    std::function<SyncResult(SyncEngine&, main::ClientContext*, const std::vector<std::string>&)>
        fn,
    SyncSharedState& state) {
    auto bindData = input.bindData->constPtrCast<SyncArgsBindData>();
    SyncEngine engine;
    auto result = fn(engine, input.context->clientContext, bindData->args);
    if (!result.ok) {
        throw RuntimeException("sqlite sync failed: " + result.error);
    }
    state.stats = result.stats;
}

offset_t emitRow(const TableFuncInput& input, TableFuncOutput& output,
    const std::vector<int64_t>& intCols, const std::vector<std::string>& stringCols) {
    auto state = input.sharedState->ptrCast<SyncSharedState>();
    std::lock_guard<std::mutex> lk{state->mtx};
    if (state->done) {
        return 0;
    }
    state->done = true;
    auto& chunk = output.dataChunk;
    chunk.state->getSelVectorUnsafe().setSelSize(1);
    for (size_t i = 0; i < intCols.size(); i++) {
        chunk.getValueVectorMutable(i).setValue<int64_t>(0, intCols[i]);
    }
    for (size_t i = 0; i < stringCols.size(); i++) {
        StringVector::addString(&chunk.getValueVectorMutable(intCols.size() + i), 0, stringCols[i]);
    }
    return 1;
}

std::unique_ptr<TableFuncSharedState> initSyncShared(const TableFuncInitSharedStateInput& input,
    std::function<SyncResult(SyncEngine&, main::ClientContext*, const std::vector<std::string>&)>
        fn) {
    auto state = std::make_unique<SyncSharedState>();
    runEngine(input, std::move(fn), *state);
    return state;
}

} // namespace

// ---- sqlite_sync('/path/to/file.db') ----

struct SqliteSyncTableFunc {
    static offset_t tableFunc(const TableFuncInput& input, TableFuncOutput& output) {
        auto state = input.sharedState->ptrCast<SyncSharedState>();
        // Copy stats under lock; emitRow takes the lock again for done.
        return emitRow(input, output,
            {state->stats.tables, state->stats.upserted, state->stats.deleted,
                state->stats.appliedFrames},
            {state->stats.status});
    }

    static std::unique_ptr<TableFuncBindData> bindFunc(main::ClientContext* /*context*/,
        const TableFuncBindInput* input) {
        auto args = readArgs(input, 1);
        if (args.empty() || args[0].empty()) {
            throw RuntimeException("sqlite_sync requires the SQLite file path");
        }
        binder::expression_vector columns;
        columns.push_back(input->binder->createVariable("tables_synced", LogicalType::INT64()));
        columns.push_back(input->binder->createVariable("rows_upserted", LogicalType::INT64()));
        columns.push_back(input->binder->createVariable("rows_deleted", LogicalType::INT64()));
        columns.push_back(input->binder->createVariable("applied_frames", LogicalType::INT64()));
        columns.push_back(input->binder->createVariable("status", LogicalType::STRING()));
        return std::make_unique<SyncArgsBindData>(std::move(columns), std::move(args));
    }

    static std::unique_ptr<TableFuncSharedState> initSharedState(
        const TableFuncInitSharedStateInput& input) {
        return initSyncShared(input,
            [](SyncEngine& engine, main::ClientContext* context,
                const std::vector<std::string>& args) { return engine.sync(context, args[0]); });
    }

    static std::unique_ptr<TableFuncLocalState> initLocalState(
        const TableFuncInitLocalStateInput&) {
        return std::make_unique<TableFuncLocalState>();
    }
};

function_set SqliteSyncFunction::getFunctionSet() {
    function_set functionSet;
    auto function =
        std::make_unique<TableFunction>(name, std::vector<LogicalTypeID>{LogicalTypeID::ANY});
    function->bindFunc = SqliteSyncTableFunc::bindFunc;
    function->tableFunc = SqliteSyncTableFunc::tableFunc;
    function->initSharedStateFunc = SqliteSyncTableFunc::initSharedState;
    function->initLocalStateFunc = SqliteSyncTableFunc::initLocalState;
    function->canParallelFunc = [] { return false; };
    functionSet.push_back(std::move(function));
    return functionSet;
}

// ---- sqlite_register_node('/path/to/file.db', 'table'[, 'label'[, 'id_column']]) ----

struct SqliteRegisterNodeTableFunc {
    static offset_t tableFunc(const TableFuncInput& input, TableFuncOutput& output) {
        auto state = input.sharedState->ptrCast<SyncSharedState>();
        return emitRow(input, output, {}, {state->stats.status});
    }

    static std::unique_ptr<TableFuncBindData> bindFunc(main::ClientContext* /*context*/,
        const TableFuncBindInput* input) {
        auto args = readArgs(input, 4);
        if (args.size() < 2 || args[0].empty() || args[1].empty()) {
            throw RuntimeException("sqlite_register_node requires (db_path, table)");
        }
        binder::expression_vector columns;
        columns.push_back(input->binder->createVariable("status", LogicalType::STRING()));
        return std::make_unique<SyncArgsBindData>(std::move(columns), std::move(args));
    }

    static std::unique_ptr<TableFuncSharedState> initSharedState(
        const TableFuncInitSharedStateInput& input) {
        return initSyncShared(input, [](SyncEngine& engine, main::ClientContext* context,
                                         const std::vector<std::string>& args) {
            return engine.registerNode(context, args[0], args[1], optionalArg(args, 2),
                optionalArg(args, 3));
        });
    }

    static std::unique_ptr<TableFuncLocalState> initLocalState(
        const TableFuncInitLocalStateInput&) {
        return std::make_unique<TableFuncLocalState>();
    }
};

function_set SqliteRegisterNodeFunction::getFunctionSet() {
    function_set functionSet;
    // (alias, table[, label[, id_column]]): one overload per arity.
    for (size_t arity = 2; arity <= 4; arity++) {
        auto function = std::make_unique<TableFunction>(name,
            std::vector<LogicalTypeID>(arity, LogicalTypeID::ANY));
        function->bindFunc = SqliteRegisterNodeTableFunc::bindFunc;
        function->tableFunc = SqliteRegisterNodeTableFunc::tableFunc;
        function->initSharedStateFunc = SqliteRegisterNodeTableFunc::initSharedState;
        function->initLocalStateFunc = SqliteRegisterNodeTableFunc::initLocalState;
        function->canParallelFunc = [] { return false; };
        functionSet.push_back(std::move(function));
    }
    return functionSet;
}

// ---- sqlite_register_edge('/path/to/file.db', 'table'[, 'label'[, 'from_col'[, 'to_col']]]) ----

struct SqliteRegisterEdgeTableFunc {
    static offset_t tableFunc(const TableFuncInput& input, TableFuncOutput& output) {
        auto state = input.sharedState->ptrCast<SyncSharedState>();
        return emitRow(input, output, {}, {state->stats.status});
    }

    static std::unique_ptr<TableFuncBindData> bindFunc(main::ClientContext* /*context*/,
        const TableFuncBindInput* input) {
        auto args = readArgs(input, 5);
        if (args.size() < 2 || args[0].empty() || args[1].empty()) {
            throw RuntimeException("sqlite_register_edge requires (db_path, table)");
        }
        binder::expression_vector columns;
        columns.push_back(input->binder->createVariable("status", LogicalType::STRING()));
        return std::make_unique<SyncArgsBindData>(std::move(columns), std::move(args));
    }

    static std::unique_ptr<TableFuncSharedState> initSharedState(
        const TableFuncInitSharedStateInput& input) {
        return initSyncShared(input, [](SyncEngine& engine, main::ClientContext* context,
                                         const std::vector<std::string>& args) {
            return engine.registerEdge(context, args[0], args[1], optionalArg(args, 2),
                optionalArg(args, 3), optionalArg(args, 4));
        });
    }

    static std::unique_ptr<TableFuncLocalState> initLocalState(
        const TableFuncInitLocalStateInput&) {
        return std::make_unique<TableFuncLocalState>();
    }
};

function_set SqliteRegisterEdgeFunction::getFunctionSet() {
    function_set functionSet;
    // (alias, table[, label[, from_col[, to_col]]]): one overload per arity.
    for (size_t arity = 2; arity <= 5; arity++) {
        auto function = std::make_unique<TableFunction>(name,
            std::vector<LogicalTypeID>(arity, LogicalTypeID::ANY));
        function->bindFunc = SqliteRegisterEdgeTableFunc::bindFunc;
        function->tableFunc = SqliteRegisterEdgeTableFunc::tableFunc;
        function->initSharedStateFunc = SqliteRegisterEdgeTableFunc::initSharedState;
        function->initLocalStateFunc = SqliteRegisterEdgeTableFunc::initLocalState;
        function->canParallelFunc = [] { return false; };
        functionSet.push_back(std::move(function));
    }
    return functionSet;
}

// ---- sqlite_sync_start(path[, interval_ms]) ----

struct SqliteSyncStartTableFunc {
    static offset_t tableFunc(const TableFuncInput& input, TableFuncOutput& output) {
        auto state = input.sharedState->ptrCast<SyncSharedState>();
        return emitRow(input, output, {}, {state->stats.status});
    }

    static std::unique_ptr<TableFuncBindData> bindFunc(main::ClientContext* /*context*/,
        const TableFuncBindInput* input) {
        auto args = readArgs(input, 1);
        if (args.empty() || args[0].empty()) {
            throw RuntimeException("sqlite_sync_start requires the SQLite file path");
        }
        binder::expression_vector columns;
        columns.push_back(input->binder->createVariable("status", LogicalType::STRING()));
        auto bindData = std::make_unique<SyncArgsBindData>(std::move(columns), std::move(args));
        if (input->params.size() > 1) {
            bindData->intArg = input->getLiteralVal<int64_t>(1);
            bindData->hasIntArg = true;
        }
        return bindData;
    }

    static std::unique_ptr<TableFuncSharedState> initSharedState(
        const TableFuncInitSharedStateInput& input) {
        auto state = std::make_unique<SyncSharedState>();
        auto bindData = input.bindData->constPtrCast<SyncArgsBindData>();
        // The daemon needs the Database, reachable via the execution
        // context's client context.
        auto* db = input.context->clientContext->getDatabase();
        int64_t intervalMs = bindData->hasIntArg ? bindData->intArg : 1000;
        auto result = SyncDaemon::start(db, bindData->args[0], intervalMs);
        if (!result.ok) {
            throw RuntimeException("sqlite_sync_start failed: " + result.error);
        }
        state->stats = result.stats;
        return state;
    }

    static std::unique_ptr<TableFuncLocalState> initLocalState(
        const TableFuncInitLocalStateInput&) {
        return std::make_unique<TableFuncLocalState>();
    }
};

function_set SqliteSyncStartFunction::getFunctionSet() {
    function_set functionSet;
    // (path[, interval_ms]): one overload per arity.
    for (size_t arity = 1; arity <= 2; arity++) {
        auto function = std::make_unique<TableFunction>(name,
            std::vector<LogicalTypeID>(arity, LogicalTypeID::ANY));
        function->bindFunc = SqliteSyncStartTableFunc::bindFunc;
        function->tableFunc = SqliteSyncStartTableFunc::tableFunc;
        function->initSharedStateFunc = SqliteSyncStartTableFunc::initSharedState;
        function->initLocalStateFunc = SqliteSyncStartTableFunc::initLocalState;
        function->canParallelFunc = [] { return false; };
        functionSet.push_back(std::move(function));
    }
    return functionSet;
}

// ---- sqlite_sync_stop(path) ----

struct SqliteSyncStopTableFunc {
    static offset_t tableFunc(const TableFuncInput& input, TableFuncOutput& output) {
        auto state = input.sharedState->ptrCast<SyncSharedState>();
        return emitRow(input, output, {}, {state->stats.status});
    }

    static std::unique_ptr<TableFuncBindData> bindFunc(main::ClientContext* /*context*/,
        const TableFuncBindInput* input) {
        auto args = readArgs(input, 1);
        if (args.empty() || args[0].empty()) {
            throw RuntimeException("sqlite_sync_stop requires the SQLite file path");
        }
        binder::expression_vector columns;
        columns.push_back(input->binder->createVariable("status", LogicalType::STRING()));
        return std::make_unique<SyncArgsBindData>(std::move(columns), std::move(args));
    }

    static std::unique_ptr<TableFuncSharedState> initSharedState(
        const TableFuncInitSharedStateInput& input) {
        auto state = std::make_unique<SyncSharedState>();
        auto bindData = input.bindData->constPtrCast<SyncArgsBindData>();
        auto result = SyncDaemon::stop(bindData->args[0]);
        if (!result.ok) {
            throw RuntimeException("sqlite_sync_stop failed: " + result.error);
        }
        state->stats = result.stats;
        return state;
    }

    static std::unique_ptr<TableFuncLocalState> initLocalState(
        const TableFuncInitLocalStateInput&) {
        return std::make_unique<TableFuncLocalState>();
    }
};

function_set SqliteSyncStopFunction::getFunctionSet() {
    function_set functionSet;
    auto function =
        std::make_unique<TableFunction>(name, std::vector<LogicalTypeID>{LogicalTypeID::ANY});
    function->bindFunc = SqliteSyncStopTableFunc::bindFunc;
    function->tableFunc = SqliteSyncStopTableFunc::tableFunc;
    function->initSharedStateFunc = SqliteSyncStopTableFunc::initSharedState;
    function->initLocalStateFunc = SqliteSyncStopTableFunc::initLocalState;
    function->canParallelFunc = [] { return false; };
    functionSet.push_back(std::move(function));
    return functionSet;
}

// ---- sqlite_sync_status() ----

struct SqliteSyncStatusTableFunc {
    struct StatusBindData : TableFuncBindData {
        explicit StatusBindData(binder::expression_vector columns)
            : TableFuncBindData{std::move(columns), 0} {}
        StatusBindData(const StatusBindData& other) : TableFuncBindData{other} {}
        std::unique_ptr<TableFuncBindData> copy() const override {
            return std::make_unique<StatusBindData>(*this);
        }
    };

    struct StatusSharedState : TableFuncSharedState {
        std::vector<DaemonStatusRow> rows;
        size_t currentOffset = 0;
        explicit StatusSharedState(std::vector<DaemonStatusRow> rows)
            : TableFuncSharedState{rows.size()}, rows{std::move(rows)} {}
    };

    static offset_t tableFunc(const TableFuncInput& input, TableFuncOutput& output) {
        auto state = input.sharedState->ptrCast<StatusSharedState>();
        std::lock_guard<std::mutex> lk{state->mtx};
        if (state->currentOffset >= state->rows.size()) {
            return 0;
        }
        auto& row = state->rows[state->currentOffset++];
        auto& chunk = output.dataChunk;
        chunk.state->getSelVectorUnsafe().setSelSize(1);
        chunk.getValueVectorMutable(0).setValue<int64_t>(0, row.running ? 1 : 0);
        chunk.getValueVectorMutable(1).setValue<int64_t>(0, row.intervalMs);
        chunk.getValueVectorMutable(2).setValue<int64_t>(0, row.appliedFrames);
        chunk.getValueVectorMutable(3).setValue<int64_t>(0, row.upserted);
        chunk.getValueVectorMutable(4).setValue<int64_t>(0, row.deleted);
        chunk.getValueVectorMutable(5).setValue<int64_t>(0, row.consecutiveErrors);
        StringVector::addString(&chunk.getValueVectorMutable(6), 0, row.sync);
        StringVector::addString(&chunk.getValueVectorMutable(7), 0, row.lastError);
        return 1;
    }

    static std::unique_ptr<TableFuncBindData> bindFunc(main::ClientContext* /*context*/,
        const TableFuncBindInput* input) {
        (void)input;
        binder::expression_vector columns;
        columns.push_back(input->binder->createVariable("running", LogicalType::INT64()));
        columns.push_back(input->binder->createVariable("interval_ms", LogicalType::INT64()));
        columns.push_back(input->binder->createVariable("applied_frames", LogicalType::INT64()));
        columns.push_back(input->binder->createVariable("upserted", LogicalType::INT64()));
        columns.push_back(input->binder->createVariable("deleted", LogicalType::INT64()));
        columns.push_back(input->binder->createVariable("errors", LogicalType::INT64()));
        columns.push_back(input->binder->createVariable("sync", LogicalType::STRING()));
        columns.push_back(input->binder->createVariable("last_error", LogicalType::STRING()));
        return std::make_unique<StatusBindData>(std::move(columns));
    }

    static std::unique_ptr<TableFuncSharedState> initSharedState(
        const TableFuncInitSharedStateInput&) {
        return std::make_unique<StatusSharedState>(SyncDaemon::status());
    }

    static std::unique_ptr<TableFuncLocalState> initLocalState(
        const TableFuncInitLocalStateInput&) {
        return std::make_unique<TableFuncLocalState>();
    }
};

function_set SqliteSyncStatusFunction::getFunctionSet() {
    function_set functionSet;
    auto function = std::make_unique<TableFunction>(name, std::vector<LogicalTypeID>{});
    function->bindFunc = SqliteSyncStatusTableFunc::bindFunc;
    function->tableFunc = SqliteSyncStatusTableFunc::tableFunc;
    function->initSharedStateFunc = SqliteSyncStatusTableFunc::initSharedState;
    function->initLocalStateFunc = SqliteSyncStatusTableFunc::initLocalState;
    function->canParallelFunc = [] { return false; };
    functionSet.push_back(std::move(function));
    return functionSet;
}

} // namespace sqlite_extension
} // namespace lbug
