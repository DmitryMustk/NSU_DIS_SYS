#include <stdint.h>
#include <stddef.h>
#include <string.h>

#define NOB_IMPLEMENTATION
#include "../thirdparty/nob.h"
#include "../thirdparty/jsmn.h"

#define NOB_STRIP_PREFIX



/*
{
  "taskId": "70c26e4e-0d51-11f1-a84d-73f4fbe1eca2",
  "requestId": "730a04e6-4de9-41f9-9d5b-53b88b17afac",
  "startIndex": 0,
  "count": 100000,
  "targetHash": "e2fc714c4727ee9395f324cd2e7f331f",
  "maxLength": 4
}
*/

typedef struct Task {
	char*	taskId;
	char*	requestId;
	int32_t startIndex;
	int32_t count;
	char*	targetHash;
	int16_t maxLength;
} Task;

char* TaskToString(Task* task) {
	return temp_sprintf("Task: {\n\ttaskId: %s,\n\trequestId: %s,\n\tstartIndex: %d,\n\tcount: %d,\n\ttargetHash: %s,\n\tmaxLength: %d,\n}",
			task->taskId,
			task->requestId,
			task->startIndex,
			task->count,
			task->targetHash,
			task->maxLength
		);
}


static int jsonEq(const char* json, jsmntok_t* tok, const char *fieldName) {
	if (tok->type == JSMN_STRING && (int)strlen(fieldName) == tok->end - tok->start &&
			strncmp(json + tok->start, fieldName, tok->end - tok->start) == 0) {
		return 0;
	}

	return -1;
}


int parseTask(Task* task) {
	int result = 0;
	String_Builder sb = {0};
	read_entire_file("./src/task.json", &sb);
	nob_log(NOB_INFO, "READ: %s", sb.items);

	jsmn_parser p;
	jsmntok_t t[128];

	jsmn_init(&p);
	int tokCount = jsmn_parse(&p, sb.items, sb.count, t, ARRAY_LEN(t));
	if (tokCount < 0) {
		nob_log(NOB_ERROR, "Failed to parse JSON: %d", result);
		return_defer(-1);
	}

	if (tokCount < 1 || t[0].type != JSMN_OBJECT) {
		nob_log(NOB_ERROR, "Object expected: %d", result);
		return_defer(-1);
	}

	for (int i = 1; i < tokCount; ++i) {
		if (jsonEq(sb.items, &t[i], "taskId") == 0) {
			task->taskId = temp_strndup(sb.items + t[i + 1].start, t[i + 1].end - t[i + 1].start);
		}
		else if (jsonEq(sb.items, &t[i], "requestId") == 0) {
			task->requestId = temp_strndup(sb.items + t[i + 1].start, t[i + 1].end - t[i + 1].start);
		}
		else if (jsonEq(sb.items, &t[i], "startIndex") == 0) {
			task->startIndex = strtol(sb.items + t[i + 1].start, NULL, 10);
		}
		else if (jsonEq(sb.items, &t[i], "count") == 0) {
			task->count = strtol(sb.items + t[i + 1].start, NULL, 10);
		}
		else if (jsonEq(sb.items, &t[i], "targetHash") == 0) {
			task->targetHash = temp_strndup(sb.items + t[i + 1].start, t[i + 1].end - t[i + 1].start);
		}
		else if (jsonEq(sb.items, &t[i], "maxLength") == 0) {
			task->maxLength = strtol(sb.items + t[i + 1].start, NULL, 10);
		}
	}
	
defer:
	sb_free(sb);
	return 0;
}


int main(void) {
    nob_log(NOB_INFO, "Start...");	
	Task t = {0};
	
	int res = parseTask(&t);
	if (res != 0) {
		return res;
	}
	nob_log(NOB_INFO, "Deserialize: \n%s", TaskToString(&t));

	return 0;
}
