/*
 * Copyright (C) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define BEAU_RPMSG_DEVICE	"/dev/rpmsg0"
#define BEAU_RPROC_STATE	"/sys/class/remoteproc/remoteproc0/state"
#define BEAU_RPMSG_TIMEOUT_MS	3000
#define BEAU_RPMSG_PAYLOAD_MAX	496U
#define BEAU_RPROC_RETRY_COUNT	30U
#define BEAU_RPROC_RETRY_US	100000U

static int write_all(int fd, const char *buf, size_t len)
{
	size_t written = 0U;

	while (written < len) {
		ssize_t ret = write(fd, buf + written, len - written);

		if (ret < 0) {
			if (errno == EINTR) {
				continue;
			}
			return -1;
		}
		written += (size_t)ret;
	}
	return 0;
}

static int read_rproc_state(char *state, size_t state_size)
{
	ssize_t got;
	int fd;

	fd = open(BEAU_RPROC_STATE, O_RDONLY | O_CLOEXEC);
	if (fd < 0) {
		return -1;
	}
	got = read(fd, state, state_size - 1U);
	close(fd);
	if (got <= 0) {
		return -1;
	}
	state[got] = '\0';
	state[strcspn(state, "\r\n")] = '\0';
	return 0;
}

static int attach_rproc(void)
{
	char state[32];
	int fd;

	if (read_rproc_state(state, sizeof(state)) != 0) {
		fprintf(stderr, "read %s failed: %s\n", BEAU_RPROC_STATE,
			strerror(errno));
		return -1;
	}
	if (strcmp(state, "attached") == 0) {
		return 0;
	}
	if (strcmp(state, "detached") != 0) {
		fprintf(stderr, "remoteproc state is %s, expected detached or attached\n",
			state);
		return -1;
	}

	fd = open(BEAU_RPROC_STATE, O_WRONLY | O_CLOEXEC);
	if (fd < 0 || write_all(fd, "start", 5U) != 0) {
		fprintf(stderr, "attach remoteproc failed: %s\n", strerror(errno));
		if (fd >= 0) {
			close(fd);
		}
		return -1;
	}
	close(fd);

	for (unsigned int attempt = 0U; attempt < BEAU_RPROC_RETRY_COUNT; attempt++) {
		if (read_rproc_state(state, sizeof(state)) == 0 &&
		    strcmp(state, "attached") == 0) {
			return 0;
		}
		usleep(BEAU_RPROC_RETRY_US);
	}
	fprintf(stderr, "remoteproc did not attach within %u ms\n",
		BEAU_RPROC_RETRY_COUNT * (BEAU_RPROC_RETRY_US / 1000U));
	return -1;
}

static int open_rpmsg(void)
{
	int fd;

	for (unsigned int attempt = 0U; attempt < BEAU_RPROC_RETRY_COUNT; attempt++) {
		fd = open(BEAU_RPMSG_DEVICE, O_RDWR | O_CLOEXEC);
		if (fd >= 0) {
			return fd;
		}
		if (errno != ENOENT) {
			break;
		}
		usleep(BEAU_RPROC_RETRY_US);
	}
	fprintf(stderr, "open %s failed: %s\n", BEAU_RPMSG_DEVICE, strerror(errno));
	return -1;
}

int main(int argc, char *argv[])
{
	const char *payload = argc > 1 ? argv[1] : "beau-rpmsg-smoke";
	char reply[BEAU_RPMSG_PAYLOAD_MAX];
	struct pollfd pfd;
	size_t len = strlen(payload);
	ssize_t got;
	int fd;
	int ret;

	if (argc > 2 || len == 0U || len > sizeof(reply)) {
		fprintf(stderr, "usage: %s [payload up to %u bytes]\n", argv[0],
			BEAU_RPMSG_PAYLOAD_MAX);
		return 2;
	}

	if (attach_rproc() != 0) {
		return 1;
	}
	fd = open_rpmsg();
	if (fd < 0) {
		return 1;
	}
	if (write_all(fd, payload, len) != 0) {
		fprintf(stderr, "write %s failed: %s\n", BEAU_RPMSG_DEVICE,
			strerror(errno));
		close(fd);
		return 1;
	}

	pfd.fd = fd;
	pfd.events = POLLIN;
	pfd.revents = 0;
	ret = poll(&pfd, 1, BEAU_RPMSG_TIMEOUT_MS);
	if (ret < 0) {
		fprintf(stderr, "poll %s failed: %s\n", BEAU_RPMSG_DEVICE,
			strerror(errno));
		close(fd);
		return 1;
	}
	if (ret == 0) {
		fprintf(stderr, "timeout waiting for RPMsg echo\n");
		close(fd);
		return 1;
	}
	if ((pfd.revents & POLLIN) == 0) {
		fprintf(stderr, "unexpected %s poll status: 0x%x\n", BEAU_RPMSG_DEVICE,
			pfd.revents);
		close(fd);
		return 1;
	}

	got = read(fd, reply, sizeof(reply));
	close(fd);
	if (got < 0) {
		fprintf(stderr, "read %s failed: %s\n", BEAU_RPMSG_DEVICE,
			strerror(errno));
		return 1;
	}
	if ((size_t)got != len || memcmp(reply, payload, len) != 0) {
		fprintf(stderr, "RPMsg echo mismatch: expected %zu bytes, got %zd\n",
			len, got);
		return 1;
	}

	printf("rpmsg: %.*s\n", (int)got, reply);
	return 0;
}
