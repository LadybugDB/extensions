# Ladybug Iceberg Extension

Reads Apache Iceberg tables through an embedded DuckDB instance. Tables can be
scanned either from a filesystem path (metadata directory or metadata file) or,
by attaching an Iceberg REST catalog, by fully qualified table name.

## Querying tables from a filesystem path

```cypher
LOAD FROM 'path/to/iceberg_table' (file_format='iceberg', allow_moved_paths=true) RETURN count(*);
```

Path-based scans resolve the current metadata file through
`metadata/version-hint.text`, which is only produced by filesystem-based
catalogs (`HadoopCatalog`/`HadoopTables`). If no version hint is present, scan a
concrete metadata file instead:

```cypher
LOAD FROM 'path/to/iceberg_table/metadata/v3.metadata.json' (file_format='iceberg') RETURN count(*);
```

## Querying tables through an Iceberg REST catalog

Real-world data lakes usually register tables in a catalog (AWS Glue, Apache
Polaris, Lakekeeper, Nessie, Unity Catalog, ...) which resolves the current
metadata file at query time — no `version-hint.text` is involved, so streaming
commits from PyIceberg/Java Iceberg writers are picked up without external
bookkeeping and concurrent commits stay safe.

To use a REST catalog, set the `iceberg_warehouse` option (plus connection and
authentication options) and reference tables by fully qualified name:

```cypher
CALL iceberg_warehouse='warehouse';
CALL iceberg_endpoint='https://rest-catalog.example.com';
CALL iceberg_token='<bearer-token>';
LOAD FROM 'default.events' (file_format='iceberg') RETURN count(*);
LOAD FROM 'iceberg_catalog.default.events' (file_format='iceberg') RETURN count(*);
```

The catalog is attached inside the embedded DuckDB instance under the fixed
alias `iceberg_catalog`. Table references can use either
`iceberg_catalog.namespace.table` (3-part) or `namespace.table` (2-part, which
is prefixed with the alias automatically). Any other 3-part name is rejected.

### Options

| Option | Description |
|---|---|
| `iceberg_warehouse` | Warehouse identifier; the path argument of the DuckDB `ATTACH ... (TYPE ICEBERG)` statement. Setting it enables REST catalog mode. |
| `iceberg_endpoint` | REST catalog endpoint URL. |
| `iceberg_endpoint_type` | Shorthand endpoint construction, e.g. `glue` or `s3_tables`. |
| `iceberg_authorization_type` | Set to `sigv4` for catalogs that authenticate via AWS SigV4 (e.g. AWS Glue). |
| `iceberg_token` | OAuth2 bearer token (confidential). |
| `iceberg_client_id` / `iceberg_client_secret` | OAuth2 client credentials (confidential). |
| `iceberg_oauth2_server_uri` | OAuth2 token endpoint used with client credentials. |

Every option can also be provided through an environment variable of the same
name (upper-case also accepted, e.g. `ICEBERG_TOKEN`), which is useful for
keeping credentials out of scripts.

Authentication is optional: catalogs reachable without credentials (e.g.
`s3_tables`/`glue` combined with S3 environment credentials) can be attached
with just `iceberg_warehouse` and `iceberg_endpoint(_type)`. Data files on S3
are read with the usual `s3_*` options of the httpfs integration.

### Time travel

For REST catalog tables, time travel uses DuckDB's `AT` clause semantics
expressed through scan options:

```cypher
LOAD FROM 'iceberg_catalog.default.events' (file_format='iceberg', snapshot_from_id=7635660646343998149) RETURN count(*);
LOAD FROM 'iceberg_catalog.default.events' (file_format='iceberg', snapshot_from_timestamp=timestamp('2026-01-01 00:00:00')) RETURN count(*);
```

`snapshot_from_id` and `snapshot_from_timestamp` are mutually exclusive; other
scan options (e.g. `allow_moved_paths`) do not apply to catalog tables.

### Metadata functions

`iceberg_metadata` and `iceberg_snapshots` also accept fully qualified catalog
table names:

```cypher
CALL iceberg_snapshots('iceberg_catalog.default.events') RETURN *;
```
