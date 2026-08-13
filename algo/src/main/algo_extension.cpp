#include "main/algo_extension.h"

#include <cstdlib>

#include "function/algo_function.h"
#include "main/client_context.h"

namespace lbug {
namespace algo_extension {

using namespace extension;

void AlgoExtension::load(main::ClientContext* context) {
#if defined(ICEBUG_ENABLED)
    // Arrow's default memory pool is mimalloc-backed, and mimalloc's per-thread init is not
    // safe on threads that predate libarrow's dlopen — GDS functions allocating Arrow buffers
    // from ladybug worker threads segfault in _mi_thread_init (non-deterministically; ~half of
    // full-suite runs). Steer Arrow to the system allocator once, before its default pool is
    // first used; Arrow reads this env var lazily, and load() runs before any GDS allocation.
    // overwrite=0 respects a user-provided value.
#if defined(_WIN32)
    // setenv() is not available on MSVC; _putenv_s always overwrites, so check first.
    if (getenv("ARROW_DEFAULT_MEMORY_POOL") == nullptr) {
        _putenv_s("ARROW_DEFAULT_MEMORY_POOL", "system");
    }
#else
    setenv("ARROW_DEFAULT_MEMORY_POOL", "system", 0);
#endif
#endif
    auto& db = *context->getDatabase();
    ExtensionUtils::addTableFunc<SCCFunction>(db);
    ExtensionUtils::addTableFuncAlias<SCCAliasFunction>(db);
    ExtensionUtils::addTableFunc<SCCKosarajuFunction>(db);
    ExtensionUtils::addTableFuncAlias<SCCKosarajuAliasFunction>(db);
    ExtensionUtils::addTableFunc<WeaklyConnectedComponentsFunction>(db);
    ExtensionUtils::addTableFuncAlias<WeaklyConnectedComponentsAliasFunction>(db);
    ExtensionUtils::addTableFunc<PageRankFunction>(db);
    ExtensionUtils::addTableFuncAlias<PageRankAliasFunction>(db);
#if defined(ICEBUG_ENABLED)
    ExtensionUtils::addTableFunc<GDSPageRankFunction>(db);
    ExtensionUtils::addTableFunc<GDSNode2VecFunction>(db);
    ExtensionUtils::addTableFunc<GDSLouvainFunction>(db);
    ExtensionUtils::addTableFunc<GDSLeidenFunction>(db);
    ExtensionUtils::addTableFunc<GDSPprFunction>(db);
#endif
    ExtensionUtils::addTableFunc<KCoreDecompositionFunction>(db);
    ExtensionUtils::addTableFuncAlias<KCoreDecompositionAliasFunction>(db);
    ExtensionUtils::addTableFunc<LouvainFunction>(db);
    ExtensionUtils::addTableFunc<SpanningForest>(db);
    ExtensionUtils::addTableFuncAlias<SpanningForestAliasFunction>(db);
}

} // namespace algo_extension
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
    lbug::algo_extension::AlgoExtension::load(context);
}

INIT_EXPORT const char* name() {
    return lbug::algo_extension::AlgoExtension::EXTENSION_NAME;
}
}
#endif
