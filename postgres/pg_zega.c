#include "postgres.h"

#include "access/htup_details.h"
#include "access/xact.h"
#include "catalog/pg_type.h"
#include "executor/spi.h"
#include "fmgr.h"
#include "lib/stringinfo.h"
#include "miscadmin.h"
#include "replication/logical.h"
#include "replication/output_plugin.h"
#include "pgstat.h"
#include "postmaster/bgworker.h"
#include "postmaster/interrupt.h"
#include "storage/ipc.h"
#include "storage/latch.h"
#include "tcop/tcopprot.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/json.h"
#include "utils/jsonb.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/timestamp.h"
#include "utils/xid8.h"

#include <curl/curl.h>
#include <stdlib.h>
#include <string.h>

PG_MODULE_MAGIC;

/*
 * zega.mode: how a commit reaches Zega.
 * precommit: sent before Postgres commits; a refusal rolls the transaction back.
 * journal: written to zega.journal in the transaction itself; pg_zega's
 * logical decoding output plugin hands it, in commit order, to a relay that
 * delivers it to Zega (APS 52).
 */
enum
{
	ZEGA_MODE_PRECOMMIT,
	ZEGA_MODE_JOURNAL
};

static const struct config_enum_entry zega_mode_options[] = {
	{"precommit", ZEGA_MODE_PRECOMMIT, false},
	{"journal", ZEGA_MODE_JOURNAL, false},
	{NULL, 0, false}
};

static char *zega_url = NULL;
static char *zega_password = NULL;
static char *zega_journal_database = NULL;
static int zega_mode = ZEGA_MODE_PRECOMMIT;
static StringInfo pending = NULL;
static int pending_ops = 0;

typedef struct {
	char data[2048];
	size_t len;
} ResponseBuf;

static size_t
write_cb(char *ptr, size_t size, size_t nmemb, void *userdata)
{
	ResponseBuf *buf = (ResponseBuf *) userdata;
	size_t incoming = size * nmemb;
	size_t room = sizeof(buf->data) - 1 - buf->len;

	if (incoming > room)
		incoming = room;
	if (incoming > 0)
	{
		memcpy(buf->data + buf->len, ptr, incoming);
		buf->len += incoming;
		buf->data[buf->len] = '\0';
	}
	return size * nmemb;
}

static void
clear_pending(void)
{
	if (pending != NULL)
	{
		/* Frees the buffer and the StringInfo itself. */
		destroyStringInfo(pending);
		pending = NULL;
	}
	pending_ops = 0;
}

static char *
json_escape_dup(const char *src)
{
	size_t cap = strlen(src) * 6 + 1;
	char *dst = malloc(cap);
	size_t n = 0;

	if (dst == NULL)
		return NULL;
	for (; *src != '\0'; src++)
	{
		unsigned char c = (unsigned char) *src;

		if (c == '"' || c == '\\')
		{
			dst[n++] = '\\';
			dst[n++] = (char) c;
		}
		else if (c == '\n')
		{
			dst[n++] = '\\';
			dst[n++] = 'n';
		}
		else if (c == '\r')
		{
			dst[n++] = '\\';
			dst[n++] = 'r';
		}
		else if (c == '\t')
		{
			dst[n++] = '\\';
			dst[n++] = 't';
		}
		else if (c < 0x20)
			n += snprintf(dst + n, 7, "\\u%04x", c);
		else
			dst[n++] = (char) c;
	}
	dst[n] = '\0';
	return dst;
}

static void
require_password(void)
{
	const unsigned char *p;

	if (zega_password == NULL || zega_password[0] == '\0')
		ereport(ERROR,
				(errmsg("pg_zega requires zega.password")));
	for (p = (const unsigned char *) zega_password; *p != '\0'; p++)
	{
		if (*p <= ' ' || *p == 127)
			ereport(ERROR,
					(errmsg("pg_zega password must be a single token")));
	}
}

static struct curl_slist *
auth_headers(struct curl_slist *headers)
{
	char *line;
	size_t n;
	struct curl_slist *next;

	require_password();
	n = strlen("Authorization: Bearer ") + strlen(zega_password) + 1;
	line = malloc(n);
	if (line == NULL)
		ereport(ERROR, (errmsg("pg_zega ran out of memory")));
	snprintf(line, n, "Authorization: Bearer %s", zega_password);
	next = curl_slist_append(headers, line);
	free(line);
	if (next == NULL)
		ereport(ERROR, (errmsg("pg_zega ran out of memory")));
	return next;
}

/*
 * One call to Zega with the bearer token: the transport result, and when it
 * is CURLE_OK the HTTP status and the start of the response body.
 */
static CURLcode
http_call(const char *url, const char *body, const char *method, long *status, ResponseBuf *response)
{
	CURL *curl;
	struct curl_slist *headers = NULL;
	CURLcode rc;

	*status = 0;
	memset(response, 0, sizeof(*response));
	headers = auth_headers(NULL);
	if (body != NULL)
	{
		struct curl_slist *next;

		next = curl_slist_append(headers, "Content-Type: application/json");
		if (next == NULL)
		{
			curl_slist_free_all(headers);
			ereport(ERROR, (errmsg("pg_zega ran out of memory")));
		}
		headers = next;
	}
	curl = curl_easy_init();
	if (curl == NULL)
	{
		curl_slist_free_all(headers);
		ereport(ERROR,
				(errmsg("pg_zega could not start the HTTP client")));
	}
	curl_easy_setopt(curl, CURLOPT_URL, url);
	if (method != NULL)
		curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method);
	if (body != NULL)
		curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, response);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
	curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
	rc = curl_easy_perform(curl);
	if (rc == CURLE_OK)
		curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, status);
	curl_slist_free_all(headers);
	curl_easy_cleanup(curl);
	return rc;
}

static void
http_request(const char *url, const char *body, const char *method)
{
	ResponseBuf response;
	long status;
	CURLcode rc = http_call(url, body, method, &status, &response);

	if (rc != CURLE_OK)
		ereport(ERROR,
				(errmsg("pg_zega could not reach Zega: %s", curl_easy_strerror(rc))));
	if (status < 200 || status >= 300)
		ereport(ERROR,
				(errmsg("pg_zega sync was rejected (%ld): %s",
						status,
						response.len > 0 ? response.data : "no detail")));
}

static void
put_schema(const char *base, const char *schema)
{
	char *escaped;
	char *body;
	char url[1024];
	size_t body_len;

	escaped = json_escape_dup(schema);
	if (escaped == NULL)
		ereport(ERROR, (errmsg("pg_zega ran out of memory")));
	body_len = strlen(escaped) + 16;
	body = malloc(body_len);
	if (body == NULL)
	{
		free(escaped);
		ereport(ERROR, (errmsg("pg_zega ran out of memory")));
	}
	snprintf(body, body_len, "{\"schema\":\"%s\"}", escaped);
	free(escaped);
	snprintf(url, sizeof(url), "%s/schema", base);
	PG_TRY();
	{
		http_request(url, body, "PUT");
	}
	PG_FINALLY();
	{
		free(body);
	}
	PG_END_TRY();
}

static void
append_id(long long **ids, size_t *n, size_t *cap, long long id)
{
	if (*n + 1 > *cap)
	{
		size_t next = *cap == 0 ? 8 : *cap * 2;
		long long *grown = realloc(*ids, next * sizeof(long long));

		if (grown == NULL)
			ereport(ERROR, (errmsg("pg_zega ran out of memory")));
		*ids = grown;
		*cap = next;
	}
	(*ids)[(*n)++] = id;
}

static long long
flush_id(const char *chunk)
{
	char *end = NULL;
	long long id = strtoll(chunk + 3, &end, 10);

	if (end == chunk + 3 || *end != '\0' || id < 0)
		ereport(ERROR, (errmsg("pg_zega flush id is not valid")));
	return id;
}

/*
 * Journal mode: this transaction's flush, schema and document become one
 * row. The flush (JSON, as a delivery's "flush") and the schema may be NULL.
 */
static void
journal_insert(const char *flush, const char *schema, const char *document)
{
	Oid types[3] = {TEXTOID, TEXTOID, TEXTOID};
	Datum values[3];
	char nulls[3] = {' ', ' ', ' '};
	int rc;

	values[0] = flush != NULL ? CStringGetTextDatum(flush) : (Datum) 0;
	values[1] = schema != NULL ? CStringGetTextDatum(schema) : (Datum) 0;
	values[2] = CStringGetTextDatum(document);
	if (flush == NULL)
		nulls[0] = 'n';
	if (schema == NULL)
		nulls[1] = 'n';
	PushActiveSnapshot(GetTransactionSnapshot());
	if (SPI_connect() != SPI_OK_CONNECT)
		ereport(ERROR, (errmsg("pg_zega could not write the journal")));
	rc = SPI_execute_with_args(
		"INSERT INTO zega.journal (txid, flush, schema, document) VALUES (pg_current_xact_id(), $1::jsonb, $2, $3)",
		3, types, values, nulls, false, 0);
	if (rc != SPI_OK_INSERT)
		ereport(ERROR, (errmsg("pg_zega could not write the journal (%d)", rc)));
	SPI_finish();
	PopActiveSnapshot();
}

/* Add one name to a flush list ("labels" or "relationships"). */
static void
append_name(StringInfo list, const char *name)
{
	if (list->len > 0)
		appendStringInfoChar(list, ',');
	escape_json(list, name);
}

/*
 * The flush of a journal entry: everything after an "R", else the labels
 * and relationship types import named with "FN" and "FR". NULL for none.
 */
static char *
flush_json(int reset, StringInfo labels, StringInfo relationships)
{
	StringInfoData flush;

	if (!reset && labels->len == 0 && relationships->len == 0)
		return NULL;
	initStringInfo(&flush);
	if (reset)
		appendStringInfoString(&flush, "{\"all\":true}");
	else
		appendStringInfo(&flush, "{\"labels\":[%s],\"relationships\":[%s]}",
						 labels->data, relationships->data);
	return flush.data;
}

static void
post_apply(void)
{
	char *copy;
	char *chunk;
	char *schema = NULL;
	char *mutations = NULL;
	size_t mutations_len = 0;
	size_t mutations_cap = 0;
	long long *node_ids = NULL;
	long long *rel_ids = NULL;
	size_t node_n = 0;
	size_t rel_n = 0;
	size_t node_cap = 0;
	size_t rel_cap = 0;
	size_t i;
	char *schema_text = NULL;
	char *document = NULL;
	char *escaped = NULL;
	char *body = NULL;
	char url[1024];
	const char *base;
	int reset = 0;
	StringInfo flush_labels;
	StringInfo flush_rels;
	char *flush = NULL;

	if (pending_ops == 0)
		return;

	flush_labels = makeStringInfo();
	flush_rels = makeStringInfo();

	base = zega_url != NULL ? zega_url : "http://host.docker.internal:9342";
	copy = strdup(pending->data);
	if (copy == NULL)
		ereport(ERROR, (errmsg("pg_zega ran out of memory")));
	chunk = copy;
	while (chunk != NULL)
	{
		char *sep = strchr(chunk, '\x1e');

		if (sep != NULL)
			*sep = '\0';
		if (strcmp(chunk, "R") == 0)
			reset = 1;
		else if (strncmp(chunk, "DN ", 3) == 0)
			append_id(&node_ids, &node_n, &node_cap, flush_id(chunk));
		else if (strncmp(chunk, "DR ", 3) == 0)
			append_id(&rel_ids, &rel_n, &rel_cap, flush_id(chunk));
		else if (strncmp(chunk, "FN ", 3) == 0)
			append_name(flush_labels, chunk + 3);
		else if (strncmp(chunk, "FR ", 3) == 0)
			append_name(flush_rels, chunk + 3);
		else if (strncmp(chunk, "schema", 6) == 0)
			schema = chunk;
		else
		{
			size_t n = strlen(chunk);
			size_t extra = n + (mutations_len > 0 ? 1 : 0);

			if (mutations_len + extra + 1 > mutations_cap)
			{
				mutations_cap = mutations_len + extra + 1;
				mutations = realloc(mutations, mutations_cap);
			}
			if (mutations_len > 0)
				mutations[mutations_len++] = '\n';
			memcpy(mutations + mutations_len, chunk, n);
			mutations_len += n;
			mutations[mutations_len] = '\0';
		}
		if (sep == NULL)
			break;
		chunk = sep + 1;
	}

	/*
	 * Precommit mode flushes engine ids read from Zega now (DN, DR). A
	 * journal entry is applied later, when those ids may mean nothing, so
	 * journal mode names what to flush by type (FN, FR) and Zega finds it
	 * when the entry is delivered.
	 */
	if ((zega_mode == ZEGA_MODE_JOURNAL && (node_n > 0 || rel_n > 0))
		|| (zega_mode == ZEGA_MODE_PRECOMMIT && (flush_labels->len > 0 || flush_rels->len > 0)))
	{
		free(copy);
		free(mutations);
		free(node_ids);
		free(rel_ids);
		ereport(ERROR,
				(errmsg("pg_zega flush does not match zega.mode"),
				 errhint("Run the import and its commit under the same zega.mode.")));
	}
	if (zega_mode == ZEGA_MODE_JOURNAL)
	{
		flush = flush_json(reset, flush_labels, flush_rels);
		reset = 0;
	}

	if (reset)
	{
		snprintf(url, sizeof(url), "%s/graph", base);
		http_request(url, NULL, "DELETE");
	}
	else
	{
		for (i = 0; i < rel_n; i++)
		{
			snprintf(url, sizeof(url), "%s/graph/relationships/%lld", base, rel_ids[i]);
			http_request(url, NULL, "DELETE");
		}
		for (i = 0; i < node_n; i++)
		{
			snprintf(url, sizeof(url), "%s/graph/nodes/%lld", base, node_ids[i]);
			http_request(url, NULL, "DELETE");
		}
	}
	free(node_ids);
	free(rel_ids);

	if (schema == NULL)
	{
		free(copy);
		free(mutations);
		if (mutations_len > 0)
			ereport(ERROR,
					(errmsg("pg_zega sync has row changes and no schema")));
		/* Nothing is tracked any more: the entry empties the graph. */
		if (flush != NULL)
			journal_insert(flush, NULL, "");
		return;
	}

	schema_text = strdup(schema);
	free(copy);
	if (schema_text == NULL)
	{
		free(mutations);
		ereport(ERROR, (errmsg("pg_zega ran out of memory")));
	}
	document = malloc(strlen(schema_text) + mutations_len + 2);
	if (document == NULL)
	{
		free(schema_text);
		free(mutations);
		ereport(ERROR, (errmsg("pg_zega ran out of memory")));
	}
	memcpy(document, schema_text, strlen(schema_text) + 1);
	if (mutations_len > 0)
	{
		strcat(document, "\n");
		strcat(document, mutations);
	}
	free(mutations);
	if (zega_mode == ZEGA_MODE_JOURNAL)
	{
		PG_TRY();
		{
			journal_insert(flush, schema_text, document);
		}
		PG_FINALLY();
		{
			free(schema_text);
			free(document);
		}
		PG_END_TRY();
		return;
	}
	escaped = json_escape_dup(document);
	free(document);
	if (escaped == NULL)
	{
		free(schema_text);
		ereport(ERROR, (errmsg("pg_zega ran out of memory")));
	}
	body = malloc(strlen(escaped) + 32);
	if (body == NULL)
	{
		free(escaped);
		free(schema_text);
		ereport(ERROR, (errmsg("pg_zega ran out of memory")));
	}
	snprintf(body, strlen(escaped) + 32, "{\"document\":true,\"query\":\"%s\"}", escaped);
	free(escaped);
	snprintf(url, sizeof(url), "%s/zql", base);
	PG_TRY();
	{
		put_schema(base, schema_text);
		http_request(url, body, NULL);
	}
	PG_FINALLY();
	{
		free(schema_text);
		free(body);
	}
	PG_END_TRY();
}

static void
zega_xact_callback(XactEvent event, void *arg)
{
	(void) arg;
	if (event == XACT_EVENT_PRE_COMMIT)
		post_apply();
	else if (event == XACT_EVENT_COMMIT || event == XACT_EVENT_ABORT)
		clear_pending();
}

void
_PG_init(void)
{
	DefineCustomStringVariable("zega.url",
							   "Base URL of the Zega sync process.",
							   NULL,
							   &zega_url,
							   "http://host.docker.internal:9342",
							   PGC_SUSET,
							   0,
							   NULL,
							   NULL,
							   NULL);
	DefineCustomEnumVariable("zega.mode",
							 "How a commit reaches Zega: precommit or journal.",
							 NULL,
							 &zega_mode,
							 ZEGA_MODE_PRECOMMIT,
							 zega_mode_options,
							 PGC_SUSET,
							 0,
							 NULL,
							 NULL,
							 NULL);
	DefineCustomStringVariable("zega.password",
							   "Bearer token sent to Zega on every sync call.",
							   NULL,
							   &zega_password,
							   NULL,
							   PGC_SUSET,
							   GUC_SUPERUSER_ONLY | GUC_NO_SHOW_ALL | GUC_NOT_IN_SAMPLE,
							   NULL,
							   NULL,
							   NULL);
	DefineCustomStringVariable("zega.journal_database",
							   "Database whose zega.journal pg_zega delivers to Zega (journal mode).",
							   "Empty: no delivery worker. Needs shared_preload_libraries and wal_level = logical.",
							   &zega_journal_database,
							   NULL,
							   PGC_POSTMASTER,
							   0,
							   NULL,
							   NULL,
							   NULL);
	RegisterXactCallback(zega_xact_callback, NULL);
	curl_global_init(CURL_GLOBAL_ALL);

	if (process_shared_preload_libraries_in_progress
		&& zega_journal_database != NULL && zega_journal_database[0] != '\0')
	{
		BackgroundWorker worker;

		memset(&worker, 0, sizeof(worker));
		worker.bgw_flags = BGWORKER_SHMEM_ACCESS | BGWORKER_BACKEND_DATABASE_CONNECTION;
		worker.bgw_start_time = BgWorkerStart_RecoveryFinished;
		worker.bgw_restart_time = 5;
		snprintf(worker.bgw_library_name, BGW_MAXLEN, "pg_zega");
		snprintf(worker.bgw_function_name, BGW_MAXLEN, "zega_delivery_main");
		snprintf(worker.bgw_name, BGW_MAXLEN, "pg_zega delivery");
		snprintf(worker.bgw_type, BGW_MAXLEN, "pg_zega delivery");
		RegisterBackgroundWorker(&worker);
	}
}

typedef struct
{
	char *data;
	size_t len;
	size_t cap;
} DynBuf;

static size_t
dyn_write(char *ptr, size_t size, size_t nmemb, void *userdata)
{
	DynBuf *buf = (DynBuf *) userdata;
	size_t incoming = size * nmemb;

	if (buf->len + incoming + 1 > buf->cap)
	{
		size_t cap = buf->cap == 0 ? 4096 : buf->cap;
		char *grown;

		while (buf->len + incoming + 1 > cap)
			cap *= 2;
		grown = realloc(buf->data, cap);
		if (grown == NULL)
			return 0;
		buf->data = grown;
		buf->cap = cap;
	}
	memcpy(buf->data + buf->len, ptr, incoming);
	buf->len += incoming;
	buf->data[buf->len] = '\0';
	return incoming;
}

static char *
http_get_dup(const char *url)
{
	CURL *curl;
	struct curl_slist *headers = NULL;
	DynBuf buf;
	long status = 0;
	CURLcode rc;

	memset(&buf, 0, sizeof(buf));
	headers = auth_headers(NULL);
	{
		struct curl_slist *next;

		next = curl_slist_append(headers, "Accept: application/json");
		if (next == NULL)
		{
			curl_slist_free_all(headers);
			ereport(ERROR, (errmsg("pg_zega ran out of memory")));
		}
		headers = next;
	}
	curl = curl_easy_init();
	if (curl == NULL)
	{
		curl_slist_free_all(headers);
		ereport(ERROR, (errmsg("pg_zega could not start the HTTP client")));
	}
	curl_easy_setopt(curl, CURLOPT_URL, url);
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, dyn_write);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
	curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
	rc = curl_easy_perform(curl);
	if (rc == CURLE_OK)
		curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
	curl_slist_free_all(headers);
	curl_easy_cleanup(curl);
	if (rc != CURLE_OK || status < 200 || status >= 300)
	{
		free(buf.data);
		if (rc != CURLE_OK)
			ereport(ERROR,
					(errmsg("pg_zega could not reach Zega: %s", curl_easy_strerror(rc))));
		ereport(ERROR,
				(errmsg("pg_zega could not read the graph (%ld)", status)));
	}
	if (buf.data == NULL)
	{
		buf.data = strdup("");
		if (buf.data == NULL)
			ereport(ERROR, (errmsg("pg_zega ran out of memory")));
	}
	return buf.data;
}

PG_FUNCTION_INFO_V1(zega_graph);

Datum
zega_graph(PG_FUNCTION_ARGS)
{
	const char *base;
	char url[1024];
	char *body;
	text *result;

	(void) fcinfo;
	base = zega_url != NULL ? zega_url : "http://host.docker.internal:9342";
	snprintf(url, sizeof(url), "%s/graph", base);
	body = http_get_dup(url);
	result = cstring_to_text(body);
	free(body);
	PG_RETURN_TEXT_P(result);
}

PG_FUNCTION_INFO_V1(zega_prepare_reload);

Datum
zega_prepare_reload(PG_FUNCTION_ARGS)
{
	char *copy;
	char *chunk;

	(void) fcinfo;
	if (pending_ops == 0 || pending == NULL)
		PG_RETURN_VOID();
	copy = strdup(pending->data);
	if (copy == NULL)
		ereport(ERROR, (errmsg("pg_zega ran out of memory")));
	chunk = copy;
	while (chunk != NULL)
	{
		char *sep = strchr(chunk, '\x1e');

		if (sep != NULL)
			*sep = '\0';
		if (strncmp(chunk, "schema", 6) != 0)
		{
			free(copy);
			ereport(ERROR,
					(errmsg("pg_zega import must not share a transaction with row changes")));
		}
		if (sep == NULL)
			break;
		chunk = sep + 1;
	}
	free(copy);
	PG_RETURN_VOID();
}

PG_FUNCTION_INFO_V1(zega_enqueue);

Datum
zega_enqueue(PG_FUNCTION_ARGS)
{
	char *chunk;
	MemoryContext old;

	if (zega_mode == ZEGA_MODE_PRECOMMIT)
		require_password();
	chunk = text_to_cstring(PG_GETARG_TEXT_PP(0));
	old = MemoryContextSwitchTo(TopMemoryContext);
	if (pending == NULL)
		pending = makeStringInfo();
	if (pending_ops > 0)
		appendStringInfoChar(pending, '\x1e');
	appendStringInfoString(pending, chunk);
	pending_ops++;
	MemoryContextSwitchTo(old);
	pfree(chunk);
	PG_RETURN_VOID();
}

/*
 * Journal delivery (APS 52): a background worker in the database named by
 * zega.journal_database. It decodes the journal through the slot "zega"
 * (pg_zega's output plugin), sends each transaction to Zega's
 * POST /deliveries under the identity (pg:<system identifier>:<database
 * oid>, txid), then deletes the row and moves the slot past it. A crash
 * between the two redelivers the row, and Zega answers with its receipt.
 *
 * While Zega cannot be reached (or answers 503) the same row is retried. A
 * row Zega refuses holds everything after it, so the order holds, until a
 * full zega.import() is committed: that import holds every change before
 * it, so the rows before it are dropped and it is delivered.
 */
#define DELIVERY_SLOT "zega"
#define DELIVERY_BATCH 100
#define DELIVERY_POLL_MS 500L
#define DELIVERY_RETRY_MAX_MS 5000L
/* With nothing journaled, how often the slot is moved up to now. */
#define DELIVERY_IDLE_ADVANCE_MS 30000L

typedef struct
{
	char *lsn;
	char *data;
	uint64 txid;
} DeliveryRow;

/* What the last round left: the source identity, and a refused txid. */
static char *delivery_source = NULL;
static uint64 delivery_refused = 0;
/* A full import is waiting behind a refusal: drop rows until it. */
static bool delivery_recovering = false;
static long delivery_wait = DELIVERY_POLL_MS;
static TimestampTz delivery_advanced = 0;

typedef enum
{
	DELIVERED,
	NOT_YET,
	REFUSED
} DeliveryOutcome;

static char *
spi_first_text(const char *query, int nargs, Oid *types, Datum *values)
{
	bool isnull;

	if (SPI_execute_with_args(query, nargs, types, values, NULL, false, 1) < 0 || SPI_processed == 0)
		return NULL;
	(void) SPI_getbinval(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 1, &isnull);
	if (isnull)
		return NULL;
	return SPI_getvalue(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 1);
}

static void
delivery_begin(void)
{
	StartTransactionCommand();
	if (SPI_connect() != SPI_OK_CONNECT)
		ereport(ERROR, (errmsg("pg_zega delivery could not use SPI")));
	PushActiveSnapshot(GetTransactionSnapshot());
}

static void
delivery_end(void)
{
	SPI_finish();
	PopActiveSnapshot();
	CommitTransactionCommand();
}

/*
 * The slot exists, or is created now: false while the extension is not
 * installed, or while the journal holds rows no slot will ever decode.
 */
static bool
delivery_ready(void)
{
	char *found;

	if (spi_first_text("SELECT 1 FROM pg_extension WHERE extname = 'pg_zega'", 0, NULL, NULL) == NULL)
		return false;
	if (delivery_source == NULL)
	{
		char *source = spi_first_text(
			"SELECT 'pg:' || (SELECT system_identifier FROM pg_control_system()) || ':' "
			"|| (SELECT oid FROM pg_database WHERE datname = current_database())",
			0, NULL, NULL);

		if (source == NULL)
			return false;
		delivery_source = MemoryContextStrdup(TopMemoryContext, source);
	}
	found = spi_first_text("SELECT plugin FROM pg_replication_slots WHERE slot_name = '" DELIVERY_SLOT "'",
						   0, NULL, NULL);
	if (found != NULL)
	{
		if (strcmp(found, "pg_zega") != 0)
			ereport(ERROR, (errmsg("pg_zega slot " DELIVERY_SLOT " does not use the pg_zega plugin")));
		return true;
	}
	/* A slot decodes only what commits after it exists. */
	if (spi_first_text("SELECT 1 FROM zega.journal LIMIT 1", 0, NULL, NULL) != NULL)
	{
		ereport(WARNING,
				(errmsg("pg_zega journal has rows written before slot " DELIVERY_SLOT " existed; they will not be delivered"),
				 errhint("Run SELECT zega.import() after the slot exists, then delete the older rows.")));
		return false;
	}
	if (SPI_execute("SELECT pg_create_logical_replication_slot('" DELIVERY_SLOT "', 'pg_zega')", false, 0) < 0)
		ereport(ERROR, (errmsg("pg_zega could not create slot " DELIVERY_SLOT)));
	ereport(LOG, (errmsg("pg_zega created slot " DELIVERY_SLOT " for journal delivery")));
	return true;
}

/* POST one journal line to Zega as a delivery. */
static DeliveryOutcome
delivery_send(DeliveryRow *row)
{
	StringInfoData body;
	ResponseBuf response;
	long status;
	CURLcode rc;
	char url[1024];
	const char *base = zega_url != NULL ? zega_url : "http://host.docker.internal:9342";

	initStringInfo(&body);
	/* The line is {"txid": ...}: the identity goes first. */
	appendStringInfo(&body, "{\"source_id\":\"%s\",%s", delivery_source, row->data + 1);
	snprintf(url, sizeof(url), "%s/deliveries", base);
	rc = http_call(url, body.data, NULL, &status, &response);
	pfree(body.data);
	if (rc != CURLE_OK || status == 503)
	{
		ereport(LOG,
				(errmsg("pg_zega txid " UINT64_FORMAT " not delivered yet (%s); retrying",
						row->txid, rc != CURLE_OK ? curl_easy_strerror(rc) : "503")));
		return NOT_YET;
	}
	if (status >= 200 && status < 300)
		return DELIVERED;
	if (delivery_refused != row->txid)
		ereport(WARNING,
				(errmsg("Zega refused txid " UINT64_FORMAT " (%ld): %s", row->txid, status,
						response.len > 0 ? response.data : "no detail"),
				 errdetail("Nothing after it is delivered. The row stays in zega.journal."),
				 errhint("SELECT zega.import() replaces the graph; the rows before it are then dropped.")));
	delivery_refused = row->txid;
	return REFUSED;
}

/* Delete the row and move the slot past it, in one transaction. */
static void
delivery_done(DeliveryRow *row)
{
	Oid types[2] = {TEXTOID, TEXTOID};
	Datum values[2];
	char txid[32];

	snprintf(txid, sizeof(txid), UINT64_FORMAT, row->txid);
	delivery_begin();
	values[0] = CStringGetTextDatum(txid);
	values[1] = CStringGetTextDatum(row->lsn);
	if (SPI_execute_with_args("DELETE FROM zega.journal WHERE txid = $1::xid8", 1, types, values, NULL, false, 0) < 0
		|| SPI_execute_with_args("SELECT pg_replication_slot_advance('" DELIVERY_SLOT "', $1::pg_lsn)",
								 1, types + 1, values + 1, NULL, false, 0) < 0)
		ereport(ERROR, (errmsg("pg_zega could not mark txid %s delivered", txid)));
	delivery_end();
}

/* Is this row a full zega.import()? */
static bool
delivery_is_import(DeliveryRow *row)
{
	Oid type = TEXTOID;
	Datum value;
	char txid[32];
	bool import;

	snprintf(txid, sizeof(txid), UINT64_FORMAT, row->txid);
	delivery_begin();
	value = CStringGetTextDatum(txid);
	import = spi_first_text("SELECT 1 FROM zega.journal WHERE txid = $1::xid8 AND flush @> '{\"all\": true}'",
							1, &type, &value) != NULL;
	delivery_end();
	return import;
}

/*
 * One round: everything committed up to now, in commit order. Returns how
 * many rows it delivered or dropped; 0 means wait before the next round.
 */
static int
delivery_round(void)
{
	DeliveryRow rows[DELIVERY_BATCH];
	int n = 0;
	int handled = 0;
	int i;
	char *upto;
	MemoryContext round = AllocSetContextCreate(TopMemoryContext, "pg_zega delivery", ALLOCSET_DEFAULT_SIZES);
	MemoryContext old;

	delivery_begin();
	if (!delivery_ready())
	{
		delivery_end();
		MemoryContextDelete(round);
		return 0;
	}
	upto = spi_first_text("SELECT pg_current_wal_flush_lsn()::text", 0, NULL, NULL);
	/*
	 * Every row waiting is in zega.journal until it is delivered. With none,
	 * there is nothing to decode: each decoding logs a few lines, so the slot
	 * is only moved up now and then, to let go of the WAL behind it.
	 */
	if (spi_first_text("SELECT 1 FROM zega.journal LIMIT 1", 0, NULL, NULL) == NULL)
	{
		if (TimestampDifferenceExceeds(delivery_advanced, GetCurrentTimestamp(), DELIVERY_IDLE_ADVANCE_MS))
		{
			Oid type = TEXTOID;
			Datum value = CStringGetTextDatum(upto);

			if (SPI_execute_with_args("SELECT pg_replication_slot_advance('" DELIVERY_SLOT "', $1::pg_lsn) "
									  "FROM pg_replication_slots WHERE slot_name = '" DELIVERY_SLOT "' "
									  "AND confirmed_flush_lsn < $1::pg_lsn",
									  1, &type, &value, NULL, false, 0) < 0)
				ereport(ERROR, (errmsg("pg_zega could not advance slot " DELIVERY_SLOT)));
			delivery_advanced = GetCurrentTimestamp();
		}
		delivery_end();
		MemoryContextDelete(round);
		return 0;
	}
	{
		Oid type = TEXTOID;
		Datum value = CStringGetTextDatum(upto);
		uint64 k;

		if (SPI_execute_with_args("SELECT lsn::text, data FROM pg_logical_slot_peek_changes('" DELIVERY_SLOT
								  "', $1::pg_lsn, " CppAsString2(DELIVERY_BATCH) ")",
								  1, &type, &value, NULL, false, 0) < 0)
			ereport(ERROR, (errmsg("pg_zega could not read slot " DELIVERY_SLOT)));
		old = MemoryContextSwitchTo(round);
		upto = pstrdup(upto);
		for (k = 0; k < SPI_processed && n < DELIVERY_BATCH; k++)
		{
			HeapTuple tuple = SPI_tuptable->vals[k];
			TupleDesc desc = SPI_tuptable->tupdesc;
			unsigned long long txid;

			rows[n].lsn = pstrdup(SPI_getvalue(tuple, desc, 1));
			rows[n].data = pstrdup(SPI_getvalue(tuple, desc, 2));
			if (sscanf(rows[n].data, "{\"txid\":\"%llu\"", &txid) != 1)
				ereport(ERROR, (errmsg("pg_zega slot emitted something that is not a journal entry")));
			rows[n].txid = (uint64) txid;
			n++;
		}
		MemoryContextSwitchTo(old);
	}
	/* Held by a refusal: a full import waiting is the way past it. */
	if (n > 0 && rows[0].txid == delivery_refused && !delivery_recovering)
		delivery_recovering = spi_first_text("SELECT 1 FROM zega.journal WHERE flush @> '{\"all\": true}' LIMIT 1",
											 0, NULL, NULL) != NULL;
	delivery_end();

	if (n > 0 && rows[0].txid == delivery_refused && !delivery_recovering)
	{
		MemoryContextDelete(round);
		return 0;
	}
	for (i = 0; i < n; i++)
	{
		DeliveryOutcome outcome;

		if (delivery_recovering && !delivery_is_import(&rows[i]))
		{
			delivery_done(&rows[i]);
			ereport(LOG, (errmsg("pg_zega dropped txid " UINT64_FORMAT "; the import after it replaces it", rows[i].txid)));
			handled++;
			continue;
		}
		outcome = delivery_send(&rows[i]);
		if (outcome != DELIVERED)
		{
			if (outcome == NOT_YET)
				delivery_wait = Min(delivery_wait * 2, DELIVERY_RETRY_MAX_MS);
			MemoryContextDelete(round);
			return handled;
		}
		delivery_done(&rows[i]);
		if (delivery_recovering)
			ereport(LOG, (errmsg("pg_zega recovered: delivered the import txid " UINT64_FORMAT, rows[i].txid)));
		delivery_recovering = false;
		delivery_refused = 0;
		delivery_wait = DELIVERY_POLL_MS;
		handled++;
	}
	MemoryContextDelete(round);
	return handled;
}

PGDLLEXPORT void zega_delivery_main(Datum arg);

void
zega_delivery_main(Datum arg)
{
	(void) arg;
	pqsignal(SIGHUP, SignalHandlerForConfigReload);
	pqsignal(SIGTERM, die);
	BackgroundWorkerUnblockSignals();
	BackgroundWorkerInitializeConnection(zega_journal_database, NULL, 0);
	pgstat_report_appname("pg_zega delivery");
	for (;;)
	{
		int handled;

		CHECK_FOR_INTERRUPTS();
		if (ConfigReloadPending)
		{
			ConfigReloadPending = false;
			ProcessConfigFile(PGC_SIGHUP);
		}
		pgstat_report_activity(STATE_RUNNING, "delivering zega.journal");
		handled = delivery_round();
		pgstat_report_activity(STATE_IDLE, NULL);
		if (handled == 0)
		{
			(void) WaitLatch(MyLatch, WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH,
							 delivery_refused != 0 ? DELIVERY_RETRY_MAX_MS : delivery_wait,
							 PG_WAIT_EXTENSION);
			ResetLatch(MyLatch);
		}
	}
}

/*
 * The logical decoding output plugin: pg_create_logical_replication_slot(
 * 'zega', 'pg_zega'). Each committed transaction that wrote a zega.journal
 * row is one line of JSON, {"txid", "flush"?, "schema", "document"}, at the commit's
 * LSN, in commit order. Everything else is skipped.
 */
static void
journal_startup(LogicalDecodingContext *ctx, OutputPluginOptions *options, bool is_init)
{
	(void) ctx;
	(void) is_init;
	options->output_type = OUTPUT_PLUGIN_TEXTUAL_OUTPUT;
}

static void
journal_begin(LogicalDecodingContext *ctx, ReorderBufferTXN *txn)
{
	(void) ctx;
	txn->output_plugin_private = NULL;
}

static char *
journal_text(HeapTuple tuple, TupleDesc desc, const char *column)
{
	int attnum = SPI_fnumber(desc, column);
	bool isnull;
	Datum value;

	if (attnum <= 0)
		ereport(ERROR, (errmsg("pg_zega journal has no column %s", column)));
	value = heap_getattr(tuple, attnum, desc, &isnull);
	return isnull ? NULL : TextDatumGetCString(value);
}

/* A jsonb column as JSON text; NULL when the column is NULL. */
static char *
journal_json(HeapTuple tuple, TupleDesc desc, const char *column)
{
	int attnum = SPI_fnumber(desc, column);
	bool isnull;
	Datum value;
	Jsonb *json;

	if (attnum <= 0)
		ereport(ERROR, (errmsg("pg_zega journal has no column %s", column)));
	value = heap_getattr(tuple, attnum, desc, &isnull);
	if (isnull)
		return NULL;
	json = DatumGetJsonbP(value);
	return JsonbToCString(NULL, &json->root, VARSIZE(json));
}

static void
journal_change(LogicalDecodingContext *ctx, ReorderBufferTXN *txn,
			   Relation relation, ReorderBufferChange *change)
{
	TupleDesc desc = RelationGetDescr(relation);
	HeapTuple tuple;
	MemoryContext old;
	StringInfoData line;
	char *flush;
	char *schema;
	char *document;
	bool isnull;
	Datum txid;
	char *namespace;

	if (change->action != REORDER_BUFFER_CHANGE_INSERT)
		return;
	if (strcmp(RelationGetRelationName(relation), "journal") != 0)
		return;
	namespace = get_namespace_name(RelationGetNamespace(relation));
	if (namespace == NULL || strcmp(namespace, "zega") != 0)
		return;
	tuple = change->data.tp.newtuple;
	if (tuple == NULL)
		return;
	txid = heap_getattr(tuple, SPI_fnumber(desc, "txid"), desc, &isnull);
	if (isnull)
		ereport(ERROR, (errmsg("pg_zega journal row has no txid")));
	flush = journal_json(tuple, desc, "flush");
	schema = journal_text(tuple, desc, "schema");
	document = journal_text(tuple, desc, "document");
	if (document == NULL)
		ereport(ERROR, (errmsg("pg_zega journal row is incomplete")));

	old = MemoryContextSwitchTo(ctx->context);
	initStringInfo(&line);
	appendStringInfo(&line, "{\"txid\":\"" UINT64_FORMAT "\",",
					 U64FromFullTransactionId(DatumGetFullTransactionId(txid)));
	if (flush != NULL)
		appendStringInfo(&line, "\"flush\":%s,", flush);
	appendStringInfoString(&line, "\"schema\":");
	if (schema != NULL)
		escape_json(&line, schema);
	else
		appendStringInfoString(&line, "null");
	appendStringInfoString(&line, ",\"document\":");
	escape_json(&line, document);
	appendStringInfoChar(&line, '}');
	MemoryContextSwitchTo(old);
	/* One row per transaction: zega.journal's key is the txid. */
	txn->output_plugin_private = line.data;
}

static void
journal_commit(LogicalDecodingContext *ctx, ReorderBufferTXN *txn, XLogRecPtr commit_lsn)
{
	char *line = (char *) txn->output_plugin_private;

	(void) commit_lsn;
	if (line == NULL)
		return;
	OutputPluginPrepareWrite(ctx, true);
	appendStringInfoString(ctx->out, line);
	OutputPluginWrite(ctx, true);
	pfree(line);
	txn->output_plugin_private = NULL;
}

void
_PG_output_plugin_init(OutputPluginCallbacks *cb)
{
	cb->startup_cb = journal_startup;
	cb->begin_cb = journal_begin;
	cb->change_cb = journal_change;
	cb->commit_cb = journal_commit;
}
