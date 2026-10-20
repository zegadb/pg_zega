CREATE EXTENSION IF NOT EXISTS pg_zega;
SET zega.password = 'pg_zega-test-token';

CREATE TABLE people (
    person_id int PRIMARY KEY,
    name text NOT NULL,
    active boolean NOT NULL DEFAULT true
);

SELECT zega.track('people');

INSERT INTO people (person_id, name) VALUES (1, 'Ada "Lovelace"');

ALTER TABLE people ADD COLUMN email text;

UPDATE people SET email = 'ada@example.com' WHERE person_id = 1;

-- Emptying a column empties it in Zega too.
ALTER TABLE people ADD COLUMN nickname text;
UPDATE people SET nickname = 'Countess' WHERE person_id = 1;
UPDATE people SET nickname = NULL WHERE person_id = 1;

BEGIN;
INSERT INTO people (person_id, name) VALUES (2, 'Grace Hopper');
ROLLBACK;

DO $$
BEGIN
    ALTER TABLE people ADD COLUMN photo bytea;
    RAISE EXCEPTION 'bytea was accepted';
EXCEPTION WHEN OTHERS THEN
    IF SQLERRM NOT LIKE '%cannot map%' THEN
        RAISE;
    END IF;
END $$;

INSERT INTO people (person_id, name) VALUES (3, 'temp');
DELETE FROM people WHERE person_id = 3;

CREATE TABLE users (
    user_id int PRIMARY KEY,
    name text NOT NULL
);
CREATE TABLE ze_friends (
    user_id int NOT NULL REFERENCES users (user_id) ON DELETE CASCADE,
    friend_id int NOT NULL REFERENCES users (user_id) ON DELETE CASCADE,
    since int NOT NULL,
    note text,
    PRIMARY KEY (user_id, friend_id)
);
SELECT zega.track('users');
SELECT zega.track('ze_friends');
INSERT INTO users (user_id, name) VALUES (10, 'Nia'), (11, 'Omar');
INSERT INTO ze_friends (user_id, friend_id, since, note) VALUES (10, 11, 2016, 'lab');
ALTER TABLE ze_friends ADD COLUMN met boolean;
UPDATE ze_friends SET met = true WHERE user_id = 10;
BEGIN;
INSERT INTO ze_friends (user_id, friend_id, since) VALUES (11, 10, 2017);
ROLLBACK;
