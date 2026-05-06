#ifndef METRICS_H
#define METRICS_H

#include <stdint.h>
#include <stddef.h>

void metrics_init(void);

void metrics_inc_http_request(const char* endpoint, int status);

void metrics_inc_tasks_created(uint64_t n);
void metrics_inc_tasks_published_ok(void);
void metrics_inc_tasks_published_failed(void);
void metrics_inc_tasks_completed_ok(void);
void metrics_inc_tasks_completed_dup(void);
void metrics_inc_tasks_completed_error(void);

void metrics_inc_requests_created(void);
void metrics_inc_requests_ready(void);

char* metrics_render(size_t* len_out);

#ifdef METRICS_IMPL

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdatomic.h>

typedef struct {
	const char* endpoint;
	int         status;
	atomic_ullong count;
} HttpCell;

#define MAX_HTTP_CELLS 32
static HttpCell g_http[MAX_HTTP_CELLS];
static size_t   g_http_n = 0;

static atomic_ullong g_tasks_created;
static atomic_ullong g_tasks_pub_ok;
static atomic_ullong g_tasks_pub_fail;
static atomic_ullong g_tasks_done_ok;
static atomic_ullong g_tasks_done_dup;
static atomic_ullong g_tasks_done_err;
static atomic_ullong g_req_created;
static atomic_ullong g_req_ready;

void metrics_init(void) { /* zero by default */ }

void metrics_inc_http_request(const char* endpoint, int status) {
	for (size_t i = 0; i < g_http_n; ++i) {
		if (g_http[i].status == status && strcmp(g_http[i].endpoint, endpoint) == 0) {
			atomic_fetch_add(&g_http[i].count, 1);
			return;
		}
	}
	if (g_http_n < MAX_HTTP_CELLS) {
		g_http[g_http_n].endpoint = endpoint;
		g_http[g_http_n].status = status;
		atomic_store(&g_http[g_http_n].count, 1);
		g_http_n++;
	}
}

void metrics_inc_tasks_created(uint64_t n)        { atomic_fetch_add(&g_tasks_created, n); }
void metrics_inc_tasks_published_ok(void)         { atomic_fetch_add(&g_tasks_pub_ok, 1); }
void metrics_inc_tasks_published_failed(void)     { atomic_fetch_add(&g_tasks_pub_fail, 1); }
void metrics_inc_tasks_completed_ok(void)         { atomic_fetch_add(&g_tasks_done_ok, 1); }
void metrics_inc_tasks_completed_dup(void)        { atomic_fetch_add(&g_tasks_done_dup, 1); }
void metrics_inc_tasks_completed_error(void)      { atomic_fetch_add(&g_tasks_done_err, 1); }
void metrics_inc_requests_created(void)           { atomic_fetch_add(&g_req_created, 1); }
void metrics_inc_requests_ready(void)             { atomic_fetch_add(&g_req_ready, 1); }

static void appendf(char** buf, size_t* len, size_t* cap, const char* fmt, ...) {
	va_list ap; va_start(ap, fmt);
	for (;;) {
		size_t avail = *cap - *len;
		va_list ap2; va_copy(ap2, ap);
		int n = vsnprintf(*buf + *len, avail, fmt, ap2);
		va_end(ap2);
		if (n < 0) break;
		if ((size_t)n < avail) { *len += (size_t)n; break; }
		*cap = (*cap ? *cap * 2 : 1024);
		while (*cap < *len + (size_t)n + 1) *cap *= 2;
		*buf = (char*)realloc(*buf, *cap);
	}
	va_end(ap);
}

char* metrics_render(size_t* len_out) {
	char* b = NULL; size_t l = 0, c = 0;
	appendf(&b, &l, &c, "# HELP http_requests_total HTTP requests by endpoint and status\n");
	appendf(&b, &l, &c, "# TYPE http_requests_total counter\n");
	for (size_t i = 0; i < g_http_n; ++i) {
		appendf(&b, &l, &c, "http_requests_total{endpoint=\"%s\",status=\"%d\"} %llu\n",
			g_http[i].endpoint, g_http[i].status,
			(unsigned long long)atomic_load(&g_http[i].count));
	}
	appendf(&b, &l, &c,
		"# TYPE crack_requests_created_total counter\ncrack_requests_created_total %llu\n"
		"# TYPE crack_requests_ready_total counter\ncrack_requests_ready_total %llu\n"
		"# TYPE crack_tasks_created_total counter\ncrack_tasks_created_total %llu\n"
		"# TYPE crack_tasks_published_total counter\ncrack_tasks_published_total{result=\"ok\"} %llu\n"
		"crack_tasks_published_total{result=\"failed\"} %llu\n"
		"# TYPE crack_tasks_completed_total counter\ncrack_tasks_completed_total{result=\"ok\"} %llu\n"
		"crack_tasks_completed_total{result=\"duplicate\"} %llu\n"
		"crack_tasks_completed_total{result=\"error\"} %llu\n",
		(unsigned long long)atomic_load(&g_req_created),
		(unsigned long long)atomic_load(&g_req_ready),
		(unsigned long long)atomic_load(&g_tasks_created),
		(unsigned long long)atomic_load(&g_tasks_pub_ok),
		(unsigned long long)atomic_load(&g_tasks_pub_fail),
		(unsigned long long)atomic_load(&g_tasks_done_ok),
		(unsigned long long)atomic_load(&g_tasks_done_dup),
		(unsigned long long)atomic_load(&g_tasks_done_err));
	*len_out = l;
	return b;
}

#endif // METRICS_IMPL
#endif // METRICS_H
