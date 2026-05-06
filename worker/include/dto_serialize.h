#ifndef DTO_SERIALIZE_H
#define DTO_SERIALIZE_H

#include <stdint.h>
#include <stddef.h>
#include "dto.h"

char* serializeResult(const Result* r);

#ifdef DTO_SERIALIZE_IMPL

static void sb_append_json_string(String_Builder* sb, const char* s) {
	sb_append_cstr(sb, "\"");
	for (const char* p = s; *p; ++p) {
		unsigned char c = (unsigned char)*p;
		switch (c) {
			case '"':  sb_append_cstr(sb, "\\\""); break;
			case '\\': sb_append_cstr(sb, "\\\\"); break;
			case '\b': sb_append_cstr(sb, "\\b");  break;
			case '\f': sb_append_cstr(sb, "\\f");  break;
			case '\n': sb_append_cstr(sb, "\\n");  break;
			case '\r': sb_append_cstr(sb, "\\r");  break;
			case '\t': sb_append_cstr(sb, "\\t");  break;
			default:
				if (c < 0x20) {
					char buf[8];
					snprintf(buf, sizeof(buf), "\\u%04x", c);
					sb_append_cstr(sb, buf);
				} else {
					da_append(sb, (char)c);
				}
		}
	}
	sb_append_cstr(sb, "\"");
}

char* serializeResult(const Result* r) {
	String_Builder sb = {0};
	sb_append_cstr(&sb, "{\"taskId\":");
	sb_append_json_string(&sb, r->taskId ? r->taskId : "");
	sb_append_cstr(&sb, ",\"requestId\":");
	sb_append_json_string(&sb, r->requestId ? r->requestId : "");
	sb_append_cstr(&sb, ",\"results\":[");
	if (r->results) {
		for (size_t i = 0; r->results[i] != NULL; ++i) {
			if (i > 0) sb_append_cstr(&sb, ",");
			sb_append_json_string(&sb, r->results[i]);
		}
	}
	sb_append_cstr(&sb, "],\"status\":");
	sb_append_json_string(&sb, r->status ? r->status : "DONE");
	sb_append_cstr(&sb, "}");
	sb_append_null(&sb);
	char* out = temp_strdup(sb.items);
	sb_free(sb);
	return out;
}

#endif // DTO_SERIALIZE_IMPL
#endif // DTO_SERIALIZE_H
