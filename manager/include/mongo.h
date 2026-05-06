#ifndef MONGO_H
#define MONGO_H

#include <stdint.h>
#include <stddef.h>

typedef struct MongoCtx MongoCtx;

// Initializes a thread-safe pool. Always uses write concern "majority".
// uri example: mongodb://mongo-0.mongo:27017,mongo-1.mongo:27017,mongo-2.mongo:27017/?replicaSet=rs0
MongoCtx* mongo_init(const char* uri, const char* db_name);
void      mongo_destroy(MongoCtx* m);

//   _id (string requestId), hash (string), maxLength (int64), status (string),
//   results (array<string>), total_tasks (int64), done_tasks (int64), createdAt, updatedAt
int mongo_insert_request(MongoCtx* m, const char* request_id, const char* hash,
                         uint64_t max_length, int64_t total_tasks);
int mongo_set_request_status(MongoCtx* m, const char* request_id, const char* status);

typedef struct {
	char    status[32];           // IN_PROGRESS | READY | ERROR | NOT_FOUND
	char**  results;
	size_t  n_results;
} MongoRequestStatus;

int  mongo_get_request_status(MongoCtx* m, const char* request_id, MongoRequestStatus* out);
void mongo_request_status_free(MongoRequestStatus* s);

int mongo_insert_task(MongoCtx* m, const char* task_id, const char* request_id,
                      uint64_t start_index, uint64_t count,
                      const char* target_hash, uint64_t max_length);
int mongo_set_task_status(MongoCtx* m, const char* task_id, const char* status);

int mongo_complete_task(MongoCtx* m, const char* task_id, const char* request_id,
                        const char* const* results, size_t n_results, int* out_was_new);

typedef struct {
	char     task_id[64];
	char     request_id[64];
	char     target_hash[64];
	uint64_t start_index;
	uint64_t count;
	uint64_t max_length;
	char     status[16];
} MongoTaskRow;

typedef struct {
	MongoTaskRow* items;
	size_t        count;
} MongoTaskList;

int  mongo_list_tasks_by_status(MongoCtx* m, const char* statuses, MongoTaskList* out);
void mongo_task_list_free(MongoTaskList* lst);

int mongo_timeout_requests(MongoCtx* m, int64_t timeout_ms);

#ifdef MONGO_IMPL

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <mongoc/mongoc.h>
#include <bson/bson.h>

#ifndef MONGO_LOG
#define MONGO_LOG(fmt, ...) fprintf(stderr, "[mongo] " fmt "\n", ##__VA_ARGS__)
#endif

struct MongoCtx {
	mongoc_uri_t*         uri;
	mongoc_client_pool_t* pool;
	char*                 db;
	mongoc_write_concern_t* wc_majority;
};

#define COLL_REQUESTS "requests"
#define COLL_TASKS    "tasks"

static int64_t now_ms(void) {
	struct timespec ts;
	clock_gettime(CLOCK_REALTIME, &ts);
	return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

MongoCtx* mongo_init(const char* uri_str, const char* db_name) {
	mongoc_init();
	bson_error_t err;
	mongoc_uri_t* uri = mongoc_uri_new_with_error(uri_str, &err);
	if (!uri) { MONGO_LOG("bad uri: %s", err.message); return NULL; }
	mongoc_uri_set_option_as_int32(uri, MONGOC_URI_CONNECTTIMEOUTMS, 5000);
	mongoc_uri_set_option_as_int32(uri, MONGOC_URI_SERVERSELECTIONTIMEOUTMS, 5000);
	mongoc_uri_set_option_as_utf8(uri, MONGOC_URI_W, "majority");
	mongoc_uri_set_option_as_utf8(uri, MONGOC_URI_READCONCERNLEVEL, "majority");

	mongoc_client_pool_t* pool = mongoc_client_pool_new(uri);
	if (!pool) { mongoc_uri_destroy(uri); return NULL; }
	mongoc_client_pool_set_appname(pool, "manager");

	mongoc_write_concern_t* wc = mongoc_write_concern_new();
	mongoc_write_concern_set_wmajority(wc, 5000);
	mongoc_write_concern_set_journal(wc, true);

	MongoCtx* m = (MongoCtx*)calloc(1, sizeof(*m));
	m->uri = uri; m->pool = pool; m->db = strdup(db_name); m->wc_majority = wc;
	return m;
}

void mongo_destroy(MongoCtx* m) {
	if (!m) return;
	mongoc_write_concern_destroy(m->wc_majority);
	mongoc_client_pool_destroy(m->pool);
	mongoc_uri_destroy(m->uri);
	free(m->db);
	free(m);
	mongoc_cleanup();
}

static mongoc_collection_t* checkout_coll(MongoCtx* m, mongoc_client_t** out_cli, const char* coll) {
	*out_cli = mongoc_client_pool_pop(m->pool);
	return mongoc_client_get_collection(*out_cli, m->db, coll);
}

static void release_coll(MongoCtx* m, mongoc_client_t* cli, mongoc_collection_t* c) {
	mongoc_collection_destroy(c);
	mongoc_client_pool_push(m->pool, cli);
}

int mongo_insert_request(MongoCtx* m, const char* request_id, const char* hash,
                         uint64_t max_length, int64_t total_tasks) {
	mongoc_client_t* cli;
	mongoc_collection_t* c = checkout_coll(m, &cli, COLL_REQUESTS);
	bson_t* doc = BCON_NEW(
		"_id",         BCON_UTF8(request_id),
		"hash",        BCON_UTF8(hash),
		"maxLength",   BCON_INT64((int64_t)max_length),
		"status",      BCON_UTF8("IN_PROGRESS"),
		"results",     "[", "]",
		"total_tasks", BCON_INT64(total_tasks),
		"done_tasks",  BCON_INT64(0),
		"createdAt",   BCON_DATE_TIME(now_ms()),
		"updatedAt",   BCON_DATE_TIME(now_ms())
	);
	bson_t opts = BSON_INITIALIZER;
	mongoc_write_concern_append(m->wc_majority, &opts);
	bson_error_t err;
	bool ok = mongoc_collection_insert_one(c, doc, &opts, NULL, &err);
	if (!ok) MONGO_LOG("insert_request: %s", err.message);
	bson_destroy(doc); bson_destroy(&opts);
	release_coll(m, cli, c);
	return ok ? 0 : -1;
}

int mongo_set_request_status(MongoCtx* m, const char* request_id, const char* status) {
	mongoc_client_t* cli;
	mongoc_collection_t* c = checkout_coll(m, &cli, COLL_REQUESTS);
	bson_t* q = BCON_NEW("_id", BCON_UTF8(request_id));
	bson_t* u = BCON_NEW("$set", "{",
		"status",    BCON_UTF8(status),
		"updatedAt", BCON_DATE_TIME(now_ms()),
	"}");
	bson_t opts = BSON_INITIALIZER;
	mongoc_write_concern_append(m->wc_majority, &opts);
	bson_error_t err;
	bool ok = mongoc_collection_update_one(c, q, u, &opts, NULL, &err);
	if (!ok) MONGO_LOG("set_request_status: %s", err.message);
	bson_destroy(q); bson_destroy(u); bson_destroy(&opts);
	release_coll(m, cli, c);
	return ok ? 0 : -1;
}

int mongo_get_request_status(MongoCtx* m, const char* request_id, MongoRequestStatus* out) {
	memset(out, 0, sizeof(*out));
	mongoc_client_t* cli;
	mongoc_collection_t* c = checkout_coll(m, &cli, COLL_REQUESTS);
	bson_t* q = BCON_NEW("_id", BCON_UTF8(request_id));
	mongoc_cursor_t* cur = mongoc_collection_find_with_opts(c, q, NULL, NULL);
	const bson_t* doc = NULL;
	int rc = -1;
	if (mongoc_cursor_next(cur, &doc)) {
		bson_iter_t it;
		if (bson_iter_init_find(&it, doc, "status") && BSON_ITER_HOLDS_UTF8(&it)) {
			uint32_t l; const char* s = bson_iter_utf8(&it, &l);
			size_t copy = l < sizeof(out->status) - 1 ? l : sizeof(out->status) - 1;
			memcpy(out->status, s, copy); out->status[copy] = '\0';
		}
		if (bson_iter_init_find(&it, doc, "results") && BSON_ITER_HOLDS_ARRAY(&it)) {
			bson_iter_t arr;
			bson_iter_recurse(&it, &arr);
			size_t cap = 4;
			out->results = (char**)calloc(cap, sizeof(char*));
			while (bson_iter_next(&arr)) {
				if (!BSON_ITER_HOLDS_UTF8(&arr)) continue;
				if (out->n_results == cap) {
					cap *= 2;
					out->results = (char**)realloc(out->results, cap * sizeof(char*));
				}
				uint32_t l; const char* s = bson_iter_utf8(&arr, &l);
				out->results[out->n_results++] = strndup(s, l);
			}
		}
		rc = 0;
	}
	mongoc_cursor_destroy(cur);
	bson_destroy(q);
	release_coll(m, cli, c);
	return rc;
}

void mongo_request_status_free(MongoRequestStatus* s) {
	if (!s || !s->results) return;
	for (size_t i = 0; i < s->n_results; ++i) free(s->results[i]);
	free(s->results);
	s->results = NULL; s->n_results = 0;
}

int mongo_insert_task(MongoCtx* m, const char* task_id, const char* request_id,
                      uint64_t start_index, uint64_t count,
                      const char* target_hash, uint64_t max_length) {
	mongoc_client_t* cli;
	mongoc_collection_t* c = checkout_coll(m, &cli, COLL_TASKS);
	bson_t* doc = BCON_NEW(
		"_id",         BCON_UTF8(task_id),
		"request_id",  BCON_UTF8(request_id),
		"start_index", BCON_INT64((int64_t)start_index),
		"count",       BCON_INT64((int64_t)count),
		"target_hash", BCON_UTF8(target_hash),
		"max_length",  BCON_INT64((int64_t)max_length),
		"status",      BCON_UTF8("NEW"),
		"createdAt",   BCON_DATE_TIME(now_ms()),
		"updatedAt",   BCON_DATE_TIME(now_ms())
	);
	bson_t opts = BSON_INITIALIZER;
	mongoc_write_concern_append(m->wc_majority, &opts);
	bson_error_t err;
	bool ok = mongoc_collection_insert_one(c, doc, &opts, NULL, &err);
	if (!ok) MONGO_LOG("insert_task: %s", err.message);
	bson_destroy(doc); bson_destroy(&opts);
	release_coll(m, cli, c);
	return ok ? 0 : -1;
}

int mongo_set_task_status(MongoCtx* m, const char* task_id, const char* status) {
	mongoc_client_t* cli;
	mongoc_collection_t* c = checkout_coll(m, &cli, COLL_TASKS);
	bson_t* q = BCON_NEW("_id", BCON_UTF8(task_id));
	bson_t* u = BCON_NEW("$set", "{",
		"status",    BCON_UTF8(status),
		"updatedAt", BCON_DATE_TIME(now_ms()),
	"}");
	bson_t opts = BSON_INITIALIZER;
	mongoc_write_concern_append(m->wc_majority, &opts);
	bson_error_t err;
	bool ok = mongoc_collection_update_one(c, q, u, &opts, NULL, &err);
	if (!ok) MONGO_LOG("set_task_status %s: %s", status, err.message);
	bson_destroy(q); bson_destroy(u); bson_destroy(&opts);
	release_coll(m, cli, c);
	return ok ? 0 : -1;
}

int mongo_complete_task(MongoCtx* m, const char* task_id, const char* request_id,
                        const char* const* results, size_t n_results, int* out_was_new) {
	if (out_was_new) *out_was_new = 0;
	mongoc_client_t* cli = mongoc_client_pool_pop(m->pool);
	mongoc_collection_t* tasks = mongoc_client_get_collection(cli, m->db, COLL_TASKS);
	mongoc_collection_t* reqs  = mongoc_client_get_collection(cli, m->db, COLL_REQUESTS);

	bson_t* q = BCON_NEW(
		"_id",    BCON_UTF8(task_id),
		"status", "{", "$ne", BCON_UTF8("DONE"), "}"
	);
	bson_t* u = bson_new();
	bson_t set_block, arr_block;
	bson_append_document_begin(u, "$set", -1, &set_block);
		bson_append_utf8(&set_block, "status", -1, "DONE", -1);
		bson_append_date_time(&set_block, "updatedAt", -1, now_ms());
		bson_append_array_begin(&set_block, "results", -1, &arr_block);
		for (size_t i = 0; i < n_results; ++i) {
			char idx[16]; snprintf(idx, sizeof(idx), "%zu", i);
			bson_append_utf8(&arr_block, idx, -1, results[i], -1);
		}
		bson_append_array_end(&set_block, &arr_block);
	bson_append_document_end(u, &set_block);

	bson_t opts = BSON_INITIALIZER;
	mongoc_write_concern_append(m->wc_majority, &opts);
	bson_t reply = BSON_INITIALIZER;
	bson_error_t err;
	bool ok = mongoc_collection_update_one(tasks, q, u, &opts, &reply, &err);
	int matched = 0;
	if (ok) {
		bson_iter_t it;
		if (bson_iter_init_find(&it, &reply, "matchedCount")) matched = (int)bson_iter_as_int64(&it);
	} else {
		MONGO_LOG("complete_task update tasks: %s", err.message);
	}
	bson_destroy(&reply);
	bson_destroy(q); bson_destroy(u); bson_destroy(&opts);

	if (!ok) {
		mongoc_collection_destroy(tasks); mongoc_collection_destroy(reqs);
		mongoc_client_pool_push(m->pool, cli);
		return -1;
	}
	if (matched == 0) {
		mongoc_collection_destroy(tasks); mongoc_collection_destroy(reqs);
		mongoc_client_pool_push(m->pool, cli);
		return 0;
	}
	if (out_was_new) *out_was_new = 1;

	bson_t* rq = BCON_NEW("_id", BCON_UTF8(request_id));
	bson_t* ru = bson_new();
	bson_t inc_block, push_block, rset_block;
	bson_append_document_begin(ru, "$inc", -1, &inc_block);
		bson_append_int64(&inc_block, "done_tasks", -1, 1);
	bson_append_document_end(ru, &inc_block);
	bson_append_document_begin(ru, "$set", -1, &rset_block);
		bson_append_date_time(&rset_block, "updatedAt", -1, now_ms());
	bson_append_document_end(ru, &rset_block);
	if (n_results > 0) {
		bson_append_document_begin(ru, "$push", -1, &push_block);
			bson_t each_doc; bson_append_document_begin(&push_block, "results", -1, &each_doc);
				bson_t each_arr; bson_append_array_begin(&each_doc, "$each", -1, &each_arr);
				for (size_t i = 0; i < n_results; ++i) {
					char idx[16]; snprintf(idx, sizeof(idx), "%zu", i);
					bson_append_utf8(&each_arr, idx, -1, results[i], -1);
				}
				bson_append_array_end(&each_doc, &each_arr);
			bson_append_document_end(&push_block, &each_doc);
		bson_append_document_end(ru, &push_block);
	}
	bson_t ropts = BSON_INITIALIZER;
	mongoc_write_concern_append(m->wc_majority, &ropts);
	ok = mongoc_collection_update_one(reqs, rq, ru, &ropts, NULL, &err);
	if (!ok) MONGO_LOG("complete_task update request: %s", err.message);
	bson_destroy(rq); bson_destroy(ru); bson_destroy(&ropts);

	if (ok) {
		bson_t* q2 = BCON_NEW("_id", BCON_UTF8(request_id),
		                     "$expr", "{",
		                       "$eq", "[",
		                         BCON_UTF8("$done_tasks"), BCON_UTF8("$total_tasks"),
		                       "]",
		                     "}");
		bson_t* u2 = BCON_NEW("$set", "{",
			"status",    BCON_UTF8("READY"),
			"updatedAt", BCON_DATE_TIME(now_ms()),
		"}");
		bson_t opts2 = BSON_INITIALIZER;
		mongoc_write_concern_append(m->wc_majority, &opts2);
		mongoc_collection_update_one(reqs, q2, u2, &opts2, NULL, &err);
		bson_destroy(q2); bson_destroy(u2); bson_destroy(&opts2);
	}

	mongoc_collection_destroy(tasks); mongoc_collection_destroy(reqs);
	mongoc_client_pool_push(m->pool, cli);
	return ok ? 0 : -1;
}

int mongo_list_tasks_by_status(MongoCtx* m, const char* statuses, MongoTaskList* out) {
	memset(out, 0, sizeof(*out));
	mongoc_client_t* cli;
	mongoc_collection_t* c = checkout_coll(m, &cli, COLL_TASKS);

	bson_t* q = bson_new();
	bson_t status_doc, in_arr;
	bson_append_document_begin(q, "status", -1, &status_doc);
		bson_append_array_begin(&status_doc, "$in", -1, &in_arr);
		int idx = 0;
		const char* p = statuses; const char* start = p;
		while (1) {
			if (*p == ',' || *p == '\0') {
				if (p > start) {
					char k[8]; snprintf(k, sizeof(k), "%d", idx++);
					bson_append_utf8(&in_arr, k, -1, start, (int)(p - start));
				}
				if (*p == '\0') break;
				start = p + 1;
			}
			++p;
		}
		bson_append_array_end(&status_doc, &in_arr);
	bson_append_document_end(q, &status_doc);

	mongoc_cursor_t* cur = mongoc_collection_find_with_opts(c, q, NULL, NULL);
	size_t cap = 16;
	out->items = (MongoTaskRow*)calloc(cap, sizeof(MongoTaskRow));
	const bson_t* doc;
	while (mongoc_cursor_next(cur, &doc)) {
		if (out->count == cap) {
			cap *= 2;
			out->items = (MongoTaskRow*)realloc(out->items, cap * sizeof(MongoTaskRow));
			memset(out->items + out->count, 0, (cap - out->count) * sizeof(MongoTaskRow));
		}
		MongoTaskRow* r = &out->items[out->count];
		bson_iter_t it;
		uint32_t l;
		if (bson_iter_init_find(&it, doc, "_id") && BSON_ITER_HOLDS_UTF8(&it)) {
			const char* s = bson_iter_utf8(&it, &l);
			snprintf(r->task_id, sizeof(r->task_id), "%.*s", (int)l, s);
		}
		if (bson_iter_init_find(&it, doc, "request_id") && BSON_ITER_HOLDS_UTF8(&it)) {
			const char* s = bson_iter_utf8(&it, &l);
			snprintf(r->request_id, sizeof(r->request_id), "%.*s", (int)l, s);
		}
		if (bson_iter_init_find(&it, doc, "target_hash") && BSON_ITER_HOLDS_UTF8(&it)) {
			const char* s = bson_iter_utf8(&it, &l);
			snprintf(r->target_hash, sizeof(r->target_hash), "%.*s", (int)l, s);
		}
		if (bson_iter_init_find(&it, doc, "start_index"))
			r->start_index = (uint64_t)bson_iter_as_int64(&it);
		if (bson_iter_init_find(&it, doc, "count"))
			r->count = (uint64_t)bson_iter_as_int64(&it);
		if (bson_iter_init_find(&it, doc, "max_length"))
			r->max_length = (uint64_t)bson_iter_as_int64(&it);
		if (bson_iter_init_find(&it, doc, "status") && BSON_ITER_HOLDS_UTF8(&it)) {
			const char* s = bson_iter_utf8(&it, &l);
			snprintf(r->status, sizeof(r->status), "%.*s", (int)l, s);
		}
		out->count++;
	}
	mongoc_cursor_destroy(cur);
	bson_destroy(q);
	release_coll(m, cli, c);
	return 0;
}

void mongo_task_list_free(MongoTaskList* lst) {
	if (!lst) return;
	free(lst->items);
	lst->items = NULL; lst->count = 0;
}

int mongo_timeout_requests(MongoCtx* m, int64_t timeout_ms) {
	int64_t cutoff = now_ms() - timeout_ms;
	mongoc_client_t* cli;
	mongoc_collection_t* c = checkout_coll(m, &cli, COLL_REQUESTS);
	bson_t* q = BCON_NEW(
		"status",    BCON_UTF8("IN_PROGRESS"),
		"createdAt", "{", "$lt", BCON_DATE_TIME(cutoff), "}"
	);
	bson_t* u = BCON_NEW("$set", "{",
		"status",    BCON_UTF8("ERROR"),
		"updatedAt", BCON_DATE_TIME(now_ms()),
	"}");
	bson_t opts = BSON_INITIALIZER;
	mongoc_write_concern_append(m->wc_majority, &opts);
	bson_t reply = BSON_INITIALIZER;
	bson_error_t err;
	bool ok = mongoc_collection_update_many(c, q, u, &opts, &reply, &err);
	int count = 0;
	if (ok) {
		bson_iter_t it;
		if (bson_iter_init_find(&it, &reply, "modifiedCount"))
			count = (int)bson_iter_as_int64(&it);
	} else {
		MONGO_LOG("timeout_requests: %s", err.message);
	}
	bson_destroy(&reply);
	bson_destroy(q); bson_destroy(u); bson_destroy(&opts);
	release_coll(m, cli, c);
	return ok ? count : -1;
}

#endif // MONGO_IMPL
#endif // MONGO_H
