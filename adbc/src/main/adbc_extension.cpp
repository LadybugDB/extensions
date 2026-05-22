#include "main/adbc_extension.h"

#include "main/client_context.h"
#include "main/database.h"
#include "storage/adbc_storage.h"

namespace lbug {
namespace adbc_extension {

void ADBCExtension::load(main::ClientContext* context) {
    auto db = context->getDatabase();
    db->registerStorageExtension(EXTENSION_NAME, std::make_unique<ADBCStorageExtension>(*db));
}

} // namespace adbc_extension
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
    lbug::adbc_extension::ADBCExtension::load(context);
}

INIT_EXPORT const char* name() {
    return lbug::adbc_extension::ADBCExtension::EXTENSION_NAME;
}
}
#endif
