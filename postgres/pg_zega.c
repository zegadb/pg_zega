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
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/json.h"
#include "utils/jsonb.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
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

static void
http_request(const char *url, const char *body, const char *method)
{
	CURL *curl;
	struct curl_slist *headers = NULL;
	ResponseBuf response;
	long status = 0;
	CURLcode rc;

	memset(&response, 0, sizeof(response));
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
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
	curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
	rc = curl_easy_perform(curl);
	if (rc == CURLE_OK)
		curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
	curl_slist_free_all(headers);
	curl_easy_cleanup(curl);

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
	RegisterXactCallback(zega_xact_callback, NULL);
	curl_global_init(CURL_GLOBAL_ALL);
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
 * The logical decoding output plugin: pg_create_logical_replication_slot(
 * 'zega', 'pg_zega'). Each committed transaction that wrote a zega.journal
 * row is one line of JSON, {"txid", "flush", "schema", "document"}, at the commit's
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
	appendStringInfo(&line, "{\"txid\":\"" UINT64_FORMAT "\",\"flush\":%s,\"schema\":",
					 U64FromFullTransactionId(DatumGetFullTransactionId(txid)),
					 flush != NULL ? flush : "null");
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
