#include "main/pg_client_extension.h"

#include "main/client_context.h"
#include "main/database.h"
#include "storage/pg_client_storage.h"

namespace lbug {
namespace pg_client_extension {

using namespace extension;

void PgClientExtension::load(main::ClientContext* context) {
    auto db = context->getDatabase();
    db->registerStorageExtension(EXTENSION_NAME,
        std::make_unique<PgClientStorageExtension>(*db));
}

} // namespace pg_client_extension
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
    lbug::pg_client_extension::PgClientExtension::load(context);
}

INIT_EXPORT const char* name() {
    return lbug::pg_client_extension::PgClientExtension::EXTENSION_NAME;
}
}
#endif
