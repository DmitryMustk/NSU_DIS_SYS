#ifndef UUID_H
#define UUID_H

#include <stdint.h>
#include <stddef.h>

void uuidv4(char* out);

#ifdef UUID_IMPL
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>

static int uuidv4_read_random(uint8_t* buf, size_t n) {
	int fd = open("/dev/urandom", O_RDONLY);
	if (fd < 0) return -1;
	size_t got = 0;
	while (got < n) {
		ssize_t r = read(fd, buf + got, n - got);
		if (r <= 0) { close(fd); return -1; }
		got += (size_t)r;
	}
	close(fd);
	return 0;
}

void uuidv4(char* out) {
	uint8_t b[16];
	if (uuidv4_read_random(b, sizeof(b)) != 0) {
		// extremely unlikely fallback
		for (int i = 0; i < 16; ++i) b[i] = (uint8_t)rand();
	}
	b[6] = (b[6] & 0x0F) | 0x40;       // version 4
	b[8] = (b[8] & 0x3F) | 0x80;       // variant 10
	snprintf(out, 37,
		"%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
		b[0],b[1],b[2],b[3], b[4],b[5], b[6],b[7], b[8],b[9],
		b[10],b[11],b[12],b[13],b[14],b[15]);
}
#endif // UUID_IMPL
#endif // UUID_H
