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
