//! pg_zega-relay: delivers pg_zega's journal to Zega (APS 52).
//!
//! A database in journal mode (`zega.mode = journal`) writes one
//! `zega.journal` row per transaction, in that transaction. pg_zega's logical
//! decoding output plugin emits those rows in commit order. The relay peeks
//! the slot, sends each row to Zega's `POST /deliveries` under the identity
//! `(pg:<system identifier>:<database oid>, <txid>)`, and only then deletes
//! the row and moves the slot past it.
//!
//! A crash anywhere in between redelivers the same row, and Zega answers with
//! the original receipt, so every transaction is applied once. A delivery
//! Zega refuses stops the relay: nothing after it is applied out of order.

use std::time::Duration;

use postgres::{Client, NoTls};
use serde::Deserialize;
use serde_json::{json, Value};

const USAGE: &str = "usage: pg_zega-relay --postgres <connection string> [--postgres-password-file <file>]
                     --zega <url> --zega-token-file <file> [--slot zega] [--poll-ms 500] [--once]

  --postgres   libpq-style keywords, e.g. \"host=127.0.0.1 port=5433 user=postgres dbname=app\"
  --once       deliver everything committed so far, then exit";

struct Options {
    postgres: String,
    postgres_password: Option<String>,
    zega: String,
    zega_token: String,
    slot: String,
    poll: Duration,
    once: bool,
}

/// One committed transaction, as the output plugin writes it.
#[derive(Deserialize)]
struct Entry {
    txid: String,
    schema: String,
    document: String,
}

/// Why the relay stopped.
enum Stop {
    /// Zega refused a delivery: it needs a person.
    Refused { txid: String, status: u16, body: String },
    Postgres(postgres::Error),
    Other(String),
}

impl From<postgres::Error> for Stop {
    fn from(error: postgres::Error) -> Self {
        Stop::Postgres(error)
    }
}

fn main() {
    let options = match options(std::env::args().skip(1).collect()) {
        Ok(options) => options,
        Err(message) => {
            eprintln!("{message}\n\n{USAGE}");
            std::process::exit(2);
        }
    };
    match run(&options) {
        Ok(()) => {}
        Err(Stop::Refused { txid, status, body }) => {
            eprintln!(
                "pg_zega-relay stopped: Zega refused txid {txid} ({status}): {body}\n\
                 Nothing after it is delivered. The row stays in zega.journal and in the slot."
            );
            std::process::exit(1);
        }
        Err(Stop::Postgres(error)) => {
            match error.as_db_error() {
                Some(db) => eprintln!("pg_zega-relay stopped: postgres: {}", db.message()),
                None => eprintln!("pg_zega-relay stopped: postgres: {error}"),
            }
            std::process::exit(1);
        }
        Err(Stop::Other(message)) => {
            eprintln!("pg_zega-relay stopped: {message}");
            std::process::exit(1);
        }
    }
}

fn options(args: Vec<String>) -> Result<Options, String> {
    let mut postgres = None;
    let mut postgres_password = None;
    let mut zega = None;
    let mut zega_token = None;
    let mut slot = "zega".to_string();
    let mut poll = Duration::from_millis(500);
    let mut once = false;
    let mut args = args.into_iter();
    while let Some(flag) = args.next() {
        let mut value = || args.next().ok_or_else(|| format!("{flag} needs a value"));
        match flag.as_str() {
            "--postgres" => postgres = Some(value()?),
            "--postgres-password-file" => postgres_password = Some(read_secret(&value()?)?),
            "--zega" => zega = Some(value()?.trim_end_matches('/').to_string()),
            "--zega-token-file" => zega_token = Some(read_secret(&value()?)?),
            "--slot" => slot = value()?,
            "--poll-ms" => {
                let ms = value()?;
                poll = Duration::from_millis(ms.parse().map_err(|_| format!("--poll-ms {ms} is not a number"))?);
            }
            "--once" => once = true,
            "--help" | "-h" => return Err("pg_zega-relay".into()),
            other => return Err(format!("unknown argument {other}")),
        }
    }
    Ok(Options {
        postgres: postgres.ok_or("--postgres is required")?,
        postgres_password,
        zega: zega.ok_or("--zega is required")?,
        zega_token: zega_token.ok_or("--zega-token-file is required")?,
        slot,
        poll,
        once,
    })
}

/// A one-word secret file; a trailing newline is not part of it.
fn read_secret(path: &str) -> Result<String, String> {
    let text = std::fs::read_to_string(path).map_err(|error| format!("cannot read {path}: {error}"))?;
    let secret = text.trim_end_matches(['\n', '\r']);
    if secret.is_empty() || secret.chars().any(|c| c.is_whitespace() || c.is_control()) {
        return Err(format!("{path} must hold one word"));
    }
    Ok(secret.to_string())
}

fn run(options: &Options) -> Result<(), Stop> {
    let mut config: postgres::Config = options.postgres.parse()?;
    if let Some(password) = &options.postgres_password {
        config.password(password);
    }
    let mut db = config.connect(NoTls)?;
    let source_id = source_id(&mut db)?;
    ensure_slot(&mut db, &options.slot)?;
    eprintln!("pg_zega-relay: {source_id}, slot {}, to {}", options.slot, options.zega);
    let agent = ureq::AgentBuilder::new().timeout(Duration::from_secs(30)).build();
    loop {
        let delivered = drain(&mut db, &agent, options, &source_id)?;
        if options.once && delivered == 0 {
            return Ok(());
        }
        if delivered == 0 {
            std::thread::sleep(options.poll);
        }
    }
}

/// `pg:<system identifier>:<database oid>`: one cluster's one database.
fn source_id(db: &mut Client) -> Result<String, Stop> {
    let row = db.query_one(
        "SELECT (SELECT system_identifier::text FROM pg_control_system()), \
                (SELECT oid::text FROM pg_database WHERE datname = current_database())",
        &[],
    )?;
    Ok(format!("pg:{}:{}", row.get::<_, String>(0), row.get::<_, String>(1)))
}

fn ensure_slot(db: &mut Client, slot: &str) -> Result<(), Stop> {
    match db.query_opt("SELECT plugin FROM pg_replication_slots WHERE slot_name = $1", &[&slot])? {
        Some(row) => {
            let plugin: Option<String> = row.get(0);
            if plugin.as_deref() != Some("pg_zega") {
                return Err(Stop::Other(format!("slot {slot} exists but does not use the pg_zega plugin")));
            }
        }
        None => {
            // A slot decodes only what commits after it exists: rows already
            // in the journal would never be delivered.
            let waiting: i64 = db.query_one("SELECT count(*) FROM zega.journal", &[])?.get(0);
            if waiting > 0 {
                return Err(Stop::Other(format!(
                    "zega.journal holds {waiting} rows written before slot {slot} existed; \
                     create the slot before setting zega.mode = journal"
                )));
            }
            db.execute("SELECT pg_create_logical_replication_slot($1, 'pg_zega')", &[&slot])?;
        }
    }
    Ok(())
}

const BATCH: i32 = 100;

/// Deliver what has committed up to now, in commit order; returns how many
/// transactions were delivered.
fn drain(db: &mut Client, agent: &ureq::Agent, options: &Options, source_id: &str) -> Result<usize, Stop> {
    let slot = options.slot.as_str();
    // Everything that committed before `upto` is decoded by this peek, unless
    // the batch fills first.
    let upto: String = db.query_one("SELECT pg_current_wal_flush_lsn()::text", &[])?.get(0);
    let rows = db.query(
        "SELECT lsn::text, data FROM pg_logical_slot_peek_changes($1, $2::text::pg_lsn, $3)",
        &[&slot, &upto, &BATCH],
    )?;
    for row in &rows {
        let lsn: String = row.get(0);
        let data: String = row.get(1);
        let entry: Entry = serde_json::from_str(&data)
            .map_err(|error| Stop::Other(format!("the slot emitted something that is not a journal entry ({error}): {data}")))?;
        deliver(agent, options, source_id, &entry)?;
        // Delete, then move the slot: a crash between the two redelivers the
        // row (Zega returns its receipt) and never strands it in the table.
        db.execute("DELETE FROM zega.journal WHERE txid = $1::text::xid8", &[&entry.txid])?;
        db.execute("SELECT pg_replication_slot_advance($1, $2::text::pg_lsn)", &[&slot, &lsn])?;
        eprintln!("pg_zega-relay: delivered txid {} at {lsn}", entry.txid);
    }
    if rows.len() < BATCH as usize {
        // Nothing else committed before `upto`: the slot can let go of the WAL
        // that only held transactions without journal rows.
        db.execute(
            "SELECT pg_replication_slot_advance($1, $2::text::pg_lsn) \
             FROM pg_replication_slots WHERE slot_name = $1 AND confirmed_flush_lsn < $2::text::pg_lsn",
            &[&slot, &upto],
        )?;
    }
    Ok(rows.len())
}

/// `POST /deliveries` until Zega answers. An unknown outcome (no answer, or
/// 503) is retried with the identical request; any other refusal stops.
fn deliver(agent: &ureq::Agent, options: &Options, source_id: &str, entry: &Entry) -> Result<(), Stop> {
    let body = json!({
        "source_id": source_id,
        "txid": entry.txid,
        "schema": entry.schema,
        "document": entry.document,
    });
    let mut wait = Duration::from_millis(200);
    loop {
        let sent = agent
            .post(&format!("{}/deliveries", options.zega))
            .set("Authorization", &format!("Bearer {}", options.zega_token))
            .send_json(&body);
        match sent {
            Ok(_) => return Ok(()),
            Err(ureq::Error::Status(status, response)) if status != 503 => {
                let body = response.into_string().unwrap_or_default();
                let body = serde_json::from_str::<Value>(&body).map(|v| v.to_string()).unwrap_or(body);
                return Err(Stop::Refused { txid: entry.txid.clone(), status, body });
            }
            Err(error) => {
                eprintln!("pg_zega-relay: txid {} not delivered yet ({error}); retrying in {wait:?}", entry.txid);
                std::thread::sleep(wait);
                wait = (wait * 2).min(Duration::from_secs(5));
            }
        }
    }
}
