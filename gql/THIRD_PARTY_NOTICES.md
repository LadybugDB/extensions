# Third-Party Notices — GQL extension

This extension's GQL→Cypher translation layer adapts mapping rules and
conformance knowledge from the following open-source projects. We gratefully
acknowledge their authors.

## Neo4j Cypher front-end (neo4j/neo4j)

- Source: https://github.com/neo4j/neo4j
- Files consulted / adapted:
  - `community/cypher/front-end/rewriting/src/main/scala/org/neo4j/cypher/internal/rewriting/rewriters/astRewriters/GQLAliasFunctionNameRewriter.scala`
    — GQL function-alias → Cypher function mapping table (re-targeted to
    LadybugDB's own function catalog in `gql_transformer.cpp`
    `GQL_FUNCTION_MAP`).
  - `PropertyExistsToIsNotNull.scala` — PROPERTY_EXISTS → IS NOT NULL rewrite
    semantics (GQL feature G115).
  - `LabelExpressionPredicateNormalizer.scala`, `NormalizeHasLabelsAndHasType.scala`
    — label-expression normalization approach.
  - `AddElementUniquenessPredicates.scala`, `AddPathPredicates.scala` —
    quantified-path-pattern quantifier bounds (`getLowerBound`/`getUpperBound`:
    `*`→0..∞, `+`→1..∞, `{m,n}`→m..n) and path-mode uniqueness semantics
    (WALK/TRAIL/ACYCLIC), adapted for the single-hop QPPI → var-length mapping
    in `gql_transformer.cpp` `quantifierRange`.
  - `GraphTypeCanonicalizer.scala` — canonical graph-type form (named node
    types with property types + named edge types with endpoint pairs, local
    aliases stripped), adapted for the `GraphTypeSpec` model in
    `gql_transformer.cpp` (CREATE GRAPH TYPE → node/rel table schema bridge).
  - `front-end/expressions/.../PathMode.scala` — GQL path-mode semantics
    reference (`PathMode.effectivePathMode`).
  - `community/neo4j-gql-status/` — GQL-status error envelope conventions.
- License: Apache License 2.0
- Copyright (c) "Neo4j" Neo4j Sweden AB

Adapted portions are rewritten in C++ for LadybugDB's parse-tree model and
re-targeted to LadybugDB's Cypher dialect; the Apache-2.0 license terms apply
to the adapted mapping tables/rules. License text:
https://www.apache.org/licenses/LICENSE-2.0

## opengql / LDBC GQL Implementation Working Group (gql-antlr)

- Source: https://github.com/opengql/grammar
- Usage: `extension/third_party/opengql/GQL.g4` (vendored ANTLR grammar for
  ISO GQL, generated from the ISO BNF artifacts via gramgen and hand-tuned).
- License: Apache License 2.0

## opengql/tck (GQL Technology Compatibility Kit)

- Source: https://github.com/opengql/tck
- Usage: `extension/gql/test/tck/{features,data,NOTICE.md,LICENSE}`
  (vendored Gherkin conformance scenarios and sample data) and the
  `run_tck.py` runner built around them. Scenarios derived from the
  openCypher TCK retain their Neo4j attribution headers; see the vendored
  `NOTICE.md`.
- License: Apache License 2.0

## ISO/IEC 39075:2024 GQL standard

- The GQL language itself is defined by ISO/IEC 39075:2024. The machine-readable
  grammar artifact (`ISO_IEC_39075(en).bnf.txt` / `.bnf.xml` published at
  standards.iso.org) is used as the reference for language-scope decisions.
- The standard document itself is copyrighted by ISO; only its published
  grammar artifact is used as a language definition reference.

## Reference copies in this repository

Local copies of several sources listed above (Neo4j rewriters and the Cypher 25
grammar, the ISO GQL grammar artifact) are kept in `docs/gql_ref/` for
consulting while maintaining the translation layer; its `README.md` indexes
them and records which piece of `src/gql_transformer.cpp` each one informed.
The ISO grammar artifact is internal reference only and is not committed to
git.
