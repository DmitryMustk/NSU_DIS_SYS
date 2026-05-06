#ifndef HTTP_H
#define HTTP_H

#include <stdint.h>
#include <stddef.h>

#define HTTP_MAX_HEADERS    32
#define HTTP_MAX_PATH       512
#define HTTP_MAX_QUERY      1024
#define HTTP_MAX_HDR_BLOCK  8192
#define HTTP_MAX_BODY       (1 << 20)   // 1 MB

typedef struct {
	char         method[8];
	char         path[HTTP_MAX_PATH];
	char         query[HTTP_MAX_QUERY];
	const char*  body;
	size_t       body_len;
} HttpReq;

typedef struct {
	int           status;      
	const char*   content_type;
	const char*   body;
	size_t        body_len;
} HttpRes;

typedef HttpRes (*HttpHandler)(const HttpReq* req, void* ud);

typedef struct {
	const char*  method;
	const char*  path;
	HttpHandler  handler;
} HttpRoute;

typedef struct HttpServer HttpServer;

HttpServer* http_start(int port, const HttpRoute* routes, size_t n_routes, void* ud);
void        http_stop(HttpServer* s);
void        http_run(HttpServer* s, volatile int* stop_flag);

int  http_query_get(const char* query, const char* key, char* out, size_t outsz);

#ifdef HTTP_IMPL

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <poll.h>

#ifndef HTTP_LOG
#define HTTP_LOG(fmt, ...) fprintf(stderr, "[http] " fmt "\n", ##__VA_ARGS__)
#endif

struct HttpServer {
	int                listen_fd;
	const HttpRoute*   routes;
	size_t             n_routes;
	void*              ud;
};

static int http_set_nonblock(int fd) {
	int fl = fcntl(fd, F_GETFL, 0); if (fl < 0) return -1;
	return fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

HttpServer* http_start(int port, const HttpRoute* routes, size_t n_routes, void* ud) {
	int fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0) { HTTP_LOG("socket: %s", strerror(errno)); return NULL; }
	int one = 1; setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	struct sockaddr_in addr = {0};
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_ANY);
	addr.sin_port = htons(port);
	if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
		HTTP_LOG("bind :%d: %s", port, strerror(errno)); close(fd); return NULL;
	}
	if (listen(fd, 64) < 0) { HTTP_LOG("listen: %s", strerror(errno)); close(fd); return NULL; }
	http_set_nonblock(fd);
	HttpServer* s = (HttpServer*)calloc(1, sizeof(*s));
	s->listen_fd = fd; s->routes = routes; s->n_routes = n_routes; s->ud = ud;
	HTTP_LOG("listening on :%d", port);
	return s;
}

void http_stop(HttpServer* s) {
	if (!s) return;
	close(s->listen_fd);
	free(s);
}

static int read_until_double_crlf(int fd, char* buf, size_t cap, size_t* out_len) {
	size_t n = 0;
	while (n + 1 < cap) {
		ssize_t r = read(fd, buf + n, cap - 1 - n);
		if (r == 0) return -1;
		if (r < 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK) {
				struct pollfd pfd = { .fd = fd, .events = POLLIN };
				int p = poll(&pfd, 1, 5000);
				if (p <= 0) return -1;
				continue;
			}
			return -1;
		}
		n += (size_t)r;
		buf[n] = '\0';
		if (strstr(buf, "\r\n\r\n")) { *out_len = n; return 0; }
	}
	return -1; // header too large
}

static int read_exact(int fd, char* buf, size_t want) {
	size_t got = 0;
	while (got < want) {
		ssize_t r = read(fd, buf + got, want - got);
		if (r == 0) return -1;
		if (r < 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK) {
				struct pollfd pfd = { .fd = fd, .events = POLLIN };
				int p = poll(&pfd, 1, 30000);
				if (p <= 0) return -1;
				continue;
			}
			return -1;
		}
		got += (size_t)r;
	}
	return 0;
}

static int write_all(int fd, const char* buf, size_t len) {
	size_t sent = 0;
	while (sent < len) {
		ssize_t w = write(fd, buf + sent, len - sent);
		if (w < 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK) {
				struct pollfd pfd = { .fd = fd, .events = POLLOUT };
				int p = poll(&pfd, 1, 30000);
				if (p <= 0) return -1;
				continue;
			}
			return -1;
		}
		sent += (size_t)w;
	}
	return 0;
}

static const char* http_status_text(int s) {
	switch (s) {
		case 200: return "OK";
		case 201: return "Created";
		case 202: return "Accepted";
		case 204: return "No Content";
		case 400: return "Bad Request";
		case 404: return "Not Found";
		case 405: return "Method Not Allowed";
		case 413: return "Payload Too Large";
		case 500: return "Internal Server Error";
		case 503: return "Service Unavailable";
		default:  return "OK";
	}
}

static int parse_request_line(char* line, HttpReq* req) {
	char* sp1 = strchr(line, ' '); if (!sp1) return -1;
	*sp1 = '\0';
	char* path_start = sp1 + 1;
	char* sp2 = strchr(path_start, ' '); if (!sp2) return -1;
	*sp2 = '\0';
	if (strlen(line) >= sizeof(req->method)) return -1;
	strcpy(req->method, line);
	char* qmark = strchr(path_start, '?');
	if (qmark) {
		*qmark = '\0';
		strncpy(req->query, qmark + 1, sizeof(req->query) - 1);
	}
	strncpy(req->path, path_start, sizeof(req->path) - 1);
	return 0;
}

static long parse_content_length(const char* hdr) {
	const char* k = "content-length:";
	size_t kl = strlen(k);
	const char* p = hdr;
	while ((p = strchr(p, '\n')) != NULL) {
		++p;
		size_t i = 0;
		while (k[i] && p[i]) {
			char a = k[i], b = p[i];
			if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
			if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
			if (a != b) break;
			++i;
		}
		if (i == kl) {
			const char* v = p + kl;
			while (*v == ' ' || *v == '\t') ++v;
			return strtol(v, NULL, 10);
		}
	}
	return -1;
}

static void send_response(int fd, const HttpRes* res) {
	char head[256];
	int n = snprintf(head, sizeof(head),
		"HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n",
		res->status, http_status_text(res->status),
		res->content_type ? res->content_type : "text/plain",
		res->body_len);
	write_all(fd, head, (size_t)n);
	if (res->body && res->body_len) write_all(fd, res->body, res->body_len);
}

static void send_simple(int fd, int status, const char* msg) {
	HttpRes r = { .status = status, .content_type = "text/plain", .body = msg, .body_len = msg ? strlen(msg) : 0 };
	send_response(fd, &r);
}

static void handle_one(HttpServer* s, int fd) {
	char hdrbuf[HTTP_MAX_HDR_BLOCK];
	size_t hdrn = 0;
	if (read_until_double_crlf(fd, hdrbuf, sizeof(hdrbuf), &hdrn) < 0) {
		close(fd); return;
	}
	char* body_start = strstr(hdrbuf, "\r\n\r\n");
	*body_start = '\0';
	body_start += 4;
	size_t already = hdrn - (size_t)(body_start - hdrbuf);

	long clen = parse_content_length(hdrbuf);

	char* eol = strstr(hdrbuf, "\r\n");
	HttpReq req = {0};
	if (eol) *eol = '\0';
	if (parse_request_line(hdrbuf, &req) < 0) { send_simple(fd, 400, "bad request"); close(fd); return; }
	if (eol) *eol = '\r';
	char* body = NULL;
	if (clen > 0) {
		if (clen > HTTP_MAX_BODY) { send_simple(fd, 413, "too large"); close(fd); return; }
		body = (char*)malloc((size_t)clen + 1);
		if (already > (size_t)clen) already = (size_t)clen;
		memcpy(body, body_start, already);
		if (already < (size_t)clen) {
			if (read_exact(fd, body + already, (size_t)clen - already) < 0) {
				free(body); close(fd); return;
			}
		}
		body[clen] = '\0';
		req.body = body; req.body_len = (size_t)clen;
	}

	HttpHandler h = NULL;
	for (size_t i = 0; i < s->n_routes; ++i) {
		const HttpRoute* r = &s->routes[i];
		if (strcmp(r->path, req.path) != 0) continue;
		if (r->method && strcmp(r->method, req.method) != 0) {
			send_simple(fd, 405, "method not allowed"); close(fd); free(body); return;
		}
		h = r->handler; break;
	}
	if (!h) { send_simple(fd, 404, "not found"); close(fd); free(body); return; }

	HttpRes res = h(&req, s->ud);
	send_response(fd, &res);
	free(body);
	close(fd);
}

void http_run(HttpServer* s, volatile int* stop_flag) {
	while (!*stop_flag) {
		struct pollfd pfd = { .fd = s->listen_fd, .events = POLLIN };
		int p = poll(&pfd, 1, 500);
		if (p < 0) { if (errno == EINTR) continue; HTTP_LOG("poll: %s", strerror(errno)); break; }
		if (p == 0) continue;
		struct sockaddr_in caddr; socklen_t clen = sizeof(caddr);
		int cfd = accept(s->listen_fd, (struct sockaddr*)&caddr, &clen);
		if (cfd < 0) { if (errno == EAGAIN || errno == EWOULDBLOCK) continue; HTTP_LOG("accept: %s", strerror(errno)); continue; }
		http_set_nonblock(cfd);
		handle_one(s, cfd);
	}
}

static int hex_nibble(char c) {
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	return -1;
}

int http_query_get(const char* query, const char* key, char* out, size_t outsz) {
	if (!query || !*query) return -1;
	size_t klen = strlen(key);
	const char* p = query;
	while (p && *p) {
		const char* eq  = strchr(p, '=');
		const char* amp = strchr(p, '&');
		if (!eq || (amp && eq > amp)) {
			if (!amp) break;
			p = amp + 1; continue;
		}
		size_t this_klen = (size_t)(eq - p);
		const char* val = eq + 1;
		size_t vlen = amp ? (size_t)(amp - val) : strlen(val);
		if (this_klen == klen && strncmp(p, key, klen) == 0) {
			size_t out_i = 0;
			for (size_t i = 0; i < vlen && out_i + 1 < outsz; ++i) {
				if (val[i] == '+') { out[out_i++] = ' '; continue; }
				if (val[i] == '%' && i + 2 < vlen) {
					int hi = hex_nibble(val[i+1]), lo = hex_nibble(val[i+2]);
					if (hi >= 0 && lo >= 0) { out[out_i++] = (char)((hi << 4) | lo); i += 2; continue; }
				}
				out[out_i++] = val[i];
			}
			out[out_i] = '\0';
			return 0;
		}
		if (!amp) break;
		p = amp + 1;
	}
	return -1;
}

#endif // HTTP_IMPL
#endif // HTTP_H
