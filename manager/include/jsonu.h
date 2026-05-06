#ifndef JSONU_H
#define JSONU_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include "../thirdparty/jsmn.h"

int  json_parse_object(const char* src, size_t srclen, jsmntok_t* tokens, size_t cap);
int  json_obj_get(const char* src, jsmntok_t* tokens, int ntok, const char* key);
int  json_get_string(const char* src, const jsmntok_t* tok, char* out, size_t outsz);
int  json_get_uint(const char* src, const jsmntok_t* tok, uint64_t* out);

typedef struct {
	char*  data;
	size_t len;
	size_t cap;
} JBuf;

void jbuf_init(JBuf* b);
void jbuf_free(JBuf* b);
void jbuf_putc(JBuf* b, char c);
void jbuf_puts(JBuf* b, const char* s);
void jbuf_putn(JBuf* b, const char* s, size_t n);
void jbuf_putu(JBuf* b, uint64_t v);
void jbuf_str(JBuf* b, const char* s);
void jbuf_kv_str(JBuf* b, const char* k, const char* v);
void jbuf_kv_u(JBuf* b, const char* k, uint64_t v);

#ifdef JSONU_IMPL

int json_parse_object(const char* src, size_t srclen, jsmntok_t* tokens, size_t cap) {
	jsmn_parser p; jsmn_init(&p);
	int n = jsmn_parse(&p, src, srclen, tokens, (unsigned int)cap);
	if (n < 1) return -1;
	if (tokens[0].type != JSMN_OBJECT) return -1;
	return n;
}

int json_obj_get(const char* src, jsmntok_t* tokens, int ntok, const char* key) {
	int klen = (int)strlen(key);
	int i = 1;
	while (i < ntok) {
		jsmntok_t* k = &tokens[i];
		if (k->type != JSMN_STRING) break;
		if (k->end - k->start == klen && strncmp(src + k->start, key, klen) == 0) {
			return i + 1;
		}
		i += 2;
		if (i - 1 < ntok && (tokens[i - 1].type == JSMN_ARRAY || tokens[i - 1].type == JSMN_OBJECT)) {
			int remaining = tokens[i - 1].size;
			while (remaining > 0 && i < ntok) {
				if (tokens[i].type == JSMN_ARRAY || tokens[i].type == JSMN_OBJECT)
					remaining += tokens[i].size;
				remaining--;
				i++;
			}
		}
	}
	return -1;
}

int json_get_string(const char* src, const jsmntok_t* tok, char* out, size_t outsz) {
	if (tok->type != JSMN_STRING) return -1;
	size_t n = (size_t)(tok->end - tok->start);
	if (n >= outsz) n = outsz - 1;
	memcpy(out, src + tok->start, n);
	out[n] = '\0';
	return 0;
}

int json_get_uint(const char* src, const jsmntok_t* tok, uint64_t* out) {
	if (tok->type != JSMN_PRIMITIVE) return -1;
	char buf[32];
	size_t n = (size_t)(tok->end - tok->start);
	if (n >= sizeof(buf)) return -1;
	memcpy(buf, src + tok->start, n);
	buf[n] = '\0';
	char* e;
	unsigned long long v = strtoull(buf, &e, 10);
	if (*e != '\0') return -1;
	*out = (uint64_t)v;
	return 0;
}

void jbuf_init(JBuf* b) { b->data = NULL; b->len = 0; b->cap = 0; }
void jbuf_free(JBuf* b) { free(b->data); b->data = NULL; b->len = b->cap = 0; }

static void jbuf_grow(JBuf* b, size_t need) {
	if (b->len + need + 1 <= b->cap) return;
	size_t nc = b->cap ? b->cap * 2 : 64;
	while (nc < b->len + need + 1) nc *= 2;
	b->data = (char*)realloc(b->data, nc);
	b->cap = nc;
}

void jbuf_putc(JBuf* b, char c)            { jbuf_grow(b, 1); b->data[b->len++] = c; b->data[b->len] = 0; }
void jbuf_putn(JBuf* b, const char* s, size_t n) { jbuf_grow(b, n); memcpy(b->data + b->len, s, n); b->len += n; b->data[b->len] = 0; }
void jbuf_puts(JBuf* b, const char* s)     { jbuf_putn(b, s, strlen(s)); }
void jbuf_putu(JBuf* b, uint64_t v) {
	char tmp[32]; int n = snprintf(tmp, sizeof(tmp), "%llu", (unsigned long long)v);
	jbuf_putn(b, tmp, (size_t)n);
}

void jbuf_str(JBuf* b, const char* s) {
	jbuf_putc(b, '"');
	for (const char* p = s; *p; ++p) {
		unsigned char c = (unsigned char)*p;
		switch (c) {
			case '"':  jbuf_puts(b, "\\\""); break;
			case '\\': jbuf_puts(b, "\\\\"); break;
			case '\b': jbuf_puts(b, "\\b"); break;
			case '\f': jbuf_puts(b, "\\f"); break;
			case '\n': jbuf_puts(b, "\\n"); break;
			case '\r': jbuf_puts(b, "\\r"); break;
			case '\t': jbuf_puts(b, "\\t"); break;
			default:
				if (c < 0x20) {
					char e[8]; snprintf(e, sizeof(e), "\\u%04x", c); jbuf_puts(b, e);
				} else jbuf_putc(b, (char)c);
		}
	}
	jbuf_putc(b, '"');
}

void jbuf_kv_str(JBuf* b, const char* k, const char* v) {
	jbuf_str(b, k); jbuf_putc(b, ':'); jbuf_str(b, v);
}
void jbuf_kv_u(JBuf* b, const char* k, uint64_t v) {
	jbuf_str(b, k); jbuf_putc(b, ':'); jbuf_putu(b, v);
}

#endif // JSONU_IMPL
#endif // JSONU_H
