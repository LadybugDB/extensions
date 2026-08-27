--
-- PostgreSQL database dump for pg_client extension tests.
--
-- Creates the expected schema and test data matching
-- extension/pg_client/test/test_files/pg_client.test
--

SET statement_timeout = 0;
SET lock_timeout = 0;
SET idle_in_transaction_session_timeout = 0;
SET client_encoding = 'UTF8';
SET standard_conforming_strings = on;
SELECT pg_catalog.set_config('search_path', '', false);
SET check_function_bodies = false;
SET xmloption = content;
SET client_min_messages = warning;
SET row_security = off;

--
-- Table: node_person
--

CREATE TABLE public.node_person (
    id integer NOT NULL PRIMARY KEY,
    name character varying(100) NOT NULL,
    age integer NOT NULL
);


ALTER TABLE public.node_person OWNER TO ci;

--
-- Table: rel_knows
--

CREATE TABLE public.rel_knows (
    id integer NOT NULL,
    from_id integer NOT NULL REFERENCES public.node_person(id),
    to_id integer NOT NULL REFERENCES public.node_person(id),
    since date NOT NULL
);


ALTER TABLE public.rel_knows OWNER TO ci;

--
-- Data for node_person
--

INSERT INTO public.node_person (id, name, age) VALUES
    (1, 'Alice',   30),
    (2, 'Bob',     25),
    (3, 'Charlie', 35),
    (4, 'Diana',   28),
    (5, 'Eve',     32);

SELECT pg_catalog.setval(pg_catalog.pg_get_serial_sequence('public.node_person', 'id'), 5, true);

--
-- Data for rel_knows
--

INSERT INTO public.rel_knows (id, from_id, to_id, since) VALUES
    (1, 1, 2, '2020-01-15'),
    (2, 1, 3, '2021-03-20'),
    (3, 2, 4, '2022-06-10'),
    (4, 3, 4, '2023-08-05'),
    (5, 4, 5, '2023-12-01');

SELECT pg_catalog.setval(pg_catalog.pg_get_serial_sequence('public.rel_knows', 'id'), 5, true);
