#ifndef API_H
#define API_H

#include "http.h"
#include "mongo.h"
#include "amqp_io.h"

typedef struct {
	MongoCtx*  mongo;
	AmqpPub*   pub;
	uint64_t   chunk_size;
} ApiCtx;

HttpRes api_crack(const HttpReq* req, void* ud);
HttpRes api_status(const HttpReq* req, void* ud);
HttpRes api_metrics(const HttpReq* req, void* ud);
HttpRes api_health(const HttpReq* req, void* ud);

#ifdef API_IMPL

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "../thirdparty/jsmn.h"
#include "uuid.h"
#include "jsonu.h"
#include "scheduler.h"
#include "metrics.h"

typedef struct { char* buf; size_t len; } StaticBody;
static __thread StaticBody g_body;
static HttpRes json_response(int status, char* body, size_t len) {
	if (g_body.buf) { free(g_body.buf); g_body.buf = NULL; }
	g_body.buf = body; g_body.len = len;
	HttpRes r = { .status = status, .content_type = "application/json",
	              .body = body, .body_len = len };
	return r;
}

static HttpRes plain_response(int status, const char* msg) {
	if (g_body.buf) { free(g_body.buf); g_body.buf = NULL; }
	HttpRes r = { .status = status, .content_type = "text/plain",
	              .body = msg, .body_len = strlen(msg) };
	return r;
}

static int hex_only(const char* s) {
	for (; *s; ++s) {
		char c = *s;
		if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')))
			return 0;
	}
	return 1;
}

HttpRes api_crack(const HttpReq* req, void* ud) {
	ApiCtx* ctx = (ApiCtx*)ud;
	if (!req->body || req->body_len == 0) {
		metrics_inc_http_request("/api/hash/crack", 400);
		return plain_response(400, "missing body");
	}
	jsmn_parser p; jsmntok_t tok[16]; jsmn_init(&p);
	int n = jsmn_parse(&p, req->body, req->body_len, tok, 16);
	if (n < 1 || tok[0].type != JSMN_OBJECT) {
		metrics_inc_http_request("/api/hash/crack", 400);
		return plain_response(400, "invalid json");
	}
	char hash[64] = {0}; uint64_t maxLen = 0;
	for (int i = 1; i < n; ) {
		jsmntok_t* k = &tok[i]; jsmntok_t* v = &tok[i + 1];
		if (k->type != JSMN_STRING) { i++; continue; }
		size_t klen = (size_t)(k->end - k->start);
		const char* kp = req->body + k->start;
		if (klen == 4 && strncmp(kp, "hash", 4) == 0 && v->type == JSMN_STRING) {
			size_t cp = (size_t)(v->end - v->start);
			if (cp >= sizeof(hash)) cp = sizeof(hash) - 1;
			memcpy(hash, req->body + v->start, cp); hash[cp] = 0;
		} else if (klen == 9 && strncmp(kp, "maxLength", 9) == 0 && v->type == JSMN_PRIMITIVE) {
			char buf[16]; size_t cp = (size_t)(v->end - v->start);
			if (cp >= sizeof(buf)) cp = sizeof(buf) - 1;
			memcpy(buf, req->body + v->start, cp); buf[cp] = 0;
			maxLen = strtoull(buf, NULL, 10);
		}
		i += 2;
		if (v->type == JSMN_OBJECT || v->type == JSMN_ARRAY) i += v->size;
	}

	if (!hash[0] || strlen(hash) != 32 || !hex_only(hash)) {
		metrics_inc_http_request("/api/hash/crack", 400);
		return plain_response(400, "invalid hash (need 32 hex chars)");
	}
	if (maxLen < 1 || maxLen > 8) {
		metrics_inc_http_request("/api/hash/crack", 400);
		return plain_response(400, "invalid maxLength (1..8)");
	}
	
	for (char* c = hash; *c; ++c) if (*c >= 'A' && *c <= 'Z') *c = (char)(*c + 32);

	uint64_t total = sched_total_combinations(maxLen);
	if (total == 0) {
		metrics_inc_http_request("/api/hash/crack", 400);
		return plain_response(400, "maxLength too large");
	}
	uint64_t chunk = ctx->chunk_size ? ctx->chunk_size : 100000;
	int64_t total_tasks = (int64_t)((total + chunk - 1) / chunk);

	char rid[37]; uuidv4(rid);
	if (mongo_insert_request(ctx->mongo, rid, hash, maxLen, total_tasks) != 0) {
		metrics_inc_http_request("/api/hash/crack", 503);
		return plain_response(503, "mongo write failed");
	}
	metrics_inc_requests_created();

	int64_t made = sched_dispatch_request(ctx->mongo, ctx->pub, rid, hash, maxLen, chunk);
	if (made < 0) {
		mongo_set_request_status(ctx->mongo, rid, "ERROR");
		metrics_inc_http_request("/api/hash/crack", 500);
		return plain_response(500, "task dispatch failed");
	}

	JBuf b; jbuf_init(&b);
	jbuf_putc(&b, '{'); jbuf_kv_str(&b, "requestId", rid); jbuf_putc(&b, '}');
	metrics_inc_http_request("/api/hash/crack", 200);
	return json_response(200, b.data, b.len);
}

HttpRes api_status(const HttpReq* req, void* ud) {
	ApiCtx* ctx = (ApiCtx*)ud;
	char rid[64];
	if (http_query_get(req->query, "requestId", rid, sizeof(rid)) != 0) {
		metrics_inc_http_request("/api/hash/status", 400);
		return plain_response(400, "requestId required");
	}
	MongoRequestStatus st = {0};
	if (mongo_get_request_status(ctx->mongo, rid, &st) != 0) {
		metrics_inc_http_request("/api/hash/status", 404);
		return plain_response(404, "not found");
	}
	JBuf b; jbuf_init(&b);
	jbuf_putc(&b, '{');
	jbuf_kv_str(&b, "status", st.status[0] ? st.status : "IN_PROGRESS");
	if (strcmp(st.status, "READY") == 0 || strcmp(st.status, "ERROR") == 0) {
		jbuf_putc(&b, ',');
		jbuf_str(&b, "results"); jbuf_putc(&b, ':');
		jbuf_putc(&b, '[');
		for (size_t i = 0; i < st.n_results; ++i) {
			if (i > 0) jbuf_putc(&b, ',');
			jbuf_str(&b, st.results[i]);
		}
		jbuf_putc(&b, ']');
	}
	jbuf_putc(&b, '}');
	mongo_request_status_free(&st);
	metrics_inc_http_request("/api/hash/status", 200);
	return json_response(200, b.data, b.len);
}

HttpRes api_metrics(const HttpReq* req, void* ud) {
	(void)req; (void)ud;
	size_t l = 0; char* body = metrics_render(&l);
	if (g_body.buf) { free(g_body.buf); }
	g_body.buf = body; g_body.len = l;
	HttpRes r = { .status = 200, .content_type = "text/plain; version=0.0.4",
	              .body = body, .body_len = l };
	return r;
}

HttpRes api_health(const HttpReq* req, void* ud) {
	(void)req; (void)ud;
	return plain_response(200, "ok");
}

#endif // API_IMPL
#endif // API_H
