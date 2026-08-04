#include "main/timeseries_extension.h"

#include "extension/extension.h"
#include "function/timeseries_function.h"
#include "main/client_context.h"

namespace lbug {
namespace timeseries_extension {

using namespace lbug::extension;

void TimeseriesExtension::load(main::ClientContext* context) {
    auto& db = *context->getDatabase();
    ExtensionUtils::addTableFunc<EmbeddingSimilarityFunction>(db);
    ExtensionUtils::addTableFunc<DetectDriftPointsFunction>(db);
}

} // namespace timeseries_extension
} // namespace lbug

#if defined(BUILD_DYNAMIC_LOAD)
extern "C" {
#if defined(_WIN32)
#define INIT_EXPORT __declspec(dllexport)
#else
#define INIT_EXPORT __attribute__((visibility("default")))
#endif
INIT_EXPORT void init(lbug::main::ClientContext* context) {
    lbug::timeseries_extension::TimeseriesExtension::load(context);
}

INIT_EXPORT const char* name() {
    return lbug::timeseries_extension::TimeseriesExtension::EXTENSION_NAME;
}
}
#endif
