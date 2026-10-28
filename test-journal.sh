#!/bin/sh
# Journal mode end to end: test.sql in zega.mode = journal, delivered by the
# relay, must leave the same graph as precommit mode (check-graph.py).
#   ./test-journal.sh <zega binary> <pg_zega-relay binary>
# Starts its own Zega on 127.0.0.1:9342 and rebuilds this compose project;
# `docker compose down -v` drops the database volume.
set -eu
cd "$(dirname "$0")"
zega_bin=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
relay_bin=$(cd "$(dirname "$2")" && pwd)/$(basename "$2")

work=.tmp/journal
mkdir -p .tmp
find "$work" -mindepth 1 -delete 2>/dev/null || true
mkdir -p "$work"
printf 'pg_zega-test-token\n' > "$work/token"
ZEGA_PASSWORD=pg_zega-test-token
export ZEGA_PASSWORD
if [ -z "${POSTGRES_PASSWORD:-}" ]; then
    POSTGRES_PASSWORD=$(openssl rand -hex 24)
    export POSTGRES_PASSWORD
fi
printf '%s\n' "$POSTGRES_PASSWORD" > "$work/pg-password"
compose="docker compose -f docker-compose.yml -f docker-compose.journal.yml"
zega_pid=

start_zega() {
    "$zega_bin" start --data "$work/zega" --port 9342 --token-file "$work/token" >> "$work/zega.log" 2>&1 &
    zega_pid=$!
    i=0
    until curl -sf -H "Authorization: Bearer $ZEGA_PASSWORD" http://127.0.0.1:9342/health >/dev/null; do
        i=$((i + 1))
        if [ "$i" -gt 60 ]; then echo "zega did not start" >&2; cat "$work/zega.log" >&2; exit 1; fi
        sleep 1
    done
}
stop_zega() {
    kill "$zega_pid"
    wait "$zega_pid" 2>/dev/null || true
    zega_pid=
}
cleanup() {
    if [ -n "$zega_pid" ]; then stop_zega; fi
}
trap cleanup EXIT

psql() {
    $compose exec -T postgres psql -U postgres -v ON_ERROR_STOP=1 -tA "$@"
}
relay() {
    "$relay_bin" --postgres "host=127.0.0.1 port=${PG_ZEGA_TEST_PORT:-5433} user=postgres dbname=postgres" \
        --postgres-password-file "$work/pg-password" \
        --zega http://127.0.0.1:9342 --zega-token-file "$work/token" --once "$@"
}
nodes() {
    curl -sf -H "Accept: application/json" -H "Authorization: Bearer $ZEGA_PASSWORD" http://127.0.0.1:9342/graph |
        python3 -c 'import json,sys; print(len(json.load(sys.stdin)["result"]["nodes"]))'
}
fail() { echo "FAIL: $*" >&2; exit 1; }

echo "starting zega and postgres (wal_level=logical)"
start_zega
$compose down -v >/dev/null 2>&1 || true
$compose up --build -d
i=0
until $compose exec -T postgres pg_isready -U postgres >/dev/null 2>&1 && psql -c "SELECT 1" >/dev/null 2>&1; do
    i=$((i + 1))
    if [ "$i" -gt 60 ]; then $compose logs --tail 80; fail "postgres did not start"; fi
    sleep 2
done

echo "the relay creates its slot; a second slot will replay everything"
relay
psql -c "SELECT pg_create_logical_replication_slot('zega_replay', 'pg_zega')" >/dev/null
psql -c "ALTER DATABASE postgres SET zega.mode = 'journal'" >/dev/null

echo "test.sql commits without Zega seeing anything"
psql < test.sql >/dev/null
[ "$(nodes)" = 0 ] || fail "Zega changed before the relay ran"
waiting=$(psql -c "SELECT count(*) FROM zega.journal")
[ "$waiting" -gt 0 ] || fail "nothing was journaled"
echo "journaled $waiting transactions"

echo "the relay delivers them in commit order: the same graph as precommit"
relay
python3 check-graph.py >/dev/null || fail "the delivered graph differs from precommit's"
[ "$(psql -c "SELECT count(*) FROM zega.journal")" = 0 ] || fail "delivered rows are still in zega.journal"

echo "with Zega down, Postgres still commits; the relay waits and delivers"
before=$(nodes)
stop_zega
psql -c "INSERT INTO users (user_id, name) VALUES (12, 'Kat')" >/dev/null
relay > "$work/relay-wait.log" 2>&1 &
relay_pid=$!
sleep 3
start_zega
wait "$relay_pid" || { cat "$work/relay-wait.log" >&2; fail "the relay did not deliver after Zega came back"; }
grep -q "retrying" "$work/relay-wait.log" || fail "the relay never had to wait"
[ "$(nodes)" = $((before + 1)) ] || fail "Kat was not delivered exactly once"

echo "a second slot redelivers every transaction: nothing is applied twice"
after=$(nodes)
relay --slot zega_replay
[ "$(nodes)" = "$after" ] || fail "a redelivery wrote again"
python3 check-graph.py >/dev/null || fail "a redelivery changed the graph"

echo "zega.import journals what to flush by type; Zega flushes it on delivery"
psql -c "SELECT zega.import()" >/dev/null
psql -c "SELECT zega.import('users')" >/dev/null
[ "$(psql -c "SELECT count(*) FROM zega.journal WHERE flush IS NOT NULL")" = 2 ] || fail "the imports journaled no flush"
relay
python3 check-graph.py >/dev/null || fail "an import changed the graph"
[ "$(nodes)" = "$after" ] || fail "an import lost or duplicated nodes"

echo "a breaking change commits with its import, as one delivery"
psql <<'SQL' >/dev/null
BEGIN;
ALTER TABLE people ADD COLUMN code text NOT NULL DEFAULT 'x';
SELECT zega.import('people');
COMMIT;
SQL
relay
curl -sf -H "Accept: application/json" -H "Authorization: Bearer $ZEGA_PASSWORD" http://127.0.0.1:9342/graph |
    python3 -c 'import json,sys; people=[n for n in json.load(sys.stdin)["result"]["nodes"] if n.get("labels")==["people"]]; assert [p.get("code") for p in people]==["x"], people' ||
    fail "people were not reloaded with the new column"

echo "a change Zega refuses stops the relay and stays journaled"
psql -c "ALTER TABLE people ADD COLUMN badge text NOT NULL DEFAULT 'y'" >/dev/null
if relay > "$work/relay-refused.log" 2>&1; then fail "the relay accepted a refused delivery"; fi
grep -q "refused txid" "$work/relay-refused.log" || { cat "$work/relay-refused.log" >&2; fail "no refusal reported"; }
[ "$(psql -c "SELECT count(*) FROM zega.journal")" = 1 ] || fail "the refused row left zega.journal"

echo "--recover without a full import drops nothing"
if relay --recover > "$work/recover-none.log" 2>&1; then fail "--recover ran without an import"; fi
grep -q "run SELECT zega.import()" "$work/recover-none.log" || { cat "$work/recover-none.log" >&2; fail "wrong --recover error"; }
[ "$(psql -c "SELECT count(*) FROM zega.journal")" = 1 ] || fail "--recover dropped rows without an import"

echo "rows after the refused one wait; an import waits for open writes"
psql -c "INSERT INTO users (user_id, name) VALUES (13, 'Lin')" >/dev/null
psql -c "BEGIN; INSERT INTO users (user_id, name) VALUES (14, 'Mo'); SELECT pg_sleep(4); COMMIT;" >/dev/null &
writer=$!
sleep 1
psql -c "SELECT zega.import()" >/dev/null &
importer=$!
sleep 1
[ "$(psql -c "SELECT count(*) FROM pg_stat_activity WHERE wait_event_type = 'Lock' AND query LIKE '%zega.import()%'")" = 1 ] ||
    fail "the import did not wait for the open write"
wait "$writer" || fail "the open write failed"
wait "$importer" || fail "the import failed"

echo "--recover drops the rows before the import and delivers it"
relay --recover > "$work/recover.log" 2>&1 || { cat "$work/recover.log" >&2; fail "--recover failed"; }
grep -q "dropped txid" "$work/recover.log" || fail "--recover dropped nothing"
[ "$(psql -c "SELECT count(*) FROM zega.journal")" = 0 ] || fail "rows are left in zega.journal"
curl -sf -H "Accept: application/json" -H "Authorization: Bearer $ZEGA_PASSWORD" http://127.0.0.1:9342/graph |
    python3 -c '
import json, sys
nodes = json.load(sys.stdin)["result"]["nodes"]
people = [n for n in nodes if n.get("labels") == ["people"]]
assert [p.get("badge") for p in people] == ["y"], people
users = sorted(n["user_id"] for n in nodes if "user_id" in n)
assert users == [10, 11, 12, 13, 14], users
' || fail "the recovered graph is not what Postgres holds"
python3 check-graph.py >/dev/null || fail "the recovered graph lost the test.sql rows"
psql -c "INSERT INTO users (user_id, name) VALUES (15, 'Pia')" >/dev/null
relay
[ "$(psql -c "SELECT count(*) FROM zega.journal")" = 0 ] || fail "the relay did not carry on after --recover"

$compose down -v >/dev/null 2>&1
echo "pg_zega journal test passed"
