
#include "main/ducklake_extension.h"

#include "main/client_context.h"
#include "options/ducklake_options.h"
#include "storage/ducklake_storage.h"

namespace lbug {
namespace ducklake_extension {

void DuckLakeExtension::load(main::ClientContext* context) {
    auto& db = *context->getDatabase();
    db.registerStorageExtension(EXTENSION_NAME, std::make_unique<DuckLakeStorageExtension>(db));
    DuckLakeOptions::registerExtensionOptions(&db);
    DuckLakeOptions::setEnvValue(context);
}

} // namespace ducklake_extension
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
    lbug::ducklake_extension::DuckLakeExtension::load(context);
}

INIT_EXPORT const char* name() {
    return lbug::ducklake_extension::DuckLakeExtension::EXTENSION_NAME;
}
}
#endif
