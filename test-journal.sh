#!/bin/sh
# Journal mode end to end: test.sql in zega.mode = journal, delivered by
# pg_zega's delivery worker, must leave the same graph as precommit mode
# (check-graph.py).
#   ./test-journal.sh <zega binary>
# Starts its own Zega on port 9342 and rebuilds this compose project;
# `docker compose down -v` drops the database volume.
set -eu
cd "$(dirname "$0")"
zega_bin=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")

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
compose="docker compose"
zega_pid=

start_zega() {
    # The container reaches the host through host-gateway, not loopback.
    "$zega_bin" start --data "$work/zega" --host 0.0.0.0 --port 9342 --token-file "$work/token" >> "$work/zega.log" 2>&1 &
    zega_pid=$!
    i=0
    until curl -sf -H "Authorization: Bearer $ZEGA_PASSWORD" http://127.0.0.1:9342/health >/dev/null; do
        i=$((i + 1))
        if [ "$i" -gt 60 ]; then echo "zega did not start" >&2; cat "$work/zega.log" >&2; exit 1; fi
        sleep 1
    done
}
cleanup() {
    if [ -n "$zega_pid" ]; then kill "$zega_pid"; wait "$zega_pid" 2>/dev/null || true; fi
}
trap cleanup EXIT

psql() {
    $compose exec -T postgres psql -U postgres -v ON_ERROR_STOP=1 -tA "$@"
}
waiting() {
    psql -c "SELECT count(*) FROM zega.journal"
}
fail() { echo "FAIL: $*" >&2; exit 1; }
# The worker empties zega.journal: wait for it.
delivered() {
    i=0
    until [ "$(waiting)" = 0 ]; do
        i=$((i + 1))
        if [ "$i" -gt 60 ]; then $compose logs --tail 40 postgres >&2; fail "$1"; fi
        sleep 0.5
    done
}
worker_log() {
    $compose logs postgres 2>&1
}
graph() {
    curl -sf -H "Accept: application/json" -H "Authorization: Bearer $ZEGA_PASSWORD" http://127.0.0.1:9342/graph
}
nodes() {
    graph | python3 -c 'import json,sys; print(len(json.load(sys.stdin)["result"]["nodes"]))'
}

echo "starting postgres; Zega stays down for now"
$compose down -v >/dev/null 2>&1 || true
$compose up --build -d
i=0
until $compose exec -T postgres pg_isready -U postgres >/dev/null 2>&1 && psql -c "SELECT 1" >/dev/null 2>&1; do
    i=$((i + 1))
    if [ "$i" -gt 60 ]; then $compose logs --tail 80; fail "postgres did not start"; fi
    sleep 2
done

echo "the delivery worker runs and creates its slot"
i=0
until [ "$(psql -c "SELECT count(*) FROM pg_replication_slots WHERE slot_name = 'zega' AND plugin = 'pg_zega'")" = 1 ]; do
    i=$((i + 1))
    if [ "$i" -gt 60 ]; then $compose logs --tail 80; fail "the worker created no slot"; fi
    sleep 0.5
done
[ "$(psql -c "SELECT count(*) FROM pg_stat_activity WHERE backend_type = 'pg_zega delivery'")" = 1 ] ||
    fail "no pg_zega delivery worker"
psql -c "ALTER DATABASE postgres SET zega.mode = 'journal'" >/dev/null

echo "with Zega down, test.sql commits and is journaled"
psql < test.sql >/dev/null
journaled=$(waiting)
[ "$journaled" -gt 0 ] || fail "nothing was journaled"
echo "journaled $journaled transactions"
status() {
    psql -c "SELECT worker_running, waiting, oldest_waiting IS NOT NULL, lag > interval '0', retained_wal_bytes > 0 FROM zega.delivery_status"
}
[ "$(status)" = "t|$journaled|t|t|t" ] || fail "zega.delivery_status does not show the waiting rows: $(status)"

i=0
until worker_log | grep -q "not delivered yet"; do
    i=$((i + 1))
    if [ "$i" -gt 60 ]; then worker_log | grep -E "pg_zega|ERROR|exited" >&2; fail "the worker never tried Zega"; fi
    sleep 0.5
done

echo "Zega comes up; the worker delivers them in commit order: the same graph as precommit"
start_zega
delivered "the worker did not deliver after Zega came up"
python3 check-graph.py >/dev/null || fail "the delivered graph differs from precommit's"
[ "$(status)" = "t|0|f||t" ] || fail "zega.delivery_status still shows rows waiting: $(status)"

echo "a commit reaches Zega without anyone running anything"
before=$(nodes)
psql -c "INSERT INTO users (user_id, name) VALUES (12, 'Kat')" >/dev/null
delivered "Kat was not delivered"
[ "$(nodes)" = $((before + 1)) ] || fail "Kat was not delivered exactly once"
after=$(nodes)

echo "zega.import flushes by type when it is delivered"
psql -c "SELECT zega.import()" >/dev/null
psql -c "SELECT zega.import('users')" >/dev/null
delivered "an import was not delivered"
python3 check-graph.py >/dev/null || fail "an import changed the graph"
[ "$(nodes)" = "$after" ] || fail "an import lost or duplicated nodes"

echo "a breaking change commits with its import, as one delivery"
psql <<'SQL' >/dev/null
BEGIN;
ALTER TABLE people ADD COLUMN code text NOT NULL DEFAULT 'x';
SELECT zega.import('people');
COMMIT;
SQL
delivered "the breaking change was not delivered"
graph | python3 -c 'import json,sys; people=[n for n in json.load(sys.stdin)["result"]["nodes"] if n.get("labels")==["people"]]; assert [p.get("code") for p in people]==["x"], people' ||
    fail "people were not reloaded with the new column"

echo "a change Zega refuses holds the journal; the rows before it in its batch are delivered"
kill "$zega_pid"; wait "$zega_pid" 2>/dev/null || true; zega_pid=
psql -c "INSERT INTO users (user_id, name) VALUES (19, 'Uma')" >/dev/null
psql -c "ALTER TABLE people ADD COLUMN badge text NOT NULL DEFAULT 'y'" >/dev/null
psql -c "INSERT INTO users (user_id, name) VALUES (13, 'Lin')" >/dev/null
start_zega
i=0
until worker_log | grep -q "Zega refused txid"; do
    i=$((i + 1))
    if [ "$i" -gt 60 ]; then fail "no refusal logged"; fi
    sleep 0.5
done
sleep 3
[ "$(waiting)" = 2 ] || fail "the refused row and the one after it were not held: $(waiting) waiting"
graph | python3 -c 'import json,sys; assert any(n.get("user_id") == 19 for n in json.load(sys.stdin)["result"]["nodes"])' ||
    fail "Uma, before the refused row, was not delivered"

echo "an import waits for open writes"
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

echo "the worker drops the rows before the import and delivers it"
delivered "the worker did not recover with the import"
worker_log | grep -q "dropped txid" || fail "the worker dropped nothing"
worker_log | grep -q "recovered: delivered the import" || fail "the worker did not report the recovery"
graph | python3 -c '
import json, sys
nodes = json.load(sys.stdin)["result"]["nodes"]
people = [n for n in nodes if n.get("labels") == ["people"]]
assert [p.get("badge") for p in people] == ["y"], people
users = sorted(n["user_id"] for n in nodes if "user_id" in n)
assert users == [10, 11, 12, 13, 14, 19], users
' || fail "the recovered graph is not what Postgres holds"
python3 check-graph.py >/dev/null || fail "the recovered graph lost the test.sql rows"
psql -c "INSERT INTO users (user_id, name) VALUES (15, 'Pia')" >/dev/null
delivered "the worker did not carry on after the recovery"

echo "after a crash the slot decodes delivered rows again; the worker skips them"
psql -c "CHECKPOINT" >/dev/null
psql -c "INSERT INTO users (user_id, name) VALUES (16, 'Quin')" >/dev/null
psql -c "UPDATE users SET name = 'Quinn' WHERE user_id = 16" >/dev/null
delivered "Quin was not delivered"
refusals=$(worker_log | grep -c "Zega refused" || true)
# Ray waits in the journal, so the worker decodes again after the crash.
kill "$zega_pid"; wait "$zega_pid" 2>/dev/null || true; zega_pid=
psql -c "INSERT INTO users (user_id, name) VALUES (17, 'Ray')" >/dev/null
$compose kill -s SIGKILL postgres >/dev/null 2>&1
$compose start postgres >/dev/null 2>&1
start_zega
delivered "the worker did not carry on after the crash"
[ "$(worker_log | grep -c "Zega refused" || true)" = "$refusals" ] || fail "Zega refused a row after the crash"
graph | python3 -c '
import json, sys
users = {n["user_id"]: n.get("name") for n in json.load(sys.stdin)["result"]["nodes"] if "user_id" in n}
assert sorted(users) == [10, 11, 12, 13, 14, 15, 16, 17, 19], users
assert users[16] == "Quinn", users[16]
' || fail "a row delivered before the crash was applied again"
worker_log | grep -q "was delivered before; skipped" || fail "the slot did not decode the delivered rows again"

$compose down -v >/dev/null 2>&1
echo "pg_zega journal test passed"
