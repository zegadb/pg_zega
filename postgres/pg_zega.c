#include "postgres.h"

#include "access/xact.h"
#include "fmgr.h"
#include "lib/stringinfo.h"
#include "miscadmin.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/memutils.h"

#include <curl/curl.h>
#include <stdlib.h>
#include <string.h>

PG_MODULE_MAGIC;

static char *zega_url = NULL;
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
		destroyStringInfo(pending);
		pfree(pending);
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
http_request(const char *url, const char *body, const char *method)
{
	CURL *curl;
	struct curl_slist *headers = NULL;
	ResponseBuf response;
	long status = 0;
	CURLcode rc;

	memset(&response, 0, sizeof(response));
	curl = curl_easy_init();
	if (curl == NULL)
		ereport(ERROR,
				(errmsg("pg_zega could not start the HTTP client")));
	if (body != NULL)
		headers = curl_slist_append(headers, "Content-Type: application/json");
	curl_easy_setopt(curl, CURLOPT_URL, url);
	if (method != NULL)
		curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method);
	if (body != NULL)
		curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
	if (headers != NULL)
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

	if (pending_ops == 0)
		return;

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
							   PGC_USERSET,
							   0,
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
	curl = curl_easy_init();
	if (curl == NULL)
		ereport(ERROR, (errmsg("pg_zega could not start the HTTP client")));
	headers = curl_slist_append(headers, "Accept: application/json");
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
	char *chunk = text_to_cstring(PG_GETARG_TEXT_PP(0));
	MemoryContext old;

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
