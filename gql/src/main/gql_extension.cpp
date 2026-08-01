#include "main/gql_extension.h"

#include "function/gql_function.h"
#include "main/client_context.h"

namespace lbug {
namespace gql_extension {

using namespace extension;

void GqlExtension::load(main::ClientContext* context) {
    auto& db = *context->getDatabase();
    ExtensionUtils::addStandaloneTableFunc<GqlFunction>(db);
}

} // namespace gql_extension
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
    lbug::gql_extension::GqlExtension::load(context);
}

INIT_EXPORT const char* name() {
    return lbug::gql_extension::GqlExtension::EXTENSION_NAME;
}
}
#endif
