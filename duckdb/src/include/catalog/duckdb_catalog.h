#pragma once

#include "binder/ddl/bound_create_table_info.h"
#include "common/vector/value_vector.h"
#include "connector/duckdb_connector.h"
#include "extension/catalog_extension.h"

namespace lbug {
namespace binder {
struct AttachOption;
} // namespace binder

namespace duckdb_extension {

struct BoundExtraCreateDuckDBTableInfo : public binder::BoundExtraCreateTableInfo {
    std::string catalogName;
    std::string schemaName;

    BoundExtraCreateDuckDBTableInfo(std::string catalogName, std::string schemaName,
        std::vector<binder::PropertyDefinition> propertyDefinitions)
        : BoundExtraCreateTableInfo{std::move(propertyDefinitions)},
          catalogName{std::move(catalogName)}, schemaName{std::move(schemaName)} {}
    BoundExtraCreateDuckDBTableInfo(const BoundExtraCreateDuckDBTableInfo& other)
        : BoundExtraCreateTableInfo{copyVector(other.propertyDefinitions)},
          catalogName{other.catalogName}, schemaName{other.schemaName} {}

    std::unique_ptr<BoundExtraCreateCatalogEntryInfo> copy() const override {
        return std::make_unique<BoundExtraCreateDuckDBTableInfo>(*this);
    }
};

class DuckDBCatalog : public extension::CatalogExtension {
public:
    DuckDBCatalog(std::string dbPath, std::string catalogName, std::string defaultSchemaName,
        main::ClientContext* context, const DuckDBConnector& connector,
        const binder::AttachOption& attachOption, std::string attachedDbName);

    void init() override;

    static std::string bindSchemaName(const binder::AttachOption& options,
        const std::string& defaultName);

protected:
    bool bindPropertyDefinitions(const std::string& tableName,
        std::vector<binder::PropertyDefinition>& propertyDefinitions);

private:
    virtual std::unique_ptr<binder::BoundCreateTableInfo> bindCreateTableInfo(
        const std::string& tableName);

private:
    void createForeignTable(const std::string& tableName);
    void createForeignRelTable(const std::string& tableName, bool internalIDContract);

protected:
    std::string dbPath;
    std::string catalogName;
    // The name this database was attached under (e.g. 'g' in ATTACH ... as g).
    // The join-push-down optimizer uses it as a lookup key into
    // DatabaseManager::getAttachedDatabase(), so shadow entries and rel group
    // entries must store this name -- not the schema-qualified catalog name.
    std::string defaultSchemaName;
    std::string dbName;
    common::ValueVector tableNamesVector;
    bool skipUnsupportedTable;
    const DuckDBConnector& connector;
    main::ClientContext* context_;
};

} // namespace duckdb_extension
} // namespace lbug
