#ifndef WORKER_METRICS_H
#define WORKER_METRICS_H

#include <stdint.h>

void wmetrics_init(int port);

void wmetrics_inc_done(void);      // task completed, hash not found
void wmetrics_inc_found(void);     // task completed, hash found
void wmetrics_inc_error(void);     // task returned ERROR or failed
void wmetrics_inc_reconnect(void); // AMQP reconnect event
void wmetrics_set_active(int v);   // 1 = processing, 0 = idle

#ifdef WORKER_METRICS_IMPL

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <stdatomic.h>

static atomic_ullong wm_done      = 0;
static atomic_ullong wm_found     = 0;
static atomic_ullong wm_error     = 0;
static atomic_ullong wm_reconnect = 0;
static atomic_int    wm_active    = 0;
static int           wm_port_g    = 0;

void wmetrics_inc_done(void)      { atomic_fetch_add(&wm_done, 1); }
void wmetrics_inc_found(void)     { atomic_fetch_add(&wm_found, 1); }
void wmetrics_inc_error(void)     { atomic_fetch_add(&wm_error, 1); }
void wmetrics_inc_reconnect(void) { atomic_fetch_add(&wm_reconnect, 1); }
void wmetrics_set_active(int v)   { atomic_store(&wm_active, v); }

static void* wmetrics_server(void* arg) {
	(void)arg;
	int sfd = socket(AF_INET, SOCK_STREAM, 0);
	if (sfd < 0) { perror("[metrics] socket"); return NULL; }
	int opt = 1;
	setsockopt(sfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
	struct sockaddr_in addr;
	memset(&addr, 0, sizeof(addr));
	addr.sin_family      = AF_INET;
	addr.sin_addr.s_addr = INADDR_ANY;
	addr.sin_port        = htons((uint16_t)wm_port_g);
	if (bind(sfd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
		perror("[metrics] bind"); close(sfd); return NULL;
	}
	listen(sfd, 8);
	fprintf(stderr, "[metrics] listening on :%d\n", wm_port_g);
	for (;;) {
		int cfd = accept(sfd, NULL, NULL);
		if (cfd < 0) continue;
		char buf[256];
		recv(cfd, buf, sizeof(buf) - 1, 0);

		char body[1024];
		int blen = snprintf(body, sizeof(body),
			"# HELP worker_tasks_total Tasks completed by this worker instance\n"
			"# TYPE worker_tasks_total counter\n"
			"worker_tasks_total{result=\"done\"} %llu\n"
			"worker_tasks_total{result=\"found\"} %llu\n"
			"worker_tasks_total{result=\"error\"} %llu\n"
			"# HELP worker_amqp_reconnects_total AMQP reconnect events\n"
			"# TYPE worker_amqp_reconnects_total counter\n"
			"worker_amqp_reconnects_total %llu\n"
			"# HELP worker_task_active 1 if currently processing a task, 0 if idle\n"
			"# TYPE worker_task_active gauge\n"
			"worker_task_active %d\n",
			(unsigned long long)atomic_load(&wm_done),
			(unsigned long long)atomic_load(&wm_found),
			(unsigned long long)atomic_load(&wm_error),
			(unsigned long long)atomic_load(&wm_reconnect),
			atomic_load(&wm_active));

		char resp[1200];
		int rlen = snprintf(resp, sizeof(resp),
			"HTTP/1.1 200 OK\r\n"
			"Content-Type: text/plain; version=0.0.4\r\n"
			"Content-Length: %d\r\n"
			"Connection: close\r\n\r\n%s",
			blen, body);
		send(cfd, resp, (size_t)rlen, 0);
		close(cfd);
	}
	return NULL;
}

void wmetrics_init(int port) {
	if (port <= 0) return;
	wm_port_g = port;
	pthread_t tid;
	pthread_attr_t attr;
	pthread_attr_init(&attr);
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
	pthread_create(&tid, &attr, wmetrics_server, NULL);
	pthread_attr_destroy(&attr);
}

#endif // WORKER_METRICS_IMPL
#endif // WORKER_METRICS_H
