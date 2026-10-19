# pg_zega

Postgres extension that keeps a [Zega](https://github.com/zegadb/zega) graph in the same transaction as the tables it tracks. Apps write through Postgres. On commit, pg_zega sends the schema and the row changes to Zega. If Zega is down or refuses the change, Postgres rolls the transaction back.

The schema is rendered from the catalog. An `ALTER TABLE` on a tracked table updates Zega. Rows already stored in Zega are left in place when the change fits them. When it does not, flush that scope and load it again from Postgres.

Postgres 17. The extension is superuser-only and links libcurl.

## Install

Build it on the Postgres that will load it. The shared library has to match that server's major version and that machine's libcurl.

```sh
sudo apt-get install build-essential libcurl4-openssl-dev postgresql-server-dev-17
cd postgres
make
sudo make install
```

```sql
CREATE EXTENSION pg_zega;
```

On a host install, point the extension at a Zega server on this machine:

```sql
ALTER SYSTEM SET zega.url = 'http://127.0.0.1:9342';
SELECT pg_reload_conf();
```

`postgres/Dockerfile` is that same build on `postgres:17-bookworm`. From this repository:

```sh
docker compose up --build -d
```

Postgres listens on `127.0.0.1:5433` as `postgres`, with trust authentication, for a local trial. Inside that container the default `zega.url` is `http://host.docker.internal:9342`, which is a Zega server on the host.

## Track a table

A table with one primary-key column is a node. The table name is the Zega type. The other columns are fields. A column named `id` is reserved for the engine id. A nullable column is optional (`email?: String`). A `NOT NULL` column is required.

Mapped types are `smallint`, `integer`, and `bigint` to Int; `real` and `double precision` to Float; `boolean` to Bool; `text`, `varchar`, and `character` to String. Any other type is rejected and the statement rolls back. That includes `bytea`, `numeric`, timestamps, `json`, `uuid`, and arrays.

```sql
CREATE TABLE users (
    user_id int PRIMARY KEY,
    name text NOT NULL
);

SELECT zega.track('users');

INSERT INTO users (user_id, name) VALUES (10, 'Nia');
```

`zega.track` is a function, so Postgres runs it with `SELECT`. It enrolls the table and creates an `AFTER ROW` trigger named `zega_sync`. It does not copy rows that are already in the table. Track those tables, then import.

Each commit renders the schema and posts it to Zega before the row changes. A following insert looks like this:

```
schema {
  type users {
    user_id: Int
    name: String
  }
}
unique { users { user_id } }
```

## Edges

A table whose name starts with `ze_` is a relationship. The Zega field name drops that prefix, so `ze_liked` is `liked`. The table needs exactly two foreign-key constraints. Count constraints, so a composite foreign key is still one endpoint. Zero, one, or three or more is an error. The two foreign keys must not share a column. The primary key is exactly the columns of those two constraints, and primary-key order is the direction. Every other column is a property of the edge. The two ends may be different types. The field is declared on the source type. There is no inverse field. Both referenced tables are tracked first.

```sql
CREATE TABLE movies (
    movie_id int PRIMARY KEY,
    title text NOT NULL
);

CREATE TABLE ze_liked (
    user_id int NOT NULL REFERENCES users (user_id),
    movie_id int NOT NULL REFERENCES movies (movie_id),
    rating int,
    PRIMARY KEY (user_id, movie_id)
);

SELECT zega.track('movies');
SELECT zega.track('ze_liked');

INSERT INTO movies (movie_id, title) VALUES (1, 'Arrival');
INSERT INTO ze_liked (user_id, movie_id, rating) VALUES (10, 1, 5);
```

That stores one `liked` edge from user 10 to movie 1, with `rating` 5. The rendered field sits on `users`:

```
liked -> movies[] {
  rating?: Int
}
```

## Changing the schema

`CREATE EXTENSION` installs an event trigger on `ddl_command_end`. Altering a tracked table puts the newly rendered schema in that same transaction. You alter the table. Zega diffs the schema it already has.

An optional column applies immediately. Existing nodes keep the properties they already had. With a tracked `teams(team_id, city, name)` that already has rows in Zega:

```sql
ALTER TABLE teams ADD COLUMN abbr text;
-- the stored schema gains abbr?: String
-- a team node still has city, name, and team_id

ALTER TABLE teams DROP COLUMN abbr;
```

A change that does not fit the nodes or edges already in Zega is refused. Postgres rolls the `ALTER` back, so the column is not added. A required column on existing nodes is the usual case. A rename or a type change that the current values cannot satisfy is refused the same way.

```sql
ALTER TABLE teams ADD COLUMN code text NOT NULL DEFAULT 'x';
-- pg_zega sync was rejected (400):
-- schema change refused: incompatible with existing data
```

Flush the scope that the new schema would reject, then load it from Postgres, in the same transaction as the `ALTER`. The flush is applied before the new schema is stored.

```sql
BEGIN;
ALTER TABLE teams RENAME COLUMN city TO town;
SELECT zega.import('teams');
COMMIT;
```

`import('teams')` removes the `teams` nodes and every tracked `ze_` edge that touches them, stores the schema Postgres just rendered (with `town`), then inserts the rows and links those edges again. A type change uses that same transaction: alter the column, call `zega.import` for that table, then commit.

`zega.import` must be the only data work in that transaction. An `INSERT`, `UPDATE`, or `DELETE` beside it raises `pg_zega import must not share a transaction with row changes`, and nothing is sent to Zega.

## Import

Import is also how an existing database adopts Zega. After the tables are tracked:

```sql
SELECT zega.import();
```

Three scopes:

| Call | What it reloads |
| --- | --- |
| `SELECT zega.import();` | The whole graph. Every tracked table is loaded, nodes first, then edges. |
| `SELECT zega.import('users');` | The `users` type, and every tracked `ze_` edge that starts or ends on `users`. |
| `SELECT zega.import('ze_liked');` | That one relationship. `liked` edges are replaced. Nodes stay. |

An untracked name raises `pg_zega table ... is not tracked`. With nothing tracked, `zega.import()` raises `pg_zega has nothing tracked to import`.

Engine ids are new after a reload. Edges are linked again from the foreign-key columns, so the old ids are not reused.

## Build on GitHub

Postgres extensions are distributed as source and compiled with [PGXS](https://www.postgresql.org/docs/current/extend-pgxs.html) on the server that loads them. `make` and `make install` write `pg_zega.so` into that server's library directory and the control file plus `pg_zega--0.1.sql` into its extension directory. A library built for another major version, another OS, or another libc will not load there.

This repository is that source. [PGXN](https://pgxn.org/) and distro packages (one package per Postgres major, the way PGDG ships extension debs) are later ways to hand the same source to `pgxn install` or `apt`. Both still compile, or they publish one binary per major version and per distribution.

The GitHub Actions workflow builds `postgres/Dockerfile` on every push and pull request. That image runs `make` and `make install` on Postgres 17. The workflow records that the source still compiles. It does not upload a shared library.

`test.sh` is the sync test. It needs a Zega server already listening on `127.0.0.1:9342`. It runs `docker compose down -v` first, which drops this compose project's database volume, then deletes that Zega graph and runs `test.sql`. Run it only against a database you can throw away.

## License

Apache License 2.0. See [LICENSE](LICENSE).
