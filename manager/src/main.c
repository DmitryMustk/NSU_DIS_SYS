#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <pthread.h>

#define NOB_IMPLEMENTATION
#include "../thirdparty/nob.h"

#define INSTPOW_IMPL
#define UUID_IMPL
#define JSONU_IMPL
#define HTTP_IMPL
#define MONGO_IMPL
#define MGR_AMQP_IO_IMPL
#define METRICS_IMPL
#define SCHED_IMPL
#define RESULT_CONSUMER_IMPL
#define API_IMPL
#include "../include/instpow.h"
#include "../include/uuid.h"
#include "../include/jsonu.h"
#include "../include/http.h"
#include "../include/mongo.h"
#include "../include/amqp_io.h"
#include "../include/metrics.h"
#include "../include/scheduler.h"
#include "../include/result_consumer.h"
#include "../include/api.h"

static volatile int g_stop = 0;
static void on_signal(int s) { (void)s; g_stop = 1; }

static const char* env_or(const char* k, const char* def) {
	const char* v = getenv(k); return (v && *v) ? v : def;
}
static int env_int(const char* k, int def) {
	const char* v = getenv(k); return (v && *v) ? atoi(v) : def;
}

typedef struct {
	MongoCtx*         mongo;
	AmqpSub*          sub;
	volatile int*     stop;
} ConsumerArgs;

static void* consumer_thread(void* arg) {
	ConsumerArgs* a = (ConsumerArgs*)arg;
	result_consumer_run(a->sub, a->mongo, a->stop, /*drain*/ 0, 0);
	return NULL;
}

typedef struct {
	MongoCtx*         mongo;
	AmqpPub*          pub;
	volatile int*     stop;
	int               interval_sec;
} RetryArgs;

typedef struct {
	MongoCtx*     mongo;
	volatile int* stop;
	int64_t       timeout_ms;
	int           interval_sec;
} TimeoutArgs;

static void* timeout_thread(void* arg) {
	TimeoutArgs* t = (TimeoutArgs*)arg;
	for (int i = 0; i < 60 && !*t->stop; ++i) usleep(100 * 1000); // initial delay 6s
	while (!*t->stop) {
		int n = mongo_timeout_requests(t->mongo, t->timeout_ms);
		if (n > 0) fprintf(stderr, "[timeout] expired %d request(s)\n", n);
		for (int i = 0; i < t->interval_sec * 10 && !*t->stop; ++i) usleep(100 * 1000);
	}
	return NULL;
}

static void* retry_thread(void* arg) {
	RetryArgs* r = (RetryArgs*)arg;
	while (!*r->stop) {
		int sent = sched_retry_queued(r->mongo, r->pub);
		if (sent > 0) fprintf(stderr, "[retry] republished %d queued tasks\n", sent);
		for (int i = 0; i < r->interval_sec * 10 && !*r->stop; ++i) usleep(100 * 1000);
	}
	return NULL;
}

int main(void) {
	signal(SIGINT,  on_signal);
	signal(SIGTERM, on_signal);
	signal(SIGPIPE, SIG_IGN);

	const char* mongo_uri = env_or("MONGO_URI",
		"mongodb://mongodb-0.mongodb:27017,mongodb-1.mongodb:27017,mongodb-2.mongodb:27017/?replicaSet=rs0");
	const char* mongo_db  = env_or("MONGO_DB", "crack");
	int         http_port = env_int("HTTP_PORT", 8080);
	uint64_t    chunk       = (uint64_t)env_int("CHUNK_SIZE", 100000);
	int64_t     timeout_ms  = (int64_t)env_int("REQUEST_TIMEOUT_SEC", 300) * 1000;

	AmqpCfg amqp = {
		.host          = env_or("RABBITMQ_HOST", "rabbitmq"),
		.port          = env_int("RABBITMQ_PORT", 5672),
		.user          = env_or("RABBITMQ_USER", "guest"),
		.pass          = env_or("RABBITMQ_PASS", "guest"),
		.vhost         = env_or("RABBITMQ_VHOST", "/"),
		.task_queue    = env_or("TASK_QUEUE", "task.queue"),
		.result_queue  = env_or("RESULT_QUEUE", "result.queue"),
		.heartbeat_sec = env_int("HEARTBEAT", 30),
	};

	fprintf(stderr, "[manager] mongo=%s db=%s http=:%d amqp=%s:%d\n",
	        mongo_uri, mongo_db, http_port, amqp.host, amqp.port);

	metrics_init();

	MongoCtx* mongo = NULL;
	while (!g_stop && (mongo = mongo_init(mongo_uri, mongo_db)) == NULL) {
		fprintf(stderr, "[manager] mongo init failed; retry in 3s\n"); sleep(3);
	}
	if (g_stop) return 0;

	AmqpPub* pub = amqp_pub_new(&amqp);
	AmqpSub* sub = amqp_sub_new(&amqp);

	
	fprintf(stderr, "[manager] recovery: draining result.queue\n");
	int drained = result_consumer_run(sub, mongo, &g_stop, /*drain*/ 1, /*idle_ms*/ 1500);
	fprintf(stderr, "[manager] recovery: drained %d result(s)\n", drained);
	int republished = sched_retry_queued(mongo, pub);
	fprintf(stderr, "[manager] recovery: republished %d task(s)\n", republished);

	// Background threads
	pthread_t cons_tid, retry_tid, timeout_tid;
	ConsumerArgs cargs = { mongo, sub, &g_stop };
	pthread_create(&cons_tid, NULL, consumer_thread, &cargs);
	RetryArgs rargs = { mongo, pub, &g_stop, env_int("RETRY_INTERVAL_SEC", 10) };
	pthread_create(&retry_tid, NULL, retry_thread, &rargs);
	TimeoutArgs targs = { mongo, &g_stop, timeout_ms, 30 };
	pthread_create(&timeout_tid, NULL, timeout_thread, &targs);

	// HTTP server (foreground)
	ApiCtx api = { .mongo = mongo, .pub = pub, .chunk_size = chunk };
	HttpRoute routes[] = {
		{ "POST", "/api/hash/crack",  api_crack   },
		{ "GET",  "/api/hash/status", api_status  },
		{ "GET",  "/metrics",         api_metrics },
		{ "GET",  "/healthz",         api_health  },
	};
	HttpServer* srv = http_start(http_port, routes, sizeof(routes)/sizeof(routes[0]), &api);
	if (!srv) { fprintf(stderr, "[manager] http_start failed\n"); g_stop = 1; }
	else      { http_run(srv, &g_stop); http_stop(srv); }

	fprintf(stderr, "[manager] shutting down\n");
	pthread_join(cons_tid, NULL);
	pthread_join(retry_tid, NULL);
	pthread_join(timeout_tid, NULL);
	amqp_sub_destroy(sub);
	amqp_pub_destroy(pub);
	mongo_destroy(mongo);
	return 0;
}
