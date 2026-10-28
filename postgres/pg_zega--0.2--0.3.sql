-- 0.2 to 0.3: zega.delivery_status (APS 52).

-- How far Zega is behind in journal mode (APS 52): the delivery worker is
-- running, how many committed rows wait in zega.journal and since when, and
-- how much WAL slot zega keeps Postgres from recycling. waiting stays 0
-- in precommit mode.
CREATE VIEW delivery_status AS
SELECT
    EXISTS (SELECT 1 FROM pg_stat_activity WHERE backend_type = 'pg_zega delivery') AS worker_running,
    (SELECT count(*) FROM journal) AS waiting,
    (SELECT min(written_at) FROM journal) AS oldest_waiting,
    (SELECT clock_timestamp() - min(written_at) FROM journal) AS lag,
    (SELECT pg_wal_lsn_diff(pg_current_wal_lsn(), restart_lsn)
       FROM pg_replication_slots WHERE slot_name = 'zega') AS retained_wal_bytes;
