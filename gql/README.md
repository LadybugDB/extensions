# GQL extension — ISO GQL compatibility layer for LadybugDB

`CALL GQL("<ISO GQL statement>")` translates GQL to LadybugDB's native Cypher
dialect and executes it. GQL and the equivalent Cypher produce the same result
sets (see the dual-run parity tests in `test/test_files/`).

This is a **translation layer**, not a native GQL execution engine: LadybugDB's
query engine is Cypher-dialect; this extension maps GQL statements onto it.

## Usage

```sql
LOAD EXTENSION '.../libgql.lbug_extension';
CALL GQL("INSERT (n:Person {name: 'Alice', age: 30})");
CALL GQL("SELECT n.name AS name, count(*) AS c FROM GRAPH main MATCH (n:Person)");
```

Notes:

- `CALL GQL(...)` must be the only statement in the query batch (engine
  rewrite-path constraint).
- Unmapped GQL constructs fail fast with
  `GQL feature not supported: <construct>` — there is no silent pass-through
  to the Cypher parser.
- LadybugDB-specific DDL (`CREATE NODE TABLE`, `COPY ...`, ...) can be passed
  through `CALL GQL(...)` as a convenience; this is a private extension, not
  ISO GQL.

## Compatibility matrix (graded)

Format follows the GQL conformance model: mandatory features (by standard
subclause) + optional features (G-feature IDs). ✓ = implemented and covered by
parity tests; ✗ = explicitly rejected with `GQL feature not supported`.

### Mandatory features

| Subclause | Feature | Status | Mapping |
|---|---|---|---|
| 8 | Transaction management (START TRANSACTION / COMMIT / ROLLBACK) | ✓ | → `BEGIN TRANSACTION` / `COMMIT` / `ROLLBACK` |
| 14.4 | MATCH / OPTIONAL MATCH | ✓ | pass-through (pattern syntax shared) |
| 14.6 | FILTER | ✓ | → `WHERE` |
| 14.8 | FOR ... IN | ✓ (basic) | → `UNWIND ... AS` (WITH ORDINALITY/OFFSET ✗) |
| 14.9–14.10 | ORDER BY / SKIP / LIMIT | ✓ | OFFSET → `SKIP` |
| 14.11 | RETURN (incl. GROUP BY on RETURN) | ✓ | grouping → `WITH ... RETURN ...` |
| 14.12 | SELECT ... FROM GRAPH ... | ✓ | → `USE GRAPH` + `MATCH ... RETURN ...` |
| 16.15 | GROUP BY / HAVING (incl. implicit grouping) | ✓ | → `WITH keys, aggs WHERE ... RETURN ...` |
| 13.2 | INSERT | ✓ | → `CREATE` |
| 13.3 | SET | ✓ (approx.) | order-independent assignment emulated via `WITH` snapshot when needed; `SET n:Label` ✗ |
| 13.4 | REMOVE | △ approx. | property → `SET n.prop = NULL` (fixed-schema approximation of "property removed"); label removal ✗ |
| 13.5 | DELETE / DETACH DELETE | ✓ | → `DELETE` / `DETACH DELETE` |
| 12.4–12.5 | CREATE GRAPH / DROP GRAPH | ✓ | `ANY` → native `CREATE GRAPH g ANY` (open graph); typed (`CREATE GRAPH g t` / inline `{ ... }`) → `CREATE GRAPH` + `CREATE NODE/REL TABLE` DDL; `IF NOT EXISTS` emulated; `CREATE OR REPLACE` ✗; `LIKE <graph>` / `AS COPY OF <graph>` ✗ |
| 12.6–12.7 | CREATE GRAPH TYPE / DROP GRAPH TYPE | ✓ (basic) | graph type registered in extension memory (see notes); `AS { spec }` / `AS COPY OF <type>` ✓; `LIKE <graph>` ✗ |
| 7 | Session management | △ partial | `SESSION SET GRAPH` → `USE GRAPH`; SCHEMA / TIME ZONE / PARAMETER ✗ |

### Optional features (selected)

| G-feature | Feature | Status | Notes |
|---|---|---|---|
| G035/G036/G037 | Quantified path patterns (`{m,n}`, `*`, `+`, `?`) | ✓ | quantified single edges and single-hop QPPIs `( ()-[]->() ){m,n}` → Cypher var-length `[e*m..n]`; multi-hop / bound interiors `( ()-[]->()-[]->() ){m,n}` expand by repetition (fixed `{n}` unrolls inline; ranged `{m,n}` with `1 ≤ n-m ≤ 4` re-translates the statement per count and joins with `UNION ALL`); interior element variables bind to **lists** (one entry per repetition, seam nodes shared); unbounded multi-hop, lower-bound-0 multi-hop, and node-quantified patterns ✗ (see difference 23) |
| G005/G015–G020 | Path search prefixes (ANY SHORTEST / ALL SHORTEST) | ✓ (single-hop) | → `[e*SHORTEST ...]` / `[e*ALL SHORTEST ...]`; counted `SHORTEST k`, `SHORTEST GROUP(S)`, `ALL/ANY PATHS` ✗ |
| G010–G013 | Path modes (WALK/TRAIL/ACYCLIC/SIMPLE) | ✓ | single var-length slot → `[e*TRAIL ...]` / `[e*ACYCLIC ...]`; multi-hop patterns (and every ACYCLIC/SIMPLE pattern) additionally bind a path variable and filter with `IS_TRAIL`/`IS_ACYCLIC`/`_gql_is_simple` over the whole path — exact ISO semantics (see difference 7); WALK = engine default. SIMPLE is *not* bare engine `*ACYCLIC` (that only keeps intermediate nodes distinct — an endpoint may equal an intermediate); SIMPLE = `_gql_is_simple(p)` (nodes distinct except first=last may coincide), with `*ACYCLIC` as a cheap superset prefilter |
| G074 etc. | Label expressions (`&`, `!`, `|`, `%`) | ✓ (node patterns) | `:A&B`/`:A\|B`/`:!A`/parens → WHERE predicates over `labels(v)` (graph-kind-aware, see difference 13); simple `:Label` unchanged (table pruning); INSERT label sets `:A&B` → `CREATE (n:A:B ...)` on ANY graphs; `%` wildcard → true on table graphs / `size(labels(v)) > 0` on ANY; `IS [NOT] LABELED <expr>` predicates share the same predicate outlet (`IS NOT` negates the whole predicate); edge label expressions ✗ |
| G100 | ELEMENT_ID | ✓ | → `internal_id()` |
| G115 | PROPERTY_EXISTS | ✓ | → `(v.prop IS NOT NULL)` (fixed-schema model of "property present", consistent with difference 1) |
| GA05 | Cast specification | ✓ | `CAST` shared syntax |
| GC03 | CREATE GRAPH TYPE | ✓ (basic) | graph type → node/rel table schema bridge; multi-label node types and `LIKE <graph>` ✗ |

Quantifier bounds follow the GQL/Neo4j semantics — note that GQL `*` is
**zero**-or-more (`[e*0..]` in Cypher; Cypher's bare `*` is one-or-more), `+`
is one-or-more, `?` is `{0,1}`. Lower bound 0 binds start = end with an empty
edge list. `DIFFERENT EDGES` over a **single** path pattern maps to the TRAIL
machinery (whole-path edge distinctness); over multiple patterns it is
rejected (cross-pattern edge disjointness is deliberately out of scope).
`REPEATABLE ELEMENTS` is dropped (LadybugDB MATCH already allows edge
repetition). Multi-hop QPPIs with **bounded** quantifiers are expanded by
repetition (see difference 23 for the ranged-representation restrictions);
unbounded multi-hop repetition is a translator expressiveness ceiling and is
rejected loudly.

### Function-name mapping

Most GQL function names match LadybugDB's catalog directly
(`UPPER`/`LOWER`/`CEILING`/`LN`/`COUNT`/`SUM`/...). Divergent spellings are
mapped (table adapted from Neo4j's `GQLAliasFunctionNameRewriter`, Apache-2.0 —
see `THIRD_PARTY_NOTICES.md`):

| GQL | LadybugDB |
|---|---|
| `COLLECT_LIST` | `COLLECT` |
| `PERCENTILE_DISC` / `PERCENTILE_CONT` | `PERCENTILEDISC` / `PERCENTILECONT` |
| `CHAR_LENGTH` / `CHARACTER_LENGTH` | `SIZE` |
| `LOCAL_DATETIME` / `ZONED_DATETIME` | `TIMESTAMP` |
| `PATH_LENGTH` | `LENGTH` (path values; quantified element bindings are lists — use `SIZE`) |
| `ELEMENT_ID` | `internal_id` |
| `LOCAL_TIME` / `ZONED_TIME` | ✗ (no TIME type in LadybugDB) |
| `STDDEV_SAMP` / `STDDEV_POP` | ✗ (no such aggregate in LadybugDB) |

`||` (GQL string concatenation) maps to `+`.

GQL `MAX`/`MIN` translate to the in-extension aggregates `_gql_max`/`_gql_min`
(registered by this extension), which implement the GQL total order: numbers
compare numerically across INT/DOUBLE, lists lexicographically, and cross-class
ranking follows the TCK pin `null < bool < array < string < number < object`
(see difference 20). Result type follows the input type, so display of native
columns is unchanged. Heterogeneous / nested-list `FOR` sources are wrapped
element-wise in `_gql_to_json(ANY) → JSON` so their element types survive
LadybugDB's list homogenization (see difference 17).

GQL `SUM`/`AVG` likewise translate to the in-extension aggregates
`_gql_sum`/`_gql_avg`. Typed inputs produce exactly what native `SUM`/`AVG`
produce (INT128/UINT128/DOUBLE, display included — the parity tests compare
against the native aggregates directly); JSON inputs (ANY-graph properties,
`_gql_to_json`-wrapped values) return JSON that preserves int-ness (`sum` of
integers prints `75`, of mixed numbers `3.5`; `avg` is always real, `2.0`).
Nulls (SQL NULL and JSON `null`) are skipped; empty groups are NULL, matching
LadybugDB's native `SUM`.

On ANY graphs the comparison operators `<` `<=` `>` `>=` `=` `<>` and `ORDER
BY` keys translate to the in-extension predicates `_gql_lt/le/gt/ge/eq/ne`
(`ANY, ANY) → BOOL`) and the sort key `_gql_sortkey(ANY) → STRING`, which
implement the GQL total order (see difference 21). Typed graphs keep the
engine's native operators byte-for-byte.

### Known semantic differences

1. **REMOVE property** is approximated as `SET n.prop = NULL` — on LadybugDB's
   fixed schema the property column still exists (as NULL); ISO GQL "property
   removed" would make `PROPERTY_EXISTS`/`properties()` behave differently.
   The layer's `PROPERTY_EXISTS` mapping (G115 → `(v.prop IS NOT NULL)`) is the
   same fixed-schema model, so removed reads as not present consistently
   (`properties()` still lists every column).
2. **SET assignment order** — GQL evaluates all RHS values before assigning
   (order-independent); Cypher `SET` is sequential. The translator snapshots
   RHS values via `WITH` when items interfere; otherwise direct mapping is
   equivalent.
3. **FROM GRAPH / SESSION SET GRAPH** route through `USE GRAPH`, which is
   session-sticky — the session's current graph stays switched after
   `CALL GQL` returns.
4. **Transaction-wrapped programs** (`START TRANSACTION <body> COMMIT` in one
   call) are rejected: the engine rewrite path would expose the COMMIT message
   instead of query rows. Issue BEGIN/COMMIT as separate statements.
5. **NEXT composition** across statements is not supported (Cypher
   multi-statements do not share binding scope).
6. `SELECT *` with GROUP BY/HAVING is not supported (use explicit items).
7. **Path modes are enforced exactly** (ISO GQL TRAIL = all edges distinct,
   ACYCLIC = all nodes distinct). A single var-length segment maps onto the
   engine's `*TRAIL`/`*ACYCLIC` recursive types — note the engine's `*ACYCLIC`
   alone only distincts intermediate nodes (start/end unconstrained) — and
   every ACYCLIC pattern, plus any TRAIL pattern with more than one edge, also
   binds a path variable and filters with `IS_TRAIL`/`IS_ACYCLIC` over the
   whole path (closed walks like `1 -> 2 -> 1` are correctly excluded).
8. **GQL has no `LENGTH()`** — ISO GQL spells it `PATH_LENGTH()` for paths
   (mapped to LadybugDB `LENGTH`). A bare `length(...)` call is a GQL syntax
   error at the parser level.
9. **Edge directions**: GQL's undirected / mixed-direction edge spellings
   (`~[e]~`, `<-[e]~`, `~[e]->`, `<->[e]`, ...) collapse to LadybugDB's
   any-direction `-[e]-` (a directed property graph has no undirected edges to
   distinguish).
10. **Graph types are per-database memory**: `CREATE GRAPH TYPE` registers the
    canonicalized type in extension state attached to the open database
    (LadybugDB has no graph-type catalog object). Types survive across
    `CALL GQL` calls and follow catalog semantics within one database (so
    independent sessions/tests do not leak types into each other), but they are
    not persisted to WAL — re-run `CREATE GRAPH TYPE` after a restart. Graph
    *names* and their schemas are durable; only the type registry is not.
11. **Synthetic primary key**: expanding a graph type adds
    `_gql_id SERIAL PRIMARY KEY` to every node table (GQL node types have no key
    property; LadybugDB node tables require one). The column is auto-filled on
    INSERT and is a regular hidden-ish property; `_gql_id` is rejected as a
    user property name.
12. **`NOT NULL` is dropped**: GQL property types may carry `NOT NULL`; LadybugDB
    DDL has no NOT NULL constraint, so it is accepted and ignored.
13. **Multi-label semantics depend on the graph kind.** Typed/tabled graphs
    give a node exactly one label (= table name), so a conjunction of distinct
    labels (`:A&B`) matches nothing and multi-label `INSERT` is rejected (the
    engine would silently create nothing there). Open ANY graphs store a label
    set (`STRING[]`) and carry real multi-label nodes: `INSERT (n:A&B ...)`,
    `MATCH (n:A&B)`, `:A|B`, `:!A` all work there. Compound label expressions
    translate to WHERE predicates over `labels(v)` chosen by graph kind (ANY:
    `list_contains`; typed: scalar equality); the operators themselves are
    graph-kind independent (`&` is AND, `|` is OR on both kinds). Multi-label
    node *types* in `CREATE GRAPH TYPE` remain unsupported. Undirected edge
    types are rejected too (LadybugDB rel tables are directed).
    **`:A:B` colon chains are not GQL label syntax at all** — ISO GQL spells
    conjunction only as `&` (`<label conjunction> ::= <label term> <ampersand>
    <label factor>`; an element pattern filler holds one label-expression
    slot), so `MATCH (n:A:B)` is a GQL *syntax error* rejected at parse by
    `CALL GQL` (use `:A&B`/`:A|B`; same for `INSERT (n:A:B ...)`). The
    three-way fork lives in the target dialects, not in this layer: Cypher 25
    reads `:` as AND (`labelExpression3: ... (AMPERSAND | COLON) ...`), while
    LadybugDB's engine reads `:A:B` as AND on ANY graphs but table union (OR)
    on typed graphs. The translator therefore renders label expressions only
    from `&`/`|`/`!` and never emits a colon chain into a match/predicate
    position; the one colon chain it emits is multi-label `INSERT` on an ANY
    graph, where `:A:B` is a creation-time label set (it stores both labels),
    not a predicate.
14. **`CREATE GRAPH g t` spelling**: ISO GQL references a graph type by bare
    name; the commonly generated `CREATE GRAPH g TYPE t` is accepted as a
    lenient pre-parse normalization (same idea as `FROM GRAPH`).
    `CREATE GRAPH TYPE t AS COPY OF u` is reinterpreted past a grammar
    ambiguity (unquoted `TYPE` can be a graph name).
15. **GQL has no bare `TIME` type** — only `LOCAL TIME`, `ZONED TIME`,
    `TIME [WITH|WITHOUT TIME ZONE]`. LadybugDB has no TIME type at all; all
    spellings are rejected.
16. **Unnamed result columns** are named after their GQL source text
    (`RETURN max(x)` → column `max(x)`), which is the GQL convention;
    LadybugDB's engine alone would uppercase function names.
17. **Mixed-type list literals**: GQL list literals preserve per-element types
    (`[1, 2.0]` holds an INT64 and a DOUBLE); LadybugDB homogenizes mixed
    literals to one element type at bind time (STRING is the universal sink),
    which silently erases types. **In `FOR` sources** the layer now wraps every
    element of a heterogeneous literal (or a literal whose elements are all
    lists) in `_gql_to_json`, preserving each element's type as JSON — so
    `FOR x IN [1, 2.0, 5, null, 3.2, 0.1] RETURN max(x)` works (see difference
    20). Everywhere else untyped literals whose element classes disagree (INT
    vs DOUBLE count as different classes; `null` ignored) are still rejected
    with `GQL feature not supported: heterogeneous list literal`, as is a mixed
    list nested inside a wrapped `FOR` element. **Non-literal elements on
    typed graphs** (Q5-3, 2026-10-02) are now covered at *bind* time: a list
    literal containing at least one non-literal element is rewritten to
    `_gql_list_checked(e1, …)`, whose bindFunc sees each argument's
    engine-derived type **before** list homogenization and raises `GQL feature
    not supported: heterogeneous list element types` when the classes differ —
    the runtime residual is closed for this shape. Known residuals (loud or
    structural, none silent on the covered shapes): lists inside **aggregate
    arguments** are not rewritten (the rewriter stops at aggregate boundaries);
    ANY graphs are exempt (JSON dynamic columns — `FOR` sources keep the
    `_gql_to_json` wrapping instead); ORDER BY keys and SET/CREATE property
    values still emit source text. **Map/record
    values** (`{}`, `{k: v}`) in expressions are rejected too (`... map value`)
    — LadybugDB expressions have no map type.
18. **`FILTER` maps to `WITH * WHERE`** so it composes after `FOR`/`MATCH`;
    result semantics are unchanged.
19. **Boolean operators do type-check their operands**: `123 AND true` raises
    a BinderException in LadybugDB as GQL requires (earlier TCK reports claimed
    otherwise — that was a test-harness regex bug matching multi-line errors,
    now fixed).
20. **`MAX`/`MIN` implement the GQL total order** via the extension aggregates
    `_gql_max`/`_gql_min`: numbers compare numerically across INT/DOUBLE (the
    winning element keeps its own type — `max([1, 2.0, 5])` is the INT `5`), lists
    compare element-wise lexicographically, and cross-class ranking is
    `null < bool < array < string < number < object`, pinned by TCK
    `Aggregation2` [11]/[12] (`max` over mixed values = `1`, `min` = `[1,2]`) —
    note this puts arrays below strings below numbers, which differs from the
    Cypher orderability CIP for those three classes; the TCK is treated as the
    executable spec here. Structurally distinct JSON objects are un-orderable
    and raise a loud error rather than guessing. Numeric aggregates over
    **ANY-graph JSON property columns** (e.g. `sum(p.age)`) work through the
    same extension aggregates (`_gql_sum`/`_gql_avg`, see the function-mapping
    note above) — that former gap is closed.
21. **Comparison and ordering over ANY-graph JSON property columns use the
    GQL total order via the comparison bridge** (Phase 10, 2026-10-02).
    Before the bridge the engine compared the stored JSON *text*: `WHERE
    p.age >= 100` matched `33`, `9`, `33.5` (`'33' >= '100'` lexicographically)
    and `p.age = 33.0` was false against the stored `33` — text order is not
    value order, a silent wrong answer (probed 2026-10-01, sentinels 9 vs 100).
    On ANY graphs the translator now splices `<` `<=` `>` `>=` `=` `<>` to
    `_gql_lt/le/gt/ge/eq/ne` and wraps `ORDER BY` keys in `_gql_sortkey`.
    The bridge classifies each operand by its logical type (JSON text is
    parsed; unparsable text is the bare string the property writer stores —
    `x`, not `"x"`; the engine's `True`/`False` BOOL→JSON spelling is a
    boolean), compares numbers as exact decimal text (arbitrary length, never
    through double — `9007199254740993` and `9007199254740994` stay ordered),
    follows the rank `null < bool < array < string < number < object` pinned
    by TCK Aggregation2 [11]/[12], yields NULL when either side is SQL NULL
    (three-valued, `NOT(UNKNOWN)` stays UNKNOWN), and loud-rejects what has
    no order: structurally distinct JSON objects, DATE/UUID/INTERVAL/SERIAL/
    DECIMAL/STRUCT/MAP operands, and non-finite typed reals. Arithmetic was
    already loud-or-correct (`p.age + 1` on `33.5` raises ConversionException;
    `CAST(33.5 AS INT64)` fails loudly). Known residual edges (all loud or
    out-of-reach, none silent): comparisons inside a searched-CASE `WHEN`
    operand (`CASE x WHEN > 5`) and inside aggregate arguments are not
    spliced; `ORDER BY ... DESC` sorts NULLs first (engine DESC convention);
    nested arrays compare via the Phase 8 array comparator, whose >int64
    integer-vs-real path folds through double; a bare string that happens to
    spell JSON (`"true"`-like bare text) is inherently ambiguous — the
    storage erased the type, and the parse-first rule wins.
22. **Schema namespaces are simulated in the extension registry** (Phase 11,
    2026-10-02). `CREATE SCHEMA` / `DROP SCHEMA` (with `IF [NOT] EXISTS`)
    maintain a per-database path set (`/myschema`, `/foo/myschema`); a
    *directory* is any non-empty proper path prefix of a registered schema —
    there is no directory entity. Qualified object names (`CREATE GRAPH
    /foo/g ANY`, `CREATE GRAPH TYPE /foo/t {...}`, `DROP GRAPH /foo/g`,
    `SESSION SET GRAPH /foo/g`) are rewritten to physical names
    (`_gqlsch__foo__g`) through a bidirectional logical↔physical map; a
    user-spelled identifier starting with `_gqlsch__` is loud-rejected.
    Catalog error conditions (create existing / name identifies a directory,
    graph or graph type; drop missing / directory / graph / graph type /
    non-empty) raise `[42000]`, and a schema statement combined with other
    statements in one program (including `NEXT`) raises `[42000]`; a
    transaction-wrapped program whose transaction is `READ ONLY` raises
    `[25G03]`, other transaction wrappers keep the untagged message. Error
    tagging is deliberately narrow: only these schema/transaction rejections
    carry codes (the corpus pins exactly these); engine passthrough errors
    stay untagged, and a wrong tag is worse than none — the TCK harness
    fails a scenario whose expected code differs from an emitted one.
    Known edges: the registry is not WAL-persisted (like the graph-type
    registry, difference 10 — restart loses schemas, DDL is re-runnable).
    Relative qualified names (`dir.name`) are root-resolved (Q5-4,
    2026-10-02): the session schema is invariantly root because `SESSION SET
    SCHEMA` is loud-rejected, so `dir.name` → `/dir/name` is exact under this
    invariant (not an approximation) and shares the same mangling — `dir.g` and
    `/dir/g` name one physical graph; an absolute reference with a dotted tail
    (`/dir/x.g`) normalizes to `/dir/x/g` (spelling change vs earlier builds).
    Selecting a
    qualified graph: GQL's `USE` is a **prefix clause of query/data-modifying
    statements** and takes no `GRAPH` keyword — `USE /foo/g MATCH (n) RETURN
    n` works (rewritten through the same path as `SESSION SET GRAPH`, which
    is the session-level spelling). Standalone `USE <graph>` is not a GQL
    statement, and `USE GRAPH x` misparses: `GRAPH` is a nonReserved word, so
    it is read as a graph *named* "GRAPH" (do not type it; a `USE GRAPH x →
    USE x` leniency would eat legal graph names and is deliberately not
    added). Conflict checks ("identifies a graph") consult both the registry
    and the engine catalog (`getGraphEntries`), so graphs created outside
    `CALL GQL` are caught; label-set cardinality rejections carry the corpus
    GQLSTATUS pins (`[22G0N]` anonymous = 0 labels < min 1, `[22G0P]`
    multi-label > max 1).

23. **Multi-hop quantified path patterns expand by repetition** (2026-10-02).
    A bounded quantifier over a multi-edge or otherwise non-collapsible
    interior `( ()-[]->()-[]->() ){m,n}` is translated by unrolling the
    interior: `{n}`/`{m,m}` unrolls inline inside the single query (no
    restrictions — aggregates, `ORDER BY`, writes, `OPTIONAL MATCH` all fine);
    a true range `{m,n}` (1 ≤ n-m ≤ 4, lower bound ≥ 1) re-translates the
    **whole statement** once per count and joins the branches with `UNION ALL`
    (`UNION` when the projection is DISTINCT, giving the global dedup GQL
    asks for). Because a Cypher union is top-level only (no wrapper clause),
    the ranged form is loud-rejected with aggregation/`GROUP BY`/`HAVING`,
    `ORDER BY`/`SKIP`/`LIMIT`, `OPTIONAL MATCH` (where a post-WITH predicate
    would drop null-extended rows) and data-modifying statements; at most 8
    branch combinations (cartesian product over ranged factors) are expanded.
    Unbounded multi-hop (`*`, `+`, `{m,}`) and lower-bound-0 multi-hop (`?`,
    `{0,n}` — zero repetitions identify the two seam nodes, which needs node
    equality the expansion deliberately avoids) stay rejected, as do
    node-quantified patterns `(n){q}` and nested quantifiers.
    **Quantified element variables bind to lists** (one entry per repetition,
    as ISO GQL requires): a single-edge interior's edge variable is re-bound
    through `relationships(e)`; multi-edge interiors bind per-repetition
    names and construct the list explicitly (seam nodes are one node shared
    by the adjacent entries; a seam's outer pattern name wins, so `RETURN a`
    still sees `a` while `x`'s first entry *is* `a`). Junction labels /
    properties must be identical or empty on one side — a conflict is
    loud-rejected. `PATH_LENGTH` maps to `LENGTH`, which only accepts path
    values: use `size(e)` on a quantified element binding (a list) and
    `PATH_LENGTH(p)` on a path variable. Quantified bindings are lists and
    have no properties — `x.prop` is loud-rejected. Finally, a **bare star
    projection with any generated pattern binding** (quantified slot renames,
    per-repetition names, compound-label `_gql_nlN`, path-mode wrap
    `_gql_ppN`) is rejected: those synthetic names would leak into `*` as
    columns the GQL source never declared (a silently wrong column set).
    Known residuals: an inline element `WHERE` inside a quantified interior
    is rejected (per-repetition expression renaming is deferred); two
    juxtaposed *user* variables (`(a)(b)`) remain rejected as before;
    interior bindings must not be re-declared elsewhere as pattern variables
    (loud). Engine-side note: `bindGraphPattern` now carries the path
    variable's alias (like node/rel variables) so `MATCH p = ... WITH *`
    star expansion accepts it — a naming fix, not a semantics change.

### Graph type → schema mapping

| GQL graph type | LadybugDB DDL |
|---|---|
| `CREATE GRAPH g ANY` | `CREATE GRAPH g ANY` (open graph) |
| `NODE Person {name STRING, age INT64}` / `(:Person {name STRING, age INT64})` | `CREATE NODE TABLE Person(_gql_id SERIAL PRIMARY KEY, name STRING, age INT64)` |
| `EDGE KNOWS CONNECTING (Person TO Person) {since INT64}` / `(:Person)-[:KNOWS {since INT64}]->(:Person)` | `CREATE REL TABLE KNOWS(FROM Person TO Person, since INT64)` |
| `CREATE GRAPH TYPE t AS { ... }` | registered in the extension's type registry |
| `CREATE GRAPH g t` / `CREATE GRAPH g { ... }` | `CREATE GRAPH g; USE GRAPH g;` + the table DDL above |

Property value types map as: `BOOL`/`BOOLEAN`→BOOL, `STRING`/`CHAR`/`VARCHAR`→STRING,
`BYTES`/`BINARY`/`VARBINARY`→BLOB, `INT8..64`/`INTEGER8..64`/`SMALLINT`/`INT`/
`BIGINT`/`INTEGER`→INT8..64/INT16/INT32/INT64, `UINT*` likewise, `FLOAT32`/`FLOAT`/
`REAL`→FLOAT, `FLOAT64`/`DOUBLE`→DOUBLE, `DECIMAL(p,s)`→DECIMAL(p,s), `DATE`→DATE,
`TIMESTAMP`/`LOCAL DATETIME`→TIMESTAMP, `TIMESTAMP WITH TIME ZONE`/`ZONED DATETIME`
→TIMESTAMP_TZ, `DURATION(...)`→INTERVAL. Length/precision qualifiers on strings and
`FLOAT` are ignored. Everything else (TIME forms, lists, structs, ANY, ...) is
rejected explicitly.

## Architecture

```
GQL text → ANTLR GQL parser (vendored opengql grammar, ISO-derived)
        → GqlToCypherTransformer (explicit per-statement mapping)
        → Cypher text → engine's standalone-call rewrite → normal pipeline
```

See `src/gql_transformer.cpp` for the mapping logic and
`THIRD_PARTY_NOTICES.md` for adapted upstream work.

## Conformance (opengql/tck)

The vendored [opengql/tck](https://github.com/opengql/tck) suite (Apache-2.0,
see `test/tck/NOTICE.md`) is executed by `test/tck/run_tck.py`, which converts
the Gherkin scenarios to the same e2e harness the hand-written suite uses.

Measured on 2026-10-02 (untyped-graph mode; `python extension/gql/test/tck/run_tck.py`):

| Feature area | run | passed | passed-with-note | failed | skipped |
|---|---|---|---|---|---|
| expressions / boolean | 149 | 31 | 118 | 0 | 1 |
| expressions / aggregation | 16 | 13 | 0 | 3 | 0 |
| catalog / create graph types | 9 | 6 | 1 | 2 | 6 |
| catalog / create graphs | 8 | 3 | 1 | 4 | 0 |
| catalog / create+drop schemas | 16 | 16 | 0 | 0 | 0 |
| debug | 1 | 1 | 0 | 0 | 0 |
| **total** | **199** | **70** | **120** | **9** | **7** of 206 |

Exception scenarios assert the corpus's GQLSTATUS code in three tiers
(regex match = pass; error without any bracketed code = pass-with-note;
different code = fail). 70 scenarios pass with codes or without error
assertions; 120 pass-with-note are error scenarios whose error is loud but
carries no GQLSTATUS code yet (engine passthrough errors — Binder,
Conversion, … — are intentionally untagged; see difference 22).

Failure classes: parse-error 4 (TCK setup uses openCypher `CREATE (...)` /
`UNWIND`, which is not GQL — GQL writes `INSERT`/`FOR`; plus one
`CREATE GRAPH ANY AS COPY OF` grammar ambiguity), rejected-by-layer 5
(`LIKE`/`AS COPY OF` copy-statement forms, multi-label node types —
unsupported by design). All failures are
loud: the layer has **no silent wrong-answer class**. One passing scenario
(`Aggregation3` [1]) is checked values-only — the vendored corpus's expected
header row contradicts its own query (`n.name|sum(n.num)` vs `p.name,
sum(p.age)`), so no implementation could pass a column-name check on it; the
feature files are left unmodified and REPORT.md discloses the exception.
Corpus-side exceptions are likewise disclosed in REPORT.md, each with the
feature files left unmodified: `Create1` schemas [7] re-reads its When as
`CREATE SCHEMA IF NOT EXISTS` (the shipped text omits the keyword while its
title and `+schemas | 0` require it); `graph-types Create1` [6] is titled
"duplicate property names" but its body lists distinct properties under a
multi-label set (a copy-paste of the [4] body) — its pinned 42000 is demoted
to the note tier for that scenario only; `$(randomLabelSet(...))` templates
(graph-types [7][8], the only two in the corpus) are substituted semantically
(engine label cardinality 1/1); and `data/catalogs/catalog-1.gql` (referenced
by `drop1` [1][2]) is restored as input data the vendored copy omitted.
See `test/tck/REPORT.md` for the per-scenario listing and methodology
(exception scenarios assert that *an* error is raised — GQLSTATUS codes are not
emitted yet; side effects are checked only where observable).

Skipped scenarios are either capability-tagged for features LadybugDB does not
have (`@MinNodeLabelsZero`, `@MaxNodeLabelsGTOne`, ...) or reference sample data
the TCK repo does not ship.

## Tests

Dual-run parity tests: every GQL case runs next to the equivalent Cypher and
asserts the same expected result.

```sh
# from repo root (e2e_test built with -DBUILD_EXTENSIONS="gql")
E2E_TEST_FILES_DIRECTORY=extension ./build/.../e2e_test --gtest_filter="gql~test~test_files~*"
# or
make extension-test

# TCK conformance run (writes test/tck/REPORT.md)
python extension/gql/test/tck/run_tck.py
```
