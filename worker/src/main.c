#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <signal.h>

#include <omp.h>

#define NOB_IMPLEMENTATION
#define MD5_IMPLEMENTATION
#include "../thirdparty/nob.h"
#include "../thirdparty/md5.h"

#define INSTPOW_IMPL
#define DTO_IMPL
#define DTO_SERIALIZE_IMPL
#define AMQP_IO_IMPL
#define WORKER_METRICS_IMPL
#include "../include/instpow.h"
#include "../include/dto.h"
#include "../include/dto_serialize.h"
#include "../include/amqp_io.h"
#include "../include/metrics.h"

#define NOB_STRIP_PREFIX

const char ALPHABET[] = "abcdefghijklmnopqrstuvwxyz0123456789";
const int  BASE       = 36;

static volatile sig_atomic_t g_stop = 0;
static void on_signal(int s) { (void)s; g_stop = 1; }

static const char* env_or(const char* k, const char* def) {
	const char* v = getenv(k);
	return (v && *v) ? v : def;
}
static int env_int(const char* k, int def) {
	const char* v = getenv(k);
	return (v && *v) ? atoi(v) : def;
}

uint64_t instpowPartSum(const int to) {
	uint64_t sum = 0;
	for (int i = 0; i <= to; ++i) {
		sum += instantPow(i);
	}
	return sum;
}

int getCombinationByIndex(char* const buf, const uint32_t bufLen, uint64_t index) {
	if (instantPow(bufLen - 2) == (uint64_t) -1 ) {
		return -1;
	}
	index -= (instpowPartSum(bufLen - 2) - 1);
	for (int i = bufLen - 2; i >= 0; i--) {
		buf[i] = ALPHABET[index % BASE];
		index /= BASE;
	}
	buf[bufLen - 1] = '\0';
	return 0;
}

uint32_t getSizeForIndex(const uint64_t index) {
	int maxPow = getMaxPow();
	for (int i = maxPow - 1; i > 0; --i) {
		if (index + 1 >= instpowPartSum(i)) {
			return i + 1;
		}
	}
	return 1;
}

void bytesToHex(const uint8_t* const bytes, const size_t bytesSize, char* const hex, const size_t hexSize) {
	for (size_t i = 0; i < bytesSize; ++i) sprintf(hex + i * 2, "%02x", bytes[i]);
	hex[hexSize - 1] = '\0';
}

static void process_task(const Task* t, char** found_out) {
	*found_out = NULL;
	char* found = NULL;
	#pragma omp parallel for shared(found)
	for (uint64_t i = t->startIndex; i < t->startIndex + t->count; ++i) {
		if (found) continue;
		const uint32_t size = getSizeForIndex(i) + 1;
		char comb[size];
		memset(comb, 0, size);
		getCombinationByIndex(comb, size, i);

		uint8_t hash[MD5_HASH_SIZE];
		md5String(comb, hash);

		char hex[33];
		bytesToHex(hash, MD5_HASH_SIZE, hex, MD5_HASH_SIZE * 2 + 1);

		if (strcmp(hex, t->targetHash) == 0) {
			#pragma omp critical
			{
				if (!found) {
					found = strdup(comb);
				}
			}
		}
	}
	*found_out = found;
}

static char* build_result_json(const Task* t, const char* found) {
	Result r = {0};
	r.taskId = t->taskId;
	r.requestId = t->requestId;
	r.status = "DONE";
	if (found) {
		char** arr = (char**)temp_alloc(sizeof(char*) * 2);
		arr[0] = (char*)found;
		arr[1] = NULL;
		r.results = arr;
	} else {
		char** arr = (char**)temp_alloc(sizeof(char*) * 1);
		arr[0] = NULL;
		r.results = arr;
	}
	return serializeResult(&r);
}

static int handle_one_message(AmqpConn* conn, const char* body, size_t body_len, uint64_t tag) {
	Task t = {0};
	if (parseTask(&t, body, body_len) != 0) {
		nob_log(ERROR, "parseTask failed; body=%.*s", (int)body_len, body);
		amqp_io_nack(conn, tag, 0);
		wmetrics_inc_error();
		return 0;
	}
	nob_log(INFO, "task %s req %s start=%lu count=%lu hash=%s",
		t.taskId, t.requestId, t.startIndex, t.count, t.targetHash);

	wmetrics_set_active(1);
	char* found = NULL;
	process_task(&t, &found);
	wmetrics_set_active(0);

	if (found) { nob_log(INFO, "  FOUND: %s", found); wmetrics_inc_found(); }
	else       { nob_log(INFO, "  not found in this range"); wmetrics_inc_done(); }

	char* json = build_result_json(&t, found);
	int published = amqp_io_publish_result(conn, json, strlen(json));
	if (found) free(found);

	if (published != 0) {
		nob_log(WARNING, "publish failed; nack+requeue");
		amqp_io_nack(conn, tag, 1);
		return -1;
	}
	if (amqp_io_ack(conn, tag) != 0) {
		nob_log(WARNING, "ack failed");
		return -1;
	}
	return 0;
}

int main(void) {
	signal(SIGINT,  on_signal);
	signal(SIGTERM, on_signal);

	#pragma omp parallel
	{
		#pragma omp single
		nob_log(NOB_INFO, "worker starting; %d OpenMP threads", omp_get_num_threads());
	}

	wmetrics_init(env_int("METRICS_PORT", 9090));

	AmqpCfg cfg = {
		.host         = env_or("RABBITMQ_HOST", "rabbitmq"),
		.port         = env_int("RABBITMQ_PORT", 5672),
		.user         = env_or("RABBITMQ_USER", "guest"),
		.pass         = env_or("RABBITMQ_PASS", "guest"),
		.vhost        = env_or("RABBITMQ_VHOST", "/"),
		.task_queue   = env_or("TASK_QUEUE",   "task.queue"),
		.result_queue = env_or("RESULT_QUEUE", "result.queue"),
		.exchange     = "",
		.task_rk      = env_or("TASK_QUEUE",   "task.queue"),
		.result_rk    = env_or("RESULT_QUEUE", "result.queue"),
		.prefetch     = (uint16_t)env_int("PREFETCH", 1),
		.heartbeat_sec = env_int("HEARTBEAT", 30),
	};

	while (!g_stop) {
		AmqpConn* conn = amqp_io_connect(&cfg);
		if (!conn) {
			nob_log(WARNING, "amqp connect failed; retry in 3s");
			sleep(3);
			continue;
		}
		nob_log(INFO, "amqp connected to %s:%d", cfg.host, cfg.port);

		int conn_alive = 1;
		while (!g_stop && conn_alive) {
			char* body = NULL; size_t blen = 0; uint64_t tag = 0;
			if (amqp_io_consume_one(conn, &body, &blen, &tag) != 0) {
				conn_alive = 0;
				break;
			}
			int r = handle_one_message(conn, body, blen, tag);
			amqp_io_free_body(body);
			temp_reset();
			if (r != 0) {
				conn_alive = 0;
				break;
			}
		}
		amqp_io_close(conn);
		if (g_stop) break;
		nob_log(WARNING, "connection lost; reconnecting in 2s");
		wmetrics_inc_reconnect();
		wmetrics_set_active(0);
		sleep(2);
	}

	nob_log(INFO, "worker stopped");
	return 0;
}
