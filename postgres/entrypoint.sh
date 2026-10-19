#!/bin/bash
# Require both passwords, then hand off to the Postgres image entrypoint.
set -euo pipefail

if [ "$#" -eq 0 ] || [ "$1" = "postgres" ]; then
    if [ -z "${POSTGRES_PASSWORD:-}" ]; then
        echo "pg_zega: POSTGRES_PASSWORD is required" >&2
        exit 1
    fi
    if [ "${POSTGRES_HOST_AUTH_METHOD:-}" = "trust" ]; then
        echo "pg_zega: POSTGRES_HOST_AUTH_METHOD=trust is refused" >&2
        exit 1
    fi
    if [ -z "${ZEGA_PASSWORD:-}" ]; then
        echo "pg_zega: ZEGA_PASSWORD is required" >&2
        exit 1
    fi
    case "$ZEGA_PASSWORD" in
        *[[:space:]]*|*[$'\177']*)
            echo "pg_zega: ZEGA_PASSWORD must be a single token" >&2
            exit 1
            ;;
    esac

    url="${ZEGA_URL:-http://zega:9342}"
    case "$url" in
        *[[:space:]]*)
            echo "pg_zega: ZEGA_URL must not contain whitespace" >&2
            exit 1
            ;;
    esac

    install -d -m 0755 /etc/pg-zega
    umask 077
    pass="${ZEGA_PASSWORD//\'/\'\'}"
    quoted_url="${url//\'/\'\'}"
    cat > /etc/pg-zega/zega.conf <<EOF
shared_preload_libraries = 'pg_zega'
zega.password = '${pass}'
zega.url = '${quoted_url}'
EOF
    chmod 0644 /etc/pg-zega/zega.conf

    sample=/usr/share/postgresql/postgresql.conf.sample
    if ! grep -q "/etc/pg-zega/zega.conf" "$sample"; then
        printf "\ninclude = '/etc/pg-zega/zega.conf'\n" >> "$sample"
    fi
fi

exec docker-entrypoint.sh "$@"
