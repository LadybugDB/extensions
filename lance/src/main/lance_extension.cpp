#include "main/lance_extension.h"

#include "function/lance_scan.h"
#include "main/client_context.h"
#include "main/database.h"
#include "storage/lance_node_table.h"
#include "storage/lance_rel_table.h"
#include "storage/table/external_table_hooks.h"

namespace lbug {
namespace lance_extension {

void LanceExtension::load(main::ClientContext* context) {
    auto& db = *context->getDatabase();
    extension::ExtensionUtils::addTableFunc<LanceScanFunction>(db);
    // Register Lance-backed table storage for StorageFormat::LANCE. The scan-state
    // rank (3) declares that Lance scan states extend the icebug-disk scan state and
    // therefore cover every lower-rank backend in shared multi-rel scans.
    storage::ExternalTableHooks hooks;
    hooks.createNodeTable = [](const storage::StorageManager* storageManager,
                                const catalog::NodeTableCatalogEntry* nodeTableEntry,
                                storage::MemoryManager* memoryManager, main::ClientContext* ctx) {
        return std::make_unique<LanceNodeTable>(storageManager, nodeTableEntry, memoryManager, ctx);
    };
    hooks.createRelTable = [](catalog::RelGroupCatalogEntry* relGroupEntry,
                               common::table_id_t fromTableID, common::table_id_t toTableID,
                               const storage::StorageManager* storageManager,
                               storage::MemoryManager* memoryManager, main::ClientContext* ctx) {
        return std::make_unique<LanceRelTable>(relGroupEntry, fromTableID, toTableID,
            storageManager, memoryManager, ctx);
    };
    hooks.createNodeScanState = [](storage::NodeTable* table, common::ValueVector* nodeIDVector,
                                    const std::vector<common::ValueVector*>& outVectors,
                                    std::shared_ptr<common::DataChunkState> outChunkState,
                                    storage::MemoryManager* memoryManager) {
        return std::make_unique<LanceNodeTableScanState>(*memoryManager, nodeIDVector, outVectors,
            std::move(outChunkState));
    };
    hooks.getNumScanMorsels = [](const transaction::Transaction* transaction,
                                  storage::NodeTable* table) {
        return table->ptrCast<LanceNodeTable>()->getNumScanMorsels(transaction);
    };
    hooks.createRelScanState = [](storage::RelTable* table, common::ValueVector* boundNodeIDVector,
                                   const std::vector<common::ValueVector*>& outVectors,
                                   std::shared_ptr<common::DataChunkState> outChunkState,
                                   storage::MemoryManager* memoryManager) {
        return std::make_unique<LanceRelTableScanState>(*memoryManager, boundNodeIDVector,
            outVectors, std::move(outChunkState));
    };
    hooks.scanStateRank = 3;
    storage::ExternalTableHookRegistry::registerHooks(common::StorageFormat::LANCE,
        std::move(hooks));
}

} // namespace lance_extension
} // namespace lbug

#if defined(BUILD_DYNAMIC_LOAD)
extern "C" {
// Because we link against the static library on windows, we implicitly inherit LBUG_STATIC_DEFINE,
// which cancels out any exporting, so we can't use LBUG_API.
#if defined(_WIN32)
#define INIT_EXPORT __declspec(dllexport)
#else
#define INIT_EXPORT __attribute__((visibility("default")))
#endif
INIT_EXPORT void init(lbug::main::ClientContext* context) {
    lbug::lance_extension::LanceExtension::load(context);
}

INIT_EXPORT const char* name() {
    return lbug::lance_extension::LanceExtension::EXTENSION_NAME;
}
}
#endif
