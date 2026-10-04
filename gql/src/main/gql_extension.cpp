#include "main/gql_extension.h"

#include "function/gql_function.h"
#include "function/gql_json_functions.h"
#include "main/client_context.h"

namespace lbug {
namespace gql_extension {

using namespace extension;

void GqlExtension::load(main::ClientContext* context) {
    auto& db = *context->getDatabase();
    ExtensionUtils::addStandaloneTableFunc<GqlFunction>(db);
    // GQL extension functions. addFunc is idempotent (checks containsFunction)
    // and catalog names are case-insensitive, so these cannot collide with the
    // JSON extension's to_json or with a re-load.
    addFunc<GqlToJsonFunction>(db, GqlToJsonFunction::name,
        catalog::CatalogEntryType::SCALAR_FUNCTION_ENTRY);
    addFunc<GqlMaxFunction>(db, GqlMaxFunction::name,
        catalog::CatalogEntryType::AGGREGATE_FUNCTION_ENTRY);
    addFunc<GqlMinFunction>(db, GqlMinFunction::name,
        catalog::CatalogEntryType::AGGREGATE_FUNCTION_ENTRY);
    addFunc<GqlSumFunction>(db, GqlSumFunction::name,
        catalog::CatalogEntryType::AGGREGATE_FUNCTION_ENTRY);
    addFunc<GqlAvgFunction>(db, GqlAvgFunction::name,
        catalog::CatalogEntryType::AGGREGATE_FUNCTION_ENTRY);
    addFunc<GqlLtFunction>(db, GqlLtFunction::name,
        catalog::CatalogEntryType::SCALAR_FUNCTION_ENTRY);
    addFunc<GqlLeFunction>(db, GqlLeFunction::name,
        catalog::CatalogEntryType::SCALAR_FUNCTION_ENTRY);
    addFunc<GqlGtFunction>(db, GqlGtFunction::name,
        catalog::CatalogEntryType::SCALAR_FUNCTION_ENTRY);
    addFunc<GqlGeFunction>(db, GqlGeFunction::name,
        catalog::CatalogEntryType::SCALAR_FUNCTION_ENTRY);
    addFunc<GqlEqFunction>(db, GqlEqFunction::name,
        catalog::CatalogEntryType::SCALAR_FUNCTION_ENTRY);
    addFunc<GqlNeFunction>(db, GqlNeFunction::name,
        catalog::CatalogEntryType::SCALAR_FUNCTION_ENTRY);
    addFunc<GqlSortKeyFunction>(db, GqlSortKeyFunction::name,
        catalog::CatalogEntryType::SCALAR_FUNCTION_ENTRY);
    addFunc<GqlIsSimpleFunction>(db, GqlIsSimpleFunction::name,
        catalog::CatalogEntryType::SCALAR_FUNCTION_ENTRY);
    addFunc<GqlListCheckedFunction>(db, GqlListCheckedFunction::name,
        catalog::CatalogEntryType::SCALAR_FUNCTION_ENTRY);
    addFunc<GqlSchemasFunction>(db, GqlSchemasFunction::name,
        catalog::CatalogEntryType::SCALAR_FUNCTION_ENTRY);
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
