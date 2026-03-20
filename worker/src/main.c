#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include <omp.h>

#define NOB_IMPLEMENTATION
#define MD5_IMPLEMENTATION
#include "../thirdparty/nob.h"
#include "../thirdparty/md5.h"

#define NOB_STRIP_PREFIX

#define INSTPOW_IMPL
#define DTO_IMPL
#include "../include/instpow.h"
#include "../include/dto.h"

const char ALPHABET[] = "abcdefghijklmnopqrstuvwxyz0123456789";
const int  BASE       = 36;

uint64_t instpowPartSum(const int to) {
	uint64_t sum = 0;
	for (int i = 0; i <= to; ++i) {
		sum += instantPow(i);
	}

	return sum;
}

int getCombinationByIndex(char* const buf, const uint32_t bufLen, uint64_t index) {
	if (instantPow(bufLen - 2) == (uint64_t)-1) {
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

int getTask(Task* const t) {
	String_Builder sb = {};
	nob_read_entire_file("./src/task.json", &sb);
	int res = parseTask(t, sb.items, sb.count);
	if (res != 0) {
		return res;
	}
	return 0;
}

void bytesToHex(const uint8_t* const bytes, const size_t bytesSize, char* const hex, const size_t hexSize) {
	for (size_t i = 0; i < bytesSize; ++i) {
		sprintf(hex + i * 2, "%02x", bytes[i]);
	}
	hex[hexSize - 1] = '\0';
}

int main(void) {
	#pragma omp parallel
	{                                                                                    
		#pragma omp single                                                             
		nob_log(NOB_INFO, "Start bruting using %d threads", omp_get_num_threads());                    
	}

	Task t = {0};
	if (getTask(&t) != 0) {
		nob_log(NOB_ERROR, "Can't get task...");
		return -1;
	}
	
	int found = 0;
	#pragma omp parallel for shared(found)
	for (uint64_t i = t.startIndex; i < t.startIndex + t.count; ++i) {
		if (found) continue;

		const uint32_t size = getSizeForIndex(i) + 1;
		char comb[size];
		memset(comb, 0, size);
		getCombinationByIndex(comb, size, i);

		uint8_t hash[MD5_HASH_SIZE];
		md5String(comb, hash);

		char hex[33];
		bytesToHex(hash, MD5_HASH_SIZE, hex, MD5_HASH_SIZE * 2 + 1);

		if (strcmp(hex, t.targetHash) == 0) {
			nob_log(NOB_INFO, "FOUND: %s", comb);
			found = 1;
		}
	}

	if (!found) {
		nob_log(NOB_INFO, "Not found combination for %s hash", t.targetHash);
	}

	return 0;
}
