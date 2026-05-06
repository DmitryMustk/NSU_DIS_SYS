#ifndef SCHED_H
#define SCHED_H

#include <stdint.h>
#include <stddef.h>
#include "mongo.h"
#include "amqp_io.h"


uint64_t sched_total_combinations(uint64_t max_length);
int64_t sched_dispatch_request(MongoCtx* mongo, AmqpPub* pub,
                               const char* request_id, const char* target_hash,
                               uint64_t max_length, uint64_t chunk_size);

int sched_retry(MongoCtx* mongo, AmqpPub* pub, const char* statuses);
int sched_retry_queued(MongoCtx* mongo, AmqpPub* pub);


int sched_publish_one(AmqpPub* pub,
                      const char* task_id, const char* request_id,
                      uint64_t start_index, uint64_t count,
                      const char* target_hash, uint64_t max_length);

#ifdef SCHED_IMPL

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "instpow.h"
#include "uuid.h"
#include "jsonu.h"
#include "metrics.h"

uint64_t sched_total_combinations(uint64_t max_length) {
	int maxp = getMaxPow();
	if ((int)max_length > maxp) return 0;
	uint64_t sum = 0;
	for (uint64_t i = 1; i <= max_length; ++i) {
		uint64_t p = instantPow((int)i);
		if (p == (uint64_t)-1) return 0;
		if (sum + p < sum) return 0; // overflow
		sum += p;
	}
	return sum;
}

int sched_publish_one(AmqpPub* pub,
                      const char* task_id, const char* request_id,
                      uint64_t start_index, uint64_t count,
                      const char* target_hash, uint64_t max_length) {
	JBuf b; jbuf_init(&b);
	jbuf_putc(&b, '{');
	jbuf_kv_str(&b, "taskId", task_id);            jbuf_putc(&b, ',');
	jbuf_kv_str(&b, "requestId", request_id);      jbuf_putc(&b, ',');
	jbuf_kv_u  (&b, "startIndex", start_index);    jbuf_putc(&b, ',');
	jbuf_kv_u  (&b, "count", count);               jbuf_putc(&b, ',');
	jbuf_kv_str(&b, "targetHash", target_hash);    jbuf_putc(&b, ',');
	jbuf_kv_u  (&b, "maxLength", max_length);
	jbuf_putc(&b, '}');
	int rc = amqp_pub_publish_task(pub, b.data, b.len);
	jbuf_free(&b);
	return rc;
}

int64_t sched_dispatch_request(MongoCtx* mongo, AmqpPub* pub,
                               const char* request_id, const char* target_hash,
                               uint64_t max_length, uint64_t chunk_size) {
	uint64_t total = sched_total_combinations(max_length);
	if (total == 0) return -1;
	if (chunk_size == 0) chunk_size = 100000;

	int64_t made = 0;
	for (uint64_t off = 0; off < total; off += chunk_size) {
		uint64_t cnt = (off + chunk_size > total) ? (total - off) : chunk_size;
		char tid[37]; uuidv4(tid);

		if (mongo_insert_task(mongo, tid, request_id, off, cnt, target_hash, max_length) != 0) {
			return -1;
		}
		metrics_inc_tasks_created(1);

		if (sched_publish_one(pub, tid, request_id, off, cnt, target_hash, max_length) == 0) {
			mongo_set_task_status(mongo, tid, "SENT");
			metrics_inc_tasks_published_ok();
		} else {
			mongo_set_task_status(mongo, tid, "QUEUED");
			metrics_inc_tasks_published_failed();
		}
		made++;
	}
	return made;
}

int sched_retry(MongoCtx* mongo, AmqpPub* pub, const char* statuses) {
	MongoTaskList lst = {0};
	if (mongo_list_tasks_by_status(mongo, statuses, &lst) != 0) return -1;
	int sent = 0;
	for (size_t i = 0; i < lst.count; ++i) {
		MongoTaskRow* r = &lst.items[i];
		if (sched_publish_one(pub, r->task_id, r->request_id,
		                      r->start_index, r->count, r->target_hash, r->max_length) == 0) {
			mongo_set_task_status(mongo, r->task_id, "SENT");
			metrics_inc_tasks_published_ok();
			sent++;
		} else {
			mongo_set_task_status(mongo, r->task_id, "QUEUED");
			metrics_inc_tasks_published_failed();
		}
	}
	mongo_task_list_free(&lst);
	return sent;
}

int sched_retry_queued(MongoCtx* mongo, AmqpPub* pub) {
	return sched_retry(mongo, pub, "NEW,QUEUED");
}

#endif // SCHED_IMPL
#endif // SCHED_H
