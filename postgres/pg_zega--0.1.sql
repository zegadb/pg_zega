CREATE TABLE tracked (
    relid oid PRIMARY KEY
);

CREATE FUNCTION enqueue(payload text)
RETURNS void
AS 'MODULE_PATHNAME', 'zega_enqueue'
LANGUAGE C STRICT;

CREATE FUNCTION zega_type(typname text)
RETURNS text
LANGUAGE sql
IMMUTABLE
AS $$
    SELECT CASE typname
        WHEN 'int2' THEN 'Int'
        WHEN 'int4' THEN 'Int'
        WHEN 'int8' THEN 'Int'
        WHEN 'float4' THEN 'Float'
        WHEN 'float8' THEN 'Float'
        WHEN 'bool' THEN 'Bool'
        WHEN 'text' THEN 'String'
        WHEN 'varchar' THEN 'String'
        WHEN 'bpchar' THEN 'String'
        ELSE NULL
    END
$$;

CREATE FUNCTION literal(ztype text, val jsonb)
RETURNS text
LANGUAGE plpgsql
IMMUTABLE
AS $$
DECLARE
    s text;
BEGIN
    IF val IS NULL OR val = 'null'::jsonb THEN
        RETURN 'null';
    END IF;
    IF ztype = 'String' THEN
        s := val #>> '{}';
        s := replace(s, E'\\', E'\\\\');
        s := replace(s, '"', E'\\"');
        s := replace(s, E'\n', E'\\n');
        s := replace(s, E'\t', E'\\t');
        RETURN '"' || s || '"';
    ELSIF ztype = 'Bool' THEN
        IF val = 'true'::jsonb THEN
            RETURN 'true';
        END IF;
        RETURN 'false';
    ELSIF ztype = 'Int' OR ztype = 'Float' THEN
        RETURN val #>> '{}';
    END IF;
    RAISE EXCEPTION 'pg_zega cannot format %', ztype;
END;
$$;

-- One foreign-key constraint is one endpoint. A composite key still counts as one.
CREATE FUNCTION endpoint(target oid, con oid, OUT ref_rel text, OUT cols text[], OUT typs text[], OUT keys text[])
RETURNS record
LANGUAGE plpgsql
AS $$
DECLARE
    relname text;
    ref oid;
    refname text;
    local_atts int2[];
    ref_atts int2[];
    i integer;
    col text;
    typ text;
    rkey text;
BEGIN
    SELECT c.relname INTO relname FROM pg_class c WHERE c.oid = target;
    SELECT fk.confrelid, fk.conkey, fk.confkey
      INTO ref, local_atts, ref_atts
    FROM pg_constraint fk
    WHERE fk.oid = con
      AND fk.conrelid = target
      AND fk.contype = 'f';
    IF ref IS NULL THEN
        RAISE EXCEPTION 'pg_zega relation % is missing a foreign key', relname;
    END IF;

    SELECT c.relname INTO refname FROM pg_class c WHERE c.oid = ref;
    IF NOT EXISTS (SELECT 1 FROM zega.tracked t WHERE t.relid = ref) THEN
        RAISE EXCEPTION 'pg_zega relation % references untracked table %', relname, refname;
    END IF;
    IF left(refname, 3) = 'ze_' OR NOT EXISTS (
        SELECT 1 FROM pg_index ix
        WHERE ix.indrelid = ref
          AND ix.indisprimary
          AND ix.indnkeyatts = 1
          AND ix.indkey[0] <> 0
    ) THEN
        RAISE EXCEPTION 'pg_zega relation % must reference a node table', relname;
    END IF;

    ref_rel := refname;
    cols := ARRAY[]::text[];
    typs := ARRAY[]::text[];
    keys := ARRAY[]::text[];
    FOR i IN 1 .. array_length(local_atts, 1) LOOP
        SELECT a.attname, y.typname INTO col, typ
        FROM pg_attribute a
        JOIN pg_type y ON y.oid = a.atttypid
        WHERE a.attrelid = target AND a.attnum = local_atts[i];
        SELECT a.attname INTO rkey
        FROM pg_attribute a
        WHERE a.attrelid = ref AND a.attnum = ref_atts[i];
        IF col !~ '^[A-Za-z_][A-Za-z0-9_]*$' THEN
            RAISE EXCEPTION 'pg_zega cannot use column name %.%', relname, col;
        END IF;
        IF rkey !~ '^[A-Za-z_][A-Za-z0-9_]*$' THEN
            RAISE EXCEPTION 'pg_zega cannot use column name %.%', refname, rkey;
        END IF;
        IF col = 'id' THEN
            RAISE EXCEPTION 'pg_zega cannot use column %.id; Zega reserves id for the engine id', relname;
        END IF;
        IF rkey = 'id' THEN
            RAISE EXCEPTION 'pg_zega cannot use column %.id; Zega reserves id for the engine id', refname;
        END IF;
        IF zega.zega_type(typ) IS NULL THEN
            RAISE EXCEPTION 'pg_zega cannot map %.% (%)', relname, col, typ;
        END IF;
        cols := array_append(cols, col);
        typs := array_append(typs, typ);
        keys := array_append(keys, rkey);
    END LOOP;
END;
$$;

-- A ze_ table is one relation. Exactly two foreign-key constraints are required,
-- so zero or one is an error and so is three or more. Each constraint is one
-- endpoint, including a composite foreign key. Primary-key order is the
-- direction. The primary key is exactly those endpoint columns. Every other
-- column is a property. The two nodes may be different types. There is no inverse.
CREATE FUNCTION shape(target oid, OUT kind text, OUT pk text, OUT field text, OUT from_rel text, OUT to_rel text, OUT from_cols text[], OUT from_typs text[], OUT from_keys text[], OUT to_cols text[], OUT to_typs text[], OUT to_keys text[])
RETURNS record
LANGUAGE plpgsql
AS $$
DECLARE
    relname text;
    nkeys integer;
    a0 integer;
    nfks integer;
    cons oid[];
BEGIN
    SELECT c.relname INTO relname FROM pg_class c WHERE c.oid = target;
    IF relname !~ '^[A-Za-z_][A-Za-z0-9_]*$' THEN
        RAISE EXCEPTION 'pg_zega cannot use table name %', relname;
    END IF;

    SELECT i.indnkeyatts, i.indkey[0]
      INTO nkeys, a0
    FROM pg_index i
    WHERE i.indrelid = target AND i.indisprimary;

    IF left(relname, 3) = 'ze_' THEN
        field := substring(relname from 4);
        IF field !~ '^[A-Za-z_][A-Za-z0-9_]*$' THEN
            RAISE EXCEPTION 'pg_zega cannot use relation name %', relname;
        END IF;
        SELECT count(*) INTO nfks
        FROM pg_constraint c
        WHERE c.conrelid = target AND c.contype = 'f';
        IF nfks <> 2 THEN
            RAISE EXCEPTION 'pg_zega relation % needs exactly two foreign-key constraints, found %', relname, nfks;
        END IF;
        IF EXISTS (
            SELECT 1
            FROM pg_constraint a
            JOIN pg_constraint b
              ON b.conrelid = a.conrelid
             AND b.oid > a.oid
             AND b.contype = 'f'
            WHERE a.conrelid = target
              AND a.contype = 'f'
              AND a.conkey && b.conkey
        ) THEN
            RAISE EXCEPTION 'pg_zega relation % foreign keys must not share a column', relname;
        END IF;
        IF nkeys IS NULL OR EXISTS (
            SELECT 1
            FROM pg_index i
            CROSS JOIN generate_series(1, i.indnkeyatts) AS g(n)
            WHERE i.indrelid = target
              AND i.indisprimary
              AND NOT EXISTS (
                  SELECT 1 FROM pg_constraint c
                  WHERE c.conrelid = target
                    AND c.contype = 'f'
                    AND i.indkey[g.n - 1] = ANY (c.conkey)
              )
        ) OR EXISTS (
            SELECT 1
            FROM pg_constraint c
            CROSS JOIN LATERAL unnest(c.conkey) AS u(att)
            WHERE c.conrelid = target
              AND c.contype = 'f'
              AND NOT EXISTS (
                  SELECT 1
                  FROM pg_index i
                  CROSS JOIN generate_series(1, i.indnkeyatts) AS g(n)
                  WHERE i.indrelid = target
                    AND i.indisprimary
                    AND i.indkey[g.n - 1] = u.att
              )
        ) THEN
            RAISE EXCEPTION 'pg_zega relation % primary key must be exactly the columns of its two foreign keys', relname;
        END IF;

        SELECT array_agg(s.oid ORDER BY s.pk_pos, s.oid) INTO cons
        FROM (
            SELECT fk.oid,
                (
                    SELECT min(g.n)
                    FROM pg_index i
                    CROSS JOIN generate_series(1, i.indnkeyatts) AS g(n)
                    WHERE i.indrelid = target
                      AND i.indisprimary
                      AND i.indkey[g.n - 1] = ANY (fk.conkey)
                ) AS pk_pos
            FROM pg_constraint fk
            WHERE fk.conrelid = target AND fk.contype = 'f'
        ) s;

        kind := 'edge';
        SELECT e.ref_rel, e.cols, e.typs, e.keys
          INTO from_rel, from_cols, from_typs, from_keys
        FROM zega.endpoint(target, cons[1]) e;
        SELECT e.ref_rel, e.cols, e.typs, e.keys
          INTO to_rel, to_cols, to_typs, to_keys
        FROM zega.endpoint(target, cons[2]) e;
        RETURN;
    END IF;

    IF nkeys = 1 AND a0 <> 0 THEN
        kind := 'node';
        SELECT a.attname INTO pk FROM pg_attribute a WHERE a.attrelid = target AND a.attnum = a0;
        RETURN;
    END IF;

    RAISE EXCEPTION 'pg_zega table % needs a one-column primary key', relname;
END;
$$;

CREATE FUNCTION match_side(keys text[], typs text[], cols text[], rec jsonb)
RETURNS text
LANGUAGE plpgsql
IMMUTABLE
AS $$
DECLARE
    i integer;
    part text := '';
    lit text;
BEGIN
    FOR i IN 1 .. array_length(keys, 1) LOOP
        IF rec -> cols[i] IS NULL OR rec -> cols[i] = 'null'::jsonb THEN
            RAISE EXCEPTION 'pg_zega relation endpoint % is null', cols[i];
        END IF;
        lit := zega.literal(zega.zega_type(typs[i]), rec -> cols[i]);
        IF part = '' THEN
            part := keys[i] || ': ' || lit;
        ELSE
            part := part || ' && ' || keys[i] || ': ' || lit;
        END IF;
    END LOOP;
    RETURN part;
END;
$$;

CREATE FUNCTION link_props(target oid, rec jsonb)
RETURNS text
LANGUAGE plpgsql
AS $$
DECLARE
    relname text;
    col record;
    ztype text;
    props text := '';
BEGIN
    SELECT c.relname INTO relname FROM pg_class c WHERE c.oid = target;
    FOR col IN
        SELECT a.attname AS name, y.typname AS typ
        FROM pg_attribute a
        JOIN pg_type y ON y.oid = a.atttypid
        WHERE a.attrelid = target
          AND a.attnum > 0
          AND NOT a.attisdropped
          AND NOT EXISTS (
              SELECT 1 FROM pg_constraint c
              WHERE c.conrelid = target
                AND c.contype = 'f'
                AND a.attnum = ANY (c.conkey)
          )
        ORDER BY a.attnum
    LOOP
        IF col.name !~ '^[A-Za-z_][A-Za-z0-9_]*$' THEN
            RAISE EXCEPTION 'pg_zega cannot use column name %.%', relname, col.name;
        END IF;
        IF col.name = 'id' THEN
            RAISE EXCEPTION 'pg_zega cannot use column %.id; Zega reserves id for the engine id', relname;
        END IF;
        ztype := zega.zega_type(col.typ);
        IF ztype IS NULL THEN
            RAISE EXCEPTION 'pg_zega cannot map %.% (%)', relname, col.name, col.typ;
        END IF;
        IF rec -> col.name IS NULL OR rec -> col.name = 'null'::jsonb THEN
            CONTINUE;
        END IF;
        props := props || ' &' || col.name || ': ' || zega.literal(ztype, rec -> col.name);
    END LOOP;
    RETURN props;
END;
$$;

CREATE FUNCTION render_schema()
RETURNS text
LANGUAGE plpgsql
AS $$
DECLARE
    rel record;
    col record;
    shp record;
    ztype text;
    body text := '';
    uniques text := '';
    fields text;
    props text;
    optional text;
    n_names text[] := ARRAY[]::text[];
    n_pks text[] := ARRAY[]::text[];
    n_fields text[] := ARRAY[]::text[];
    e_from text[] := ARRAY[]::text[];
    e_field text[] := ARRAY[]::text[];
    i integer;
    j integer;
BEGIN
    DELETE FROM zega.tracked t
    WHERE NOT EXISTS (SELECT 1 FROM pg_class c WHERE c.oid = t.relid);

    FOR rel IN
        SELECT t.relid, c.relname
        FROM zega.tracked t
        JOIN pg_class c ON c.oid = t.relid
        ORDER BY c.relname
    LOOP
        SELECT * INTO shp FROM zega.shape(rel.relid);
        IF shp.kind = 'edge' THEN
            IF EXISTS (
                SELECT 1 FROM pg_attribute a
                WHERE a.attrelid = (SELECT c.oid FROM pg_class c WHERE c.relname = shp.from_rel AND c.oid IN (SELECT relid FROM zega.tracked))
                  AND a.attnum > 0 AND NOT a.attisdropped AND a.attname = shp.field
            ) THEN
                RAISE EXCEPTION 'pg_zega edge % uses the same name as a column on %', shp.field, shp.from_rel;
            END IF;
            props := '';
            FOR col IN
                SELECT a.attname AS name, a.attnotnull AS notnull, y.typname AS typ
                FROM pg_attribute a
                JOIN pg_type y ON y.oid = a.atttypid
                WHERE a.attrelid = rel.relid
                  AND a.attnum > 0
                  AND NOT a.attisdropped
                  AND NOT EXISTS (
                      SELECT 1 FROM pg_constraint c
                      WHERE c.conrelid = rel.relid
                        AND c.contype = 'f'
                        AND a.attnum = ANY (c.conkey)
                  )
                ORDER BY a.attnum
            LOOP
                IF col.name !~ '^[A-Za-z_][A-Za-z0-9_]*$' THEN
                    RAISE EXCEPTION 'pg_zega cannot use column name %.%', rel.relname, col.name;
                END IF;
                IF col.name = 'id' THEN
                    RAISE EXCEPTION 'pg_zega cannot use column %.id; Zega reserves id for the engine id', rel.relname;
                END IF;
                ztype := zega.zega_type(col.typ);
                IF ztype IS NULL THEN
                    RAISE EXCEPTION 'pg_zega cannot map %.% (%)', rel.relname, col.name, col.typ;
                END IF;
                optional := CASE WHEN col.notnull THEN '' ELSE '?' END;
                props := props || format(E'\n      %s%s: %s', col.name, optional, ztype);
            END LOOP;
            IF props = '' THEN
                e_field := array_append(e_field, format(E'\n    %s -> %s[]', shp.field, shp.to_rel));
            ELSE
                e_field := array_append(e_field, format(E'\n    %s -> %s[] {%s\n    }', shp.field, shp.to_rel, props));
            END IF;
            e_from := array_append(e_from, shp.from_rel);
            CONTINUE;
        END IF;

        fields := '';
        FOR col IN
            SELECT a.attname AS name, a.attnotnull AS notnull, y.typname AS typ
            FROM pg_attribute a
            JOIN pg_type y ON y.oid = a.atttypid
            WHERE a.attrelid = rel.relid
              AND a.attnum > 0
              AND NOT a.attisdropped
            ORDER BY a.attnum
        LOOP
            IF col.name !~ '^[A-Za-z_][A-Za-z0-9_]*$' THEN
                RAISE EXCEPTION 'pg_zega cannot use column name %.%', rel.relname, col.name;
            END IF;
            IF col.name = 'id' THEN
                RAISE EXCEPTION 'pg_zega cannot use column %.id; Zega reserves id for the engine id', rel.relname;
            END IF;
            ztype := zega.zega_type(col.typ);
            IF ztype IS NULL THEN
                RAISE EXCEPTION 'pg_zega cannot map %.% (%)', rel.relname, col.name, col.typ;
            END IF;
            optional := CASE WHEN col.notnull THEN '' ELSE '?' END;
            fields := fields || format(E'\n    %s%s: %s', col.name, optional, ztype);
        END LOOP;
        n_names := array_append(n_names, rel.relname);
        n_pks := array_append(n_pks, shp.pk);
        n_fields := array_append(n_fields, fields);
    END LOOP;

    FOR i IN 1 .. coalesce(array_length(n_names, 1), 0) LOOP
        fields := n_fields[i];
        FOR j IN 1 .. coalesce(array_length(e_from, 1), 0) LOOP
            IF e_from[j] = n_names[i] THEN
                fields := fields || e_field[j];
            END IF;
        END LOOP;
        body := body || format(E'\n  type %s {%s\n  }', n_names[i], fields);
        IF uniques = '' THEN
            uniques := format('%s { %s }', n_names[i], n_pks[i]);
        ELSE
            uniques := uniques || format(' %s { %s }', n_names[i], n_pks[i]);
        END IF;
    END LOOP;

    IF body = '' THEN
        RETURN NULL;
    END IF;
    RETURN 'schema {' || body || E'\n}\nunique { ' || uniques || ' }';
END;
$$;

CREATE FUNCTION enqueue_schema()
RETURNS void
LANGUAGE plpgsql
AS $$
DECLARE
    schema_txt text;
BEGIN
    schema_txt := zega.render_schema();
    IF schema_txt IS NULL THEN
        PERFORM zega.enqueue('R');
    ELSE
        PERFORM zega.enqueue(schema_txt);
    END IF;
END;
$$;

CREATE FUNCTION sync_row()
RETURNS trigger
LANGUAGE plpgsql
AS $$
DECLARE
    pk text;
    pktype text;
    schema_txt text;
    rec jsonb;
    shp record;
    side record;
    props text;
    edge_mut text;
    col record;
    ztype text;
    lit text;
    cond text := '';
    extras text := '';
    sets text := '';
    mutation text;
BEGIN
    SELECT a.attname, y.typname INTO pk, pktype
    FROM pg_index i
    JOIN pg_attribute a
      ON a.attrelid = i.indrelid
     AND a.attnum = i.indkey[0]
    JOIN pg_type y ON y.oid = a.atttypid
    WHERE i.indrelid = TG_RELID
      AND i.indisprimary
      AND i.indnkeyatts = 1
      AND i.indkey[0] <> 0;

    SELECT * INTO shp FROM zega.shape(TG_RELID);
    schema_txt := zega.render_schema();
    IF schema_txt IS NULL THEN
        RAISE EXCEPTION 'pg_zega has no schema for %', TG_TABLE_NAME;
    END IF;

    IF shp.kind = 'edge' THEN
        PERFORM zega.enqueue(schema_txt);
        IF TG_OP = 'DELETE' OR TG_OP = 'UPDATE' THEN
            side := OLD;
            edge_mut := 'mutation { ' || shp.from_rel || '('
                || zega.match_side(shp.from_keys, shp.from_typs, shp.from_cols, to_jsonb(side))
                || ') { ' || shp.field || ' -> unlink ' || shp.to_rel || '('
                || zega.match_side(shp.to_keys, shp.to_typs, shp.to_cols, to_jsonb(side))
                || ') } }';
            PERFORM zega.enqueue(edge_mut);
        END IF;
        IF TG_OP = 'INSERT' OR TG_OP = 'UPDATE' THEN
            side := NEW;
            props := zega.link_props(TG_RELID, to_jsonb(side));
            edge_mut := 'mutation { ' || shp.from_rel || '('
                || zega.match_side(shp.from_keys, shp.from_typs, shp.from_cols, to_jsonb(side))
                || ') { ' || shp.field || ' -> link ' || shp.to_rel || '('
                || zega.match_side(shp.to_keys, shp.to_typs, shp.to_cols, to_jsonb(side))
                || ')';
            IF props <> '' THEN
                edge_mut := edge_mut || ' {' || props || ' }';
            END IF;
            edge_mut := edge_mut || ' } }';
            PERFORM zega.enqueue(edge_mut);
        END IF;
        IF TG_OP = 'DELETE' THEN
            RETURN OLD;
        END IF;
        RETURN NEW;
    END IF;

    IF pk IS NULL THEN
        RAISE EXCEPTION 'pg_zega requires a single-column primary key on %', TG_TABLE_NAME;
    END IF;

    IF TG_OP = 'DELETE' THEN
        lit := zega.literal(zega.zega_type(pktype), to_jsonb(OLD) -> pk);
        mutation := 'mutation { delete ' || TG_TABLE_NAME || '(' || pk || ': ' || lit || ') { @detach @id } }';
        PERFORM zega.enqueue(schema_txt);
        PERFORM zega.enqueue(mutation);
        RETURN OLD;
    END IF;

    rec := to_jsonb(NEW);
    FOR col IN
        SELECT a.attname AS name, y.typname AS typ
        FROM pg_attribute a
        JOIN pg_type y ON y.oid = a.atttypid
        WHERE a.attrelid = TG_RELID
          AND a.attnum > 0
          AND NOT a.attisdropped
        ORDER BY a.attnum
    LOOP
        ztype := zega.zega_type(col.typ);
        IF ztype IS NULL THEN
            RAISE EXCEPTION 'pg_zega cannot map %.% (%)', TG_TABLE_NAME, col.name, col.typ;
        END IF;
        IF col.name = 'id' THEN
            RAISE EXCEPTION 'pg_zega cannot use column %.id; Zega reserves id for the engine id', TG_TABLE_NAME;
        END IF;
        IF rec -> col.name IS NULL OR rec -> col.name = 'null'::jsonb THEN
            -- An UPDATE that empties a column clears the property in Zega too.
            IF TG_OP = 'UPDATE' AND col.name <> pk
               AND COALESCE(to_jsonb(OLD) -> col.name, 'null'::jsonb) <> 'null'::jsonb THEN
                IF sets = '' THEN
                    sets := col.name || ': null';
                ELSE
                    sets := sets || ', ' || col.name || ': null';
                END IF;
            END IF;
            CONTINUE;
        END IF;
        lit := zega.literal(ztype, rec -> col.name);
        IF col.name = pk THEN
            cond := col.name || ': ' || lit;
        ELSIF TG_OP = 'INSERT' THEN
            IF extras = '' THEN
                extras := col.name || ': ' || lit;
            ELSE
                extras := extras || ' && ' || col.name || ': ' || lit;
            END IF;
        ELSE
            IF sets = '' THEN
                sets := col.name || ': ' || lit;
            ELSE
                sets := sets || ', ' || col.name || ': ' || lit;
            END IF;
        END IF;
    END LOOP;

    IF extras <> '' THEN
        cond := cond || ' && ' || extras;
    END IF;

    IF TG_OP = 'INSERT' THEN
        mutation := 'mutation { ' || TG_TABLE_NAME || '(' || cond || ') { ' || pk || ' } }';
    ELSE
        IF sets = '' THEN
            RETURN NEW;
        END IF;
        mutation := 'mutation { ' || TG_TABLE_NAME || '(' || cond || ') set ' || sets || ' }';
    END IF;

    PERFORM zega.enqueue(schema_txt);
    PERFORM zega.enqueue(mutation);
    RETURN NEW;
END;
$$;

CREATE FUNCTION graph()
RETURNS text
AS 'MODULE_PATHNAME', 'zega_graph'
LANGUAGE C VOLATILE;

CREATE FUNCTION prepare_reload()
RETURNS void
AS 'MODULE_PATHNAME', 'zega_prepare_reload'
LANGUAGE C VOLATILE;

CREATE FUNCTION read_graph()
RETURNS jsonb
LANGUAGE plpgsql
AS $$
DECLARE
    body jsonb;
BEGIN
    body := zega.graph()::jsonb;
    IF COALESCE(body->>'ok', '') <> 'true' THEN
        RAISE EXCEPTION 'pg_zega could not read the graph';
    END IF;
    RETURN COALESCE(body->'result', '{}'::jsonb);
END;
$$;

CREATE FUNCTION node_insert(target oid, rec jsonb)
RETURNS text
LANGUAGE plpgsql
AS $$
DECLARE
    relname text;
    pk text;
    col record;
    ztype text;
    lit text;
    cond text := '';
    extras text := '';
BEGIN
    SELECT c.relname INTO relname FROM pg_class c WHERE c.oid = target;
    SELECT s.pk INTO pk FROM zega.shape(target) AS s;
    IF pk IS NULL THEN
        RAISE EXCEPTION 'pg_zega requires a single-column primary key on %', relname;
    END IF;
    FOR col IN
        SELECT a.attname AS name, y.typname AS typ
        FROM pg_attribute a
        JOIN pg_type y ON y.oid = a.atttypid
        WHERE a.attrelid = target
          AND a.attnum > 0
          AND NOT a.attisdropped
        ORDER BY a.attnum
    LOOP
        ztype := zega.zega_type(col.typ);
        IF ztype IS NULL THEN
            RAISE EXCEPTION 'pg_zega cannot map %.% (%)', relname, col.name, col.typ;
        END IF;
        IF col.name = 'id' THEN
            RAISE EXCEPTION 'pg_zega cannot use column %.id; Zega reserves id for the engine id', relname;
        END IF;
        IF rec -> col.name IS NULL OR rec -> col.name = 'null'::jsonb THEN
            CONTINUE;
        END IF;
        lit := zega.literal(ztype, rec -> col.name);
        IF col.name = pk THEN
            cond := col.name || ': ' || lit;
        ELSIF extras = '' THEN
            extras := col.name || ': ' || lit;
        ELSE
            extras := extras || ' && ' || col.name || ': ' || lit;
        END IF;
    END LOOP;
    IF cond = '' THEN
        RAISE EXCEPTION 'pg_zega import is missing the primary key of %', relname;
    END IF;
    IF extras <> '' THEN
        cond := cond || ' && ' || extras;
    END IF;
    RETURN 'mutation { ' || relname || '(' || cond || ') { ' || pk || ' } }';
END;
$$;

CREATE FUNCTION edge_link(target oid, rec jsonb)
RETURNS text
LANGUAGE plpgsql
AS $$
DECLARE
    shp record;
    props text;
    edge_mut text;
BEGIN
    SELECT * INTO shp FROM zega.shape(target);
    IF shp.kind IS DISTINCT FROM 'edge' THEN
        RAISE EXCEPTION 'pg_zega import expected an edge table';
    END IF;
    props := zega.link_props(target, rec);
    edge_mut := 'mutation { ' || shp.from_rel || '('
        || zega.match_side(shp.from_keys, shp.from_typs, shp.from_cols, rec)
        || ') { ' || shp.field || ' -> link ' || shp.to_rel || '('
        || zega.match_side(shp.to_keys, shp.to_typs, shp.to_cols, rec)
        || ')';
    IF props <> '' THEN
        edge_mut := edge_mut || ' {' || props || ' }';
    END IF;
    RETURN edge_mut || ' } }';
END;
$$;

CREATE FUNCTION import_rows(target oid)
RETURNS void
LANGUAGE plpgsql
AS $$
DECLARE
    shp record;
    rec record;
BEGIN
    SELECT * INTO shp FROM zega.shape(target);
    FOR rec IN EXECUTE format('SELECT to_jsonb(t) AS row FROM %s AS t', target::regclass)
    LOOP
        IF shp.kind = 'edge' THEN
            PERFORM zega.enqueue(zega.edge_link(target, rec.row));
        ELSE
            PERFORM zega.enqueue(zega.node_insert(target, rec.row));
        END IF;
    END LOOP;
END;
$$;

CREATE FUNCTION flush_node(label text)
RETURNS void
LANGUAGE plpgsql
AS $$
DECLARE
    graph jsonb;
    node record;
BEGIN
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

CREATE FUNCTION flush_edge(field text)
RETURNS void
LANGUAGE plpgsql
AS $$
DECLARE
    graph jsonb;
    rel record;
BEGIN
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
CREATE FUNCTION import(target regclass DEFAULT NULL)
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

CREATE FUNCTION track(target regclass)
RETURNS void
LANGUAGE plpgsql
AS $$
DECLARE
    nsp text;
    rel text;
BEGIN
    SELECT n.nspname, c.relname
      INTO nsp, rel
    FROM pg_class c
    JOIN pg_namespace n ON n.oid = c.relnamespace
    WHERE c.oid = target;

    INSERT INTO zega.tracked (relid) VALUES (target)
    ON CONFLICT DO NOTHING;

    IF NOT EXISTS (
        SELECT 1 FROM pg_trigger
        WHERE tgrelid = target AND tgname = 'zega_sync' AND NOT tgisinternal
    ) THEN
        EXECUTE format(
            'CREATE TRIGGER zega_sync AFTER INSERT OR UPDATE OR DELETE ON %I.%I FOR EACH ROW EXECUTE FUNCTION zega.sync_row()',
            nsp, rel
        );
    END IF;

    PERFORM zega.enqueue_schema();
END;
$$;

CREATE FUNCTION on_ddl()
RETURNS event_trigger
LANGUAGE plpgsql
AS $$
BEGIN
    IF NOT EXISTS (SELECT 1 FROM zega.tracked) THEN
        RETURN;
    END IF;
    PERFORM zega.enqueue_schema();
END;
$$;

CREATE EVENT TRIGGER zega_ddl
ON ddl_command_end
EXECUTE FUNCTION zega.on_ddl();
