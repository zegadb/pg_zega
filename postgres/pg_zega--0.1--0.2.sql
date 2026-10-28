-- 0.1 to 0.2: journal mode (APS 52).

-- Journal mode (zega.mode = journal): one row per transaction that changed
-- what Zega holds, written in that transaction. The relay delivers each row
-- to Zega in commit order, then deletes it.
CREATE TABLE journal (
    txid xid8 PRIMARY KEY,
    schema text NOT NULL,
    document text NOT NULL,
    written_at timestamptz NOT NULL DEFAULT clock_timestamp()
);

-- Flush a scope in Zega, then load it from Postgres.
-- import() replaces the whole graph.
-- import(node) replaces that type and every tracked edge that touches it.
-- import(ze_edge) replaces that one relationship.
-- The flush runs before the new schema is stored, so a breaking schema
-- change can be committed in the same transaction.
CREATE OR REPLACE FUNCTION import(target regclass DEFAULT NULL)
RETURNS void
LANGUAGE plpgsql
AS $$
DECLARE
    relid oid;
    relname text;
    shp record;
    edge_id oid;
    edge_shp record;
    edges oid[] := ARRAY[]::oid[];
BEGIN
    IF current_setting('zega.mode', true) = 'journal' THEN
        RAISE EXCEPTION 'pg_zega import is not available in journal mode'
            USING HINT = 'Set zega.mode to precommit for the import.';
    END IF;
    PERFORM zega.prepare_reload();
    IF target IS NULL THEN
        IF NOT EXISTS (SELECT 1 FROM zega.tracked) THEN
            RAISE EXCEPTION 'pg_zega has nothing tracked to import';
        END IF;
        PERFORM zega.enqueue('R');
        PERFORM zega.enqueue_schema();
        FOR relid IN
            SELECT t.relid
            FROM zega.tracked t
            JOIN pg_class c ON c.oid = t.relid
            ORDER BY (left(c.relname, 3) = 'ze_'), c.relname
        LOOP
            PERFORM zega.import_rows(relid);
        END LOOP;
        RETURN;
    END IF;

    IF NOT EXISTS (SELECT 1 FROM zega.tracked WHERE tracked.relid = target) THEN
        RAISE EXCEPTION 'pg_zega table % is not tracked', target;
    END IF;
    SELECT c.relname INTO relname FROM pg_class c WHERE c.oid = target;
    SELECT * INTO shp FROM zega.shape(target);

    IF shp.kind = 'edge' THEN
        PERFORM zega.flush_edge(shp.field);
        PERFORM zega.enqueue_schema();
        PERFORM zega.import_rows(target);
        RETURN;
    END IF;

    FOR edge_id IN
        SELECT t.relid
        FROM zega.tracked t
        JOIN pg_class c ON c.oid = t.relid
        WHERE left(c.relname, 3) = 'ze_'
        ORDER BY c.relname
    LOOP
        SELECT * INTO edge_shp FROM zega.shape(edge_id);
        IF edge_shp.from_rel = relname OR edge_shp.to_rel = relname THEN
            PERFORM zega.flush_edge(edge_shp.field);
            edges := array_append(edges, edge_id);
        END IF;
    END LOOP;
    PERFORM zega.flush_node(relname);
    PERFORM zega.enqueue_schema();
    PERFORM zega.import_rows(target);
    FOREACH edge_id IN ARRAY edges
    LOOP
        PERFORM zega.import_rows(edge_id);
    END LOOP;
END;
$$;
