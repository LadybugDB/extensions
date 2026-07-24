#!/usr/bin/env python3
"""
Test script for pg_client extension using pgembed to spin up a temporary PostgreSQL instance.

Usage:
    E2E_TEST_FILES_DIRECTORY=extension/pg_client/test/test_files python3 -m pytest extension/pg_client/test/test_pg_client.py -v

    Or directly:
    python3 extension/pg_client/test/test_pg_client.py
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
            # Create node table
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

            # Create rel table
            conn.execute(sa.text("""
                CREATE TABLE rel_knows (
                    id SERIAL PRIMARY KEY,
                    from_id INTEGER NOT NULL,
                    to_id INTEGER NOT NULL,
                    since DATE
                )
            """))

            conn.execute(sa.text("""
                INSERT INTO rel_knows (from_id, to_id, since) VALUES
                    (1, 2, '2020-01-15'),
                    (1, 3, '2021-03-20'),
                    (2, 4, '2022-06-10'),
                    (3, 4, '2023-08-05'),
                    (4, 5, '2023-12-01')
            """))

            # Create a regular table (no prefix) for testing
            conn.execute(sa.text("""
                CREATE TABLE organisation (
                    id SERIAL PRIMARY KEY,
                    name VARCHAR(100),
                    revenue DOUBLE PRECISION
                )
            """))

            conn.execute(sa.text("""
                INSERT INTO organisation (name, revenue) VALUES
                    ('ACME Corp', 1000000.50),
                    ('Globex Inc', 2500000.75),
                    ('Initech', 500000.00)
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
        """Test scanning a node_* table from PostgreSQL."""
        script = f"""
        LOAD EXTENSION '{PG_CLIENT_EXT}';
        ATTACH '{self.conn_str}' AS testdb (DBTYPE PG_CLIENT);
        LOAD FROM testdb.node_person RETURN *;
        """
        stdout, stderr = run_lbug(script)
        self.assertIn("Alice", stdout, f"Scan failed: {stderr}")
        self.assertIn("Bob", stdout, f"Bob not found: {stderr}")
        self.assertIn("Charlie", stdout, f"Charlie not found: {stderr}")

    def test_04_scan_node_table_with_filter(self):
        """Test scanning with a filter condition."""
        script = f"""
        LOAD EXTENSION '{PG_CLIENT_EXT}';
        ATTACH '{self.conn_str}' AS testdb (DBTYPE PG_CLIENT);
        LOAD FROM testdb.node_person WHERE age > 30 RETURN name, age;
        """
        stdout, stderr = run_lbug(script)
        # Charlie (35) and Eve (32) are > 30
        self.assertIn("Charlie", stdout, f"Filter scan failed: {stderr}")
        self.assertIn("Eve", stdout, f"Eve not found: {stderr}")
        self.assertNotIn("Alice", stdout, "Alice should be filtered out")

    def test_05_scan_rel_table(self):
        """Test scanning a rel_* table from PostgreSQL."""
        script = f"""
        LOAD EXTENSION '{PG_CLIENT_EXT}';
        ATTACH '{self.conn_str}' AS testdb (DBTYPE PG_CLIENT);
        LOAD FROM testdb.rel_knows RETURN *;
        """
        stdout, stderr = run_lbug(script)
        # Should see the relationship data (Ladybug shell uses box-drawing chars)
        self.assertIn("2020-01-15", stdout, f"Rel scan failed: {stderr}")
        self.assertIn("2023-12-01", stdout, f"Rel scan missing last row: {stderr}")

    def test_06_regular_table_not_discovered(self):
        """Test that tables without node_/rel_ prefix are not discovered by default."""
        script = f"""
        LOAD EXTENSION '{PG_CLIENT_EXT}';
        ATTACH '{self.conn_str}' AS testdb (DBTYPE PG_CLIENT);
        LOAD FROM testdb.organisation RETURN *;
        """
        stdout, stderr = run_lbug(script)
        # organisation doesn't have node_/rel_ prefix, so it shouldn't be found
        # Ladybug prints errors to stdout with "Error:" prefix
        self.assertIn("Error", stdout, "Expected error for non-node/rel table")

    def test_07_match_query(self):
        """Test MATCH query on attached node table."""
        script = f"""
        LOAD EXTENSION '{PG_CLIENT_EXT}';
        ATTACH '{self.conn_str}' AS testdb (DBTYPE PG_CLIENT);
        MATCH (p:testdb.node_person) RETURN p.name, p.age ORDER BY p.age;
        """
        stdout, stderr = run_lbug(script)
        self.assertIn("Bob", stdout, f"MATCH failed: {stderr}")

    def test_08_show_tables(self):
        """Test SHOW_TABLES includes attached tables."""
        script = f"""
        LOAD EXTENSION '{PG_CLIENT_EXT}';
        ATTACH '{self.conn_str}' AS testdb (DBTYPE PG_CLIENT);
        CALL SHOW_TABLES() RETURN *;
        """
        stdout, stderr = run_lbug(script)
        self.assertIn("node_person", stdout, f"SHOW_TABLES missing node_person: {stderr}")
        self.assertIn("rel_knows", stdout, f"SHOW_TABLES missing rel_knows: {stderr}")

    def test_09_table_info(self):
        """Test TABLE_INFO on attached table."""
        script = f"""
        LOAD EXTENSION '{PG_CLIENT_EXT}';
        ATTACH '{self.conn_str}' AS testdb (DBTYPE PG_CLIENT);
        CALL TABLE_INFO('testdb.node_person') RETURN *;
        """
        stdout, stderr = run_lbug(script)
        self.assertIn("name", stdout.lower(), f"TABLE_INFO missing: {stderr}")
        self.assertIn("age", stdout.lower(), f"TABLE_INFO missing age: {stderr}")
        self.assertIn("email", stdout.lower(), f"TABLE_INFO missing email: {stderr}")

    def test_10_attach_with_schema(self):
        """Test ATTACH with non-default schema."""
        # First, create a separate schema with data
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


if __name__ == "__main__":
    unittest.main()
