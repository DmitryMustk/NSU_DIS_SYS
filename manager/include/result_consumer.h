#ifndef RESULT_CONSUMER_H
#define RESULT_CONSUMER_H

#include <stdint.h>
#include "amqp_io.h"
#include "mongo.h"

int result_consumer_run(AmqpSub* sub, MongoCtx* mongo,
                        volatile int* stop_flag, int drain, int idle_timeout_ms);

#ifdef RESULT_CONSUMER_IMPL

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../thirdparty/jsmn.h"
#include "metrics.h"

static int rc_parse_results(const char* json, size_t jlen, int arr_idx_token,
                            jsmntok_t* tok, int ntok,
                            char*** out_arr, size_t* out_n) {
	if (arr_idx_token < 0 || arr_idx_token >= ntok) return -1;
	if (tok[arr_idx_token].type != JSMN_ARRAY) return -1;
	int n = tok[arr_idx_token].size;
	char** arr = (char**)calloc((size_t)(n > 0 ? n : 1), sizeof(char*));
	for (int i = 0; i < n; ++i) {
		jsmntok_t* t = &tok[arr_idx_token + 1 + i];
		size_t l = (size_t)(t->end - t->start);
		arr[i] = (char*)malloc(l + 1);
		memcpy(arr[i], json + t->start, l);
		arr[i][l] = '\0';
	}
	*out_arr = arr; *out_n = (size_t)n;
	return 0;
}

static int rc_handle_one(MongoCtx* mongo, const char* body, size_t blen) {
	jsmn_parser p; jsmntok_t tok[64];
	jsmn_init(&p);
	int n = jsmn_parse(&p, body, blen, tok, 64);
	if (n < 1 || tok[0].type != JSMN_OBJECT) {
		fprintf(stderr, "[result] invalid JSON: %.*s\n", (int)blen, body);
		return -1;
	}
	char taskId[64] = {0}, requestId[64] = {0}, status[16] = {0};
	int results_idx = -1;
	for (int i = 1; i < n;) {
		jsmntok_t* k = &tok[i];
		if (k->type != JSMN_STRING) { i++; continue; }
		size_t klen = (size_t)(k->end - k->start);
		const char* kp = body + k->start;
		jsmntok_t* v = &tok[i + 1];
		size_t vlen = (size_t)(v->end - v->start);
		const char* vp = body + v->start;

		if (klen == 6 && strncmp(kp, "taskId", 6) == 0 && v->type == JSMN_STRING) {
			size_t cp = vlen < sizeof(taskId) - 1 ? vlen : sizeof(taskId) - 1;
			memcpy(taskId, vp, cp); taskId[cp] = 0;
			i += 2;
		} else if (klen == 9 && strncmp(kp, "requestId", 9) == 0 && v->type == JSMN_STRING) {
			size_t cp = vlen < sizeof(requestId) - 1 ? vlen : sizeof(requestId) - 1;
			memcpy(requestId, vp, cp); requestId[cp] = 0;
			i += 2;
		} else if (klen == 6 && strncmp(kp, "status", 6) == 0 && v->type == JSMN_STRING) {
			size_t cp = vlen < sizeof(status) - 1 ? vlen : sizeof(status) - 1;
			memcpy(status, vp, cp); status[cp] = 0;
			i += 2;
		} else if (klen == 7 && strncmp(kp, "results", 7) == 0 && v->type == JSMN_ARRAY) {
			results_idx = i + 1;
			int sz = v->size;
			i += 2 + sz;
		} else {
			// skip
			i += 2;
			if (v->type == JSMN_OBJECT || v->type == JSMN_ARRAY) i += v->size;
		}
	}

	if (!taskId[0] || !requestId[0]) {
		fprintf(stderr, "[result] missing taskId/requestId\n");
		return -1;
	}

	char** results = NULL; size_t nres = 0;
	if (results_idx >= 0) {
		rc_parse_results(body, blen, results_idx, tok, n, &results, &nres);
	}

	if (strcmp(status, "ERROR") == 0) {
		int was_new = 0;
		mongo_complete_task(mongo, taskId, requestId, NULL, 0, &was_new);
		metrics_inc_tasks_completed_error();
	} else {
		int was_new = 0;
		int rc = mongo_complete_task(mongo, taskId, requestId,
		                             (const char* const*)results, nres, &was_new);
		if (rc == 0) {
			if (was_new) metrics_inc_tasks_completed_ok();
			else         metrics_inc_tasks_completed_dup();
		} else {
			metrics_inc_tasks_completed_error();
		}
	}

	for (size_t i = 0; i < nres; ++i) free(results[i]);
	free(results);
	return 0;
}

int result_consumer_run(AmqpSub* sub, MongoCtx* mongo,
                        volatile int* stop_flag, int drain, int idle_timeout_ms) {
	int handled = 0;
	int idle_ms = 0;
	while (1) {
		if (stop_flag && *stop_flag) break;
		char* body = NULL; size_t blen = 0; uint64_t tag = 0;
		int r = amqp_sub_consume_one(sub, &body, &blen, &tag, 1000);
		if (r < 0) {
			if (drain) break;
			fprintf(stderr, "[result] connection error; reconnecting in 2s\n");
			sleep(2);
			continue;
		}
		if (r == 1) {
			if (drain) {
				idle_ms += 1000;
				if (idle_ms >= idle_timeout_ms) break;
			}
			continue;
		}
		idle_ms = 0;
		if (rc_handle_one(mongo, body, blen) == 0) {
			amqp_sub_ack(sub, tag);
		} else {
			amqp_sub_nack(sub, tag, 0); // bad message, drop
		}
		amqp_sub_free_body(body);
		handled++;
	}
	return handled;
}

#endif // RESULT_CONSUMER_IMPL
#endif // RESULT_CONSUMER_H
