#!/usr/bin/env python3
"""
Test script for pg_client extension using pgembed to spin up a temporary PostgreSQL instance.

Tests both LOAD FROM scans and in-place Cypher queries (MATCH).
"""

import os
import sys
import tempfile
import subprocess
import unittest

import pgembed
import sqlalchemy as sa
from sqlalchemy_utils import database_exists, create_database


PG_CLIENT_EXT = os.path.join(
    os.path.dirname(__file__), "..", "build", "libpg_client.lbug_extension"
)
LBUG_BIN = os.path.join(
    os.path.dirname(__file__), "..", "..", "..", "build", "release", "tools", "shell", "lbug"
)


def run_lbug(script: str, lbug_bin: str = LBUG_BIN) -> str:
    """Run a ladybug script and return the output."""
    result = subprocess.run(
        [lbug_bin],
        input=script,
        capture_output=True,
        text=True,
        timeout=30,
    )
    return result.stdout, result.stderr


class TestPgClientExtension(unittest.TestCase):
    """Test the pg_client extension using a temporary PostgreSQL instance."""

    @classmethod
    def setUpClass(cls):
        """Start a temporary PostgreSQL instance and prepare test data."""
        cls.tmpdir = tempfile.mkdtemp()
        cls.pg = pgembed.get_server(cls.tmpdir)
        cls.database_name = "testdb"
        cls.uri = cls.pg.get_uri(cls.database_name)

        if not database_exists(cls.uri):
            create_database(cls.uri)

        engine = sa.create_engine(cls.uri, isolation_level="AUTOCOMMIT")
        conn = engine.connect()

        with conn.begin():
            # Create node_person table (detected as node table by prefix)
            conn.execute(sa.text("""
                CREATE TABLE node_person (
                    id SERIAL PRIMARY KEY,
                    name VARCHAR(100) NOT NULL,
                    age INTEGER,
                    email VARCHAR(100)
                )
            """))

            conn.execute(sa.text("""
                INSERT INTO node_person (name, age, email) VALUES
                    ('Alice', 30, 'alice@example.com'),
                    ('Bob', 25, 'bob@example.com'),
                    ('Charlie', 35, 'charlie@example.com'),
                    ('Diana', 28, 'diana@example.com'),
                    ('Eve', 32, 'eve@example.com')
            """))

            # Create node_company table (detected as node table by prefix)
            conn.execute(sa.text("""
                CREATE TABLE node_company (
                    id SERIAL PRIMARY KEY,
                    name VARCHAR(100),
                    revenue DOUBLE PRECISION
                )
            """))

            conn.execute(sa.text("""
                INSERT INTO node_company (name, revenue) VALUES
                    ('ACME Corp', 1000000.50),
                    ('Globex Inc', 2500000.75),
                    ('Initech', 500000.00)
            """))

            # Create rel_knows table with FK columns named src_id/dst_id
            # Prefix rel_ + FK constraints → auto-register as relationship table
            conn.execute(sa.text("""
                CREATE TABLE rel_knows (
                    id SERIAL PRIMARY KEY,
                    src_id INTEGER NOT NULL,
                    dst_id INTEGER NOT NULL,
                    since VARCHAR(20)
                )
            """))

            # Add actual FOREIGN KEY constraints so the FK query detects src/dst tables
            conn.execute(sa.text("""
                ALTER TABLE rel_knows
                    ADD CONSTRAINT fk_src FOREIGN KEY (src_id) REFERENCES node_person(id),
                    ADD CONSTRAINT fk_dst FOREIGN KEY (dst_id) REFERENCES node_person(id)
            """))

            conn.execute(sa.text("""
                INSERT INTO rel_knows (src_id, dst_id, since) VALUES
                    (1, 2, '2020-01-15'),
                    (1, 3, '2021-03-20'),
                    (2, 4, '2022-06-10'),
                    (3, 4, '2023-08-05'),
                    (4, 5, '2023-12-01')
            """))

            # Create rel_works_at table with FK constraints
            conn.execute(sa.text("""
                CREATE TABLE rel_works_at (
                    id SERIAL PRIMARY KEY,
                    src_id INTEGER NOT NULL REFERENCES node_person(id),
                    dst_id INTEGER NOT NULL REFERENCES node_company(id)
                )
            """))

            conn.execute(sa.text("""
                INSERT INTO rel_works_at (src_id, dst_id) VALUES
                    (1, 1),
                    (2, 2),
                    (3, 1),
                    (4, 3),
                    (5, 2)
            """))

        conn.close()
        cls.conn_str = cls.uri

    def test_01_load_extension(self):
        """Test that the pg_client extension can be loaded."""
        script = f"""
        LOAD EXTENSION '{PG_CLIENT_EXT}';
        RETURN "LOAD OK";
        """
        stdout, stderr = run_lbug(script)
        self.assertIn("LOAD OK", stdout, f"Extension load failed: {stderr}")

    def test_02_attach_database(self):
        """Test ATTACH to PostgreSQL via pg_client."""
        script = f"""
        LOAD EXTENSION '{PG_CLIENT_EXT}';
        ATTACH '{self.conn_str}' AS testdb (DBTYPE PG_CLIENT);
        RETURN "ATTACH OK";
        """
        stdout, stderr = run_lbug(script)
        self.assertIn("ATTACH OK", stdout, f"ATTACH failed: {stderr}")

    def test_03_scan_node_table(self):
        """Test LOAD FROM a node_* table."""
        script = f"""
        LOAD EXTENSION '{PG_CLIENT_EXT}';
        ATTACH '{self.conn_str}' AS testdb (DBTYPE PG_CLIENT);
        LOAD FROM testdb.node_person RETURN name, age ORDER BY id;
        """
        stdout, stderr = run_lbug(script)
        self.assertIn("Alice", stdout, f"Scan failed: {stderr}")
        self.assertIn("Bob", stdout, f"Bob not found: {stderr}")
        self.assertIn("Charlie", stdout, f"Charlie not found: {stderr}")
        self.assertIn("Diana", stdout, f"Diana not found: {stderr}")
        self.assertIn("Eve", stdout, f"Eve not found: {stderr}")

    def test_04_scan_node_table_with_filter(self):
        """Test LOAD FROM with filter pushdown."""
        script = f"""
        LOAD EXTENSION '{PG_CLIENT_EXT}';
        ATTACH '{self.conn_str}' AS testdb (DBTYPE PG_CLIENT);
        LOAD FROM testdb.node_person WHERE age > 30 RETURN name, age;
        """
        stdout, stderr = run_lbug(script)
        self.assertIn("Charlie", stdout, f"Filter scan failed: {stderr}")
        self.assertIn("Eve", stdout, f"Eve not found: {stderr}")
        self.assertNotIn("Alice", stdout, "Alice should be filtered out")

    def test_05_match_node_table(self):
        """Test MATCH query on node table (in-place)."""
        script = f"""
        LOAD EXTENSION '{PG_CLIENT_EXT}';
        ATTACH '{self.conn_str}' AS testdb (DBTYPE PG_CLIENT);
        MATCH (p:testdb.node_person) RETURN p.name, p.age ORDER BY p.age;
        """
        stdout, stderr = run_lbug(script)
        self.assertIn("Bob", stdout, f"MATCH failed: {stderr}")
        self.assertIn("Alice", stdout, f"MATCH missing Alice: {stderr}")
        self.assertIn("Charlie", stdout, f"MATCH missing Charlie: {stderr}")

    def test_06_load_rel_table(self):
        """Test LOAD FROM a rel_* table."""
        script = f"""
        LOAD EXTENSION '{PG_CLIENT_EXT}';
        ATTACH '{self.conn_str}' AS testdb (DBTYPE PG_CLIENT);
        LOAD FROM testdb.rel_knows RETURN src_id, dst_id, since ORDER BY id;
        """
        stdout, stderr = run_lbug(script)
        self.assertIn("2020-01-15", stdout, f"Rel load failed: {stderr}")
        self.assertIn("2021-03-20", stdout, f"Rel load missing: {stderr}")
        self.assertIn("2023-12-01", stdout, f"Rel load missing: {stderr}")

    def test_06b_match_rel_table(self):
        """Test MATCH query traversing a rel_* table (the documented pattern).

        Regression test: registering rel_* tables as discoverable node shadows
        used to break MATCH ... -[...]-> ... queries because the planner
        couldn't find a RelGroupCatalogEntry to anchor the scan. The rel
        table must now be registered as a RelGroupCatalogEntry and the
        ForeignRelTable must own a shared scan state to drive morsel-based
        parallelism across HashJoinBuild workers.

        Note: the count(*) form is the tightest correctness check for
        morsel-driven parallel scan — the rel table scan must visit every
        row exactly once across all worker threads. The projection form
        (RETURN a.name, b.name, k.since) additionally requires an ID
        mapping layer to translate PG foreign-key values into lbug
        internal node IDs; that is tracked separately.
        """
        script = f"""
        LOAD EXTENSION '{PG_CLIENT_EXT}';
        ATTACH '{self.conn_str}' AS testdb (DBTYPE PG_CLIENT);
        MATCH (a:node_person)-[k:rel_knows]->(b:node_person)
        RETURN count(*);
        """
        stdout, stderr = run_lbug(script)
        self.assertIn("5", stdout, f"MATCH rel count failed: {stderr}")

    def test_06c_match_rel_count_parallel(self):
        """Test count(*) over a MATCH ... -[...]-> ... join on a rel_* table.

        This is the tightest test for morsel-driven parallel correctness: the
        ForeignRelTable scan is driven by HashJoinBuild workers, so every
        joined row must be visited exactly once. A bug in the shared
        PgClientScanSharedState (e.g. per-thread state instead of per-table)
        would surface as either a wrong count or duplicated rows.
        """
        script = f"""
        LOAD EXTENSION '{PG_CLIENT_EXT}';
        ATTACH '{self.conn_str}' AS testdb (DBTYPE PG_CLIENT);
        MATCH (a:node_person)-[k:rel_knows]->(b:node_person)
        RETURN count(*);
        """
        stdout, stderr = run_lbug(script)
        self.assertIn("5", stdout, f"MATCH rel count failed: {stderr}")

    def test_07_scan_rel_table_filter(self):
        """Test LOAD FROM a rel_* table with filter."""
        script = f"""
        LOAD EXTENSION '{PG_CLIENT_EXT}';
        ATTACH '{self.conn_str}' AS testdb (DBTYPE PG_CLIENT);
        LOAD FROM testdb.rel_knows WHERE src_id = 1 RETURN dst_id, since ORDER BY dst_id;
        """
        stdout, stderr = run_lbug(script)
        self.assertIn("2020-01-15", stdout, f"Rel filter failed: {stderr}")
        self.assertIn("2021-03-20", stdout, f"Rel filter missing: {stderr}")

    def test_09_show_tables(self):
        """Test SHOW_TABLES includes node and rel tables."""
        script = f"""
        LOAD EXTENSION '{PG_CLIENT_EXT}';
        ATTACH '{self.conn_str}' AS testdb (DBTYPE PG_CLIENT);
        CALL SHOW_TABLES() RETURN *;
        """
        stdout, stderr = run_lbug(script)
        self.assertIn("node_person", stdout, f"SHOW_TABLES missing node_person: {stderr}")
        self.assertIn("rel_knows", stdout, f"SHOW_TABLES missing rel_knows: {stderr}")

    def test_10_table_info(self):
        """Test TABLE_INFO on attached table."""
        script = f"""
        LOAD EXTENSION '{PG_CLIENT_EXT}';
        ATTACH '{self.conn_str}' AS testdb (DBTYPE PG_CLIENT);
        CALL TABLE_INFO('testdb.node_person') RETURN *;
        """
        stdout, stderr = run_lbug(script)
        self.assertIn("name", stdout.lower(), f"TABLE_INFO missing name: {stderr}")
        self.assertIn("age", stdout.lower(), f"TABLE_INFO missing age: {stderr}")

    def test_11_attach_with_schema(self):
        """Test ATTACH with non-default schema."""
        engine = sa.create_engine(self.uri, isolation_level="AUTOCOMMIT")
        conn = engine.connect()
        with conn.begin():
            conn.execute(sa.text("CREATE SCHEMA IF NOT EXISTS custom_schema"))
            conn.execute(sa.text("""
                CREATE TABLE custom_schema.node_product (
                    id SERIAL PRIMARY KEY,
                    name VARCHAR(100),
                    price DOUBLE PRECISION
                )
            """))
            conn.execute(sa.text("""
                INSERT INTO custom_schema.node_product (name, price) VALUES
                    ('Widget', 9.99),
                    ('Gadget', 24.99),
                    ('Doohickey', 14.99)
            """))
        conn.close()

        script = f"""
        LOAD EXTENSION '{PG_CLIENT_EXT}';
        ATTACH '{self.conn_str}' AS customdb (DBTYPE PG_CLIENT, SCHEMA = 'custom_schema');
        LOAD FROM customdb.node_product RETURN *;
        """
        stdout, stderr = run_lbug(script)
        self.assertIn("Widget", stdout, f"Schema attach failed: {stderr}")
        self.assertIn("Gadget", stdout, f"Gadget not found: {stderr}")

    def test_12_regular_table_not_discovered(self):
        """Test that tables without node_/rel_ prefix are not exposed."""
        # Create a table without prefix
        engine = sa.create_engine(self.uri, isolation_level="AUTOCOMMIT")
        conn = engine.connect()
        with conn.begin():
            conn.execute(sa.text("""
                CREATE TABLE IF NOT EXISTS hidden_data (
                    id SERIAL PRIMARY KEY,
                    secret VARCHAR(100)
                )
            """))
            conn.execute(sa.text("""
                INSERT INTO hidden_data (secret) VALUES ('sensitive')
            """))
        conn.close()

        script = f"""
        LOAD EXTENSION '{PG_CLIENT_EXT}';
        ATTACH '{self.conn_str}' AS testdb (DBTYPE PG_CLIENT);
        LOAD FROM testdb.hidden_data RETURN *;
        """
        stdout, stderr = run_lbug(script)
        self.assertIn("Error", stdout, "Expected error for non-node/rel table")


if __name__ == "__main__":
    unittest.main()
