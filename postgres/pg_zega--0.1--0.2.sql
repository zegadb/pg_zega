-- 0.1 to 0.2: journal mode (APS 52).

-- Journal mode (zega.mode = journal): one row per transaction that changed
-- what Zega holds, written in that transaction. The relay delivers each row
-- to Zega in commit order, then deletes it. flush is what zega.import
-- empties first ({"all": true}, or "labels" and "relationships").
CREATE TABLE journal (
    txid xid8 PRIMARY KEY,
    flush jsonb,
    schema text,
    document text NOT NULL,
    written_at timestamptz NOT NULL DEFAULT clock_timestamp()
);

CREATE OR REPLACE FUNCTION flush_node(label text)
RETURNS void
LANGUAGE plpgsql
AS $$
DECLARE
    graph jsonb;
    node record;
BEGIN
    -- Journal mode names the type: Zega finds its nodes when the entry is
    -- delivered, after every earlier entry.
    IF current_setting('zega.mode', true) = 'journal' THEN
        PERFORM zega.enqueue('FN ' || label);
        RETURN;
    END IF;
    graph := zega.read_graph();
    FOR node IN
        SELECT elem
        FROM jsonb_array_elements(COALESCE(graph->'nodes', '[]'::jsonb)) AS elem
        WHERE elem->'labels' ? label
    LOOP
        PERFORM zega.enqueue('DN ' || (node.elem->>'id'));
    END LOOP;
END;
$$;

CREATE OR REPLACE FUNCTION flush_edge(field text)
RETURNS void
LANGUAGE plpgsql
AS $$
DECLARE
    graph jsonb;
    rel record;
BEGIN
    IF current_setting('zega.mode', true) = 'journal' THEN
        PERFORM zega.enqueue('FR ' || field);
        RETURN;
    END IF;
    graph := zega.read_graph();
    FOR rel IN
        SELECT elem
        FROM jsonb_array_elements(COALESCE(graph->'rels', '[]'::jsonb)) AS elem
        WHERE elem->>'type' = field
    LOOP
        PERFORM zega.enqueue('DR ' || (rel.elem->>'id'));
    END LOOP;
END;
$$;

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
    PERFORM zega.prepare_reload();
    -- No tracked table may change while the import reads it, and each read
    -- below takes a new snapshot after these locks: whatever committed before
    -- this import is in it, and whatever commits after it follows it, in
    -- Zega too. Run it at the default isolation, read committed.
    FOR relid IN SELECT t.relid FROM zega.tracked t ORDER BY t.relid
    LOOP
        EXECUTE format('LOCK TABLE %s IN SHARE MODE', relid::regclass);
    END LOOP;
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
