#ifndef DTO_H
#define DTO_H

#include "../thirdparty/jsmn.h"

typedef struct Task {
	char*    taskId;
	char*    requestId;
	uint64_t startIndex;
	uint64_t count;
	char*    targetHash;
	uint64_t maxLength;
} Task;

typedef struct Result {
	char*  taskId;
	char*  requestId;
	char** results;
	char*  status;
} Result;

#ifdef DTO_IMPL

int parseTask(Task* task, const char* json, size_t jsonLen) {
	jsmn_parser p;
	jsmntok_t t[128];

	jsmn_init(&p);
	int tokCount = jsmn_parse(&p, json, jsonLen, t, sizeof(t) / sizeof(t[0]));
	if (tokCount < 0) return -1;
	if (tokCount < 1 || t[0].type != JSMN_OBJECT) return -1;

	for (int i = 1; i < tokCount; ++i) {
		if (t[i].type == JSMN_STRING && (int)strlen("taskId") == t[i].end - t[i].start &&
				strncmp(json + t[i].start, "taskId", t[i].end - t[i].start) == 0) {
			task->taskId = temp_strndup(json + t[i + 1].start, t[i + 1].end - t[i + 1].start);
		}
		else if (t[i].type == JSMN_STRING && (int)strlen("requestId") == t[i].end - t[i].start &&
				strncmp(json + t[i].start, "requestId", t[i].end - t[i].start) == 0) {
			task->requestId = temp_strndup(json + t[i + 1].start, t[i + 1].end - t[i + 1].start);
		}
		else if (t[i].type == JSMN_STRING && (int)strlen("startIndex") == t[i].end - t[i].start &&
				strncmp(json + t[i].start, "startIndex", t[i].end - t[i].start) == 0) {
			task->startIndex = strtol(json + t[i + 1].start, NULL, 10);
		}
		else if (t[i].type == JSMN_STRING && (int)strlen("count") == t[i].end - t[i].start &&
				strncmp(json + t[i].start, "count", t[i].end - t[i].start) == 0) {
			task->count = strtol(json + t[i + 1].start, NULL, 10);
		}
		else if (t[i].type == JSMN_STRING && (int)strlen("targetHash") == t[i].end - t[i].start &&
				strncmp(json + t[i].start, "targetHash", t[i].end - t[i].start) == 0) {
			task->targetHash = temp_strndup(json + t[i + 1].start, t[i + 1].end - t[i + 1].start);
		}
		else if (t[i].type == JSMN_STRING && (int)strlen("maxLength") == t[i].end - t[i].start &&
				strncmp(json + t[i].start, "maxLength", t[i].end - t[i].start) == 0) {
			task->maxLength = strtol(json + t[i + 1].start, NULL, 10);
		}
	}
	return 0;
}

int parseResult(Result* result, const char* json, size_t jsonLen) {
	jsmn_parser p;
	jsmntok_t t[128];

	jsmn_init(&p);
	int tokCount = jsmn_parse(&p, json, jsonLen, t, sizeof(t) / sizeof(t[0]));
	if (tokCount < 0) return -1;
	if (tokCount < 1 || t[0].type != JSMN_OBJECT) return -1;

	for (int i = 1; i < tokCount; ++i) {
		if (t[i].type == JSMN_STRING && (int)strlen("taskId") == t[i].end - t[i].start &&
				strncmp(json + t[i].start, "taskId", t[i].end - t[i].start) == 0) {
			result->taskId = temp_strndup(json + t[i + 1].start, t[i + 1].end - t[i + 1].start);
		}
		else if (t[i].type == JSMN_STRING && (int)strlen("requestId") == t[i].end - t[i].start &&
				strncmp(json + t[i].start, "requestId", t[i].end - t[i].start) == 0) {
			result->requestId = temp_strndup(json + t[i + 1].start, t[i + 1].end - t[i + 1].start);
		}
		else if (t[i].type == JSMN_STRING && (int)strlen("results") == t[i].end - t[i].start &&
				strncmp(json + t[i].start, "results", t[i].end - t[i].start) == 0) {
			int arrSize_results = t[i + 1].size;
			result->results = temp_alloc(sizeof(char*) * arrSize_results);
			for (int j = 0; j < arrSize_results; ++j) {
				result->results[j] = temp_strndup(json + t[i + 2 + j].start, t[i + 2 + j].end - t[i + 2 + j].start);
			}
			i += arrSize_results + 1;
		}
		else if (t[i].type == JSMN_STRING && (int)strlen("status") == t[i].end - t[i].start &&
				strncmp(json + t[i].start, "status", t[i].end - t[i].start) == 0) {
			result->status = temp_strndup(json + t[i + 1].start, t[i + 1].end - t[i + 1].start);
		}
	}
	return 0;
}

char* TaskToString(Task* task) {
	return temp_sprintf("Task: {\n\ttaskId: %s,\n\trequestId: %s,\n\tstartIndex: %lu,\n\tcount: %lu,\n\ttargetHash: %s,\n\tmaxLength: %lu\n}",
		task->taskId,
		task->requestId,
		task->startIndex,
		task->count,
		task->targetHash,
		task->maxLength
	);
}

char* ResultToString(Result* result) {
	return temp_sprintf("Result: {\n\ttaskId: %s,\n\trequestId: %s,\n\tresults: [...],\n\tstatus: %s\n}",
		result->taskId,
		result->requestId,
		result->status
	);
}

#endif //DTO_IMPL

#endif //DTO_H