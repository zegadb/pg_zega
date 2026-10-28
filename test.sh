#!/bin/sh
# Rebuilds this compose project and deletes the graph at http://127.0.0.1:9342.
# `docker compose down -v` drops the database volume. Start Zega on port 9342 first, with
# ZEGA_TOKEN=pg_zega-test-token (the token test.sql uses).
set -eu
cd "$(dirname "$0")"

ZEGA_PASSWORD=pg_zega-test-token
export ZEGA_PASSWORD

if [ -z "${POSTGRES_PASSWORD:-}" ]; then
    POSTGRES_PASSWORD=$(openssl rand -base64 24)
    export POSTGRES_PASSWORD
fi

echo "building postgres"
docker compose down -v >/dev/null 2>&1 || true
docker compose up --build -d

echo "waiting for postgres and the host zega"
ready=0
i=0
while [ "$i" -lt 60 ]; do
    if curl -sf -H "Authorization: Bearer $ZEGA_PASSWORD" http://127.0.0.1:9342/health >/dev/null && \
       docker compose exec -T postgres pg_isready -U postgres >/dev/null; then
        ready=1
        break
    fi
    i=$((i + 1))
    sleep 2
done
if [ "$ready" -ne 1 ]; then
    echo "postgres or zega did not become ready. Start Zega on 127.0.0.1:9342" >&2
    docker compose logs --tail 80
    exit 1
fi

curl -sf -H "Authorization: Bearer $ZEGA_PASSWORD" -X DELETE http://127.0.0.1:9342/graph >/dev/null

echo "running sync test"
docker compose exec -T postgres psql -U postgres -v ON_ERROR_STOP=1 < test.sql

python3 check-graph.py

echo "zega.import replaces the graph with the same rows"
docker compose exec -T postgres psql -U postgres -v ON_ERROR_STOP=1 -c "SELECT zega.import()" >/dev/null
docker compose exec -T postgres psql -U postgres -v ON_ERROR_STOP=1 -c "SELECT zega.import('users')" >/dev/null
python3 check-graph.py >/dev/null
echo "pg_zega import test passed"

echo "upgrading from 0.1 or 0.2 gives what a fresh install has"
pg() { docker compose exec -T postgres psql -U postgres -v ON_ERROR_STOP=1 -tA "$@"; }
# Every object in schema zega: functions with their bodies, tables and views
# with their columns, and each view's query.
signature='SELECT p.proname::text || pg_get_function_identity_arguments(p.oid) || pg_get_functiondef(p.oid)
             FROM pg_proc p JOIN pg_namespace n ON n.oid = p.pronamespace WHERE n.nspname = '"'zega'"'
           UNION ALL
           SELECT c.relname::text || c.relkind::text || coalesce(pg_get_viewdef(c.oid), '"''"') || string_agg(a.attname::text || format_type(a.atttypid, a.atttypmod) || a.attnotnull::text, '"','"' ORDER BY a.attnum)
             FROM pg_class c JOIN pg_namespace n ON n.oid = c.relnamespace
             JOIN pg_attribute a ON a.attrelid = c.oid AND a.attnum > 0 AND NOT a.attisdropped
            WHERE n.nspname = '"'zega'"' GROUP BY c.oid, c.relname, c.relkind
           ORDER BY 1'
mkdir -p .tmp
for from in fresh 0.1 0.2; do
    db=upgrade_$(echo "$from" | tr . _)
    pg -c "DROP DATABASE IF EXISTS $db" >/dev/null 2>&1
    pg -c "CREATE DATABASE $db" >/dev/null
    if [ "$from" = fresh ]; then
        pg -d "$db" -c "CREATE EXTENSION pg_zega" >/dev/null
    else
        pg -d "$db" -c "CREATE EXTENSION pg_zega VERSION '$from'" -c "ALTER EXTENSION pg_zega UPDATE" >/dev/null
    fi
    [ "$(pg -d "$db" -c "SELECT extversion FROM pg_extension WHERE extname = 'pg_zega'")" = 0.3 ] ||
        { echo "$db is not at 0.3" >&2; exit 1; }
    pg -d "$db" -c "$signature" > ".tmp/upgrade-$from.txt"
done
[ -s .tmp/upgrade-fresh.txt ] || { echo "the fresh install has no objects" >&2; exit 1; }
for from in 0.1 0.2; do
    if ! cmp -s .tmp/upgrade-fresh.txt ".tmp/upgrade-$from.txt"; then
        echo "upgrading from $from differs from a fresh install:" >&2
        diff .tmp/upgrade-fresh.txt ".tmp/upgrade-$from.txt" | head -20 >&2
        exit 1
    fi
done
echo "pg_zega upgrade test passed"
