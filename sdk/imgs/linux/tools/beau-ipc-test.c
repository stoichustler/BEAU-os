/* SPDX-License-Identifier: BSD-3-Clause */
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define BEAU_IPC_MESSAGE_MAX 1024U
#define BEAU_IPC_TIMEOUT_MS 3000

static int parse_channel(const char *text, uint32_t *channel)
{
	char *end;
	unsigned long value;
	if (!text || !*text || *text == '-')
		return -EINVAL;
	errno = 0;
	value = strtoul(text, &end, 0);
	if (errno || *end || value > UINT32_MAX)
		return -EINVAL;
	*channel = (uint32_t)value;
	return 0;
}

static int open_channel(uint32_t channel)
{
	char path[64];
	int length = snprintf(path, sizeof(path), "/dev/beau-ipc-%u", channel);
	if (length < 0 || (size_t)length >= sizeof(path))
		return -ENAMETOOLONG;
	length = open(path, O_RDWR);
	return length < 0 ? -errno : length;
}

static int wait_readable(int fd)
{
	struct pollfd pfd = { .fd = fd, .events = POLLIN };
	int ret;
	do {
		ret = poll(&pfd, 1, BEAU_IPC_TIMEOUT_MS);
	} while (ret < 0 && errno == EINTR);
	if (ret == 0)
		return -ETIMEDOUT;
	if (ret < 0)
		return -errno;
	return (pfd.revents & (POLLERR | POLLNVAL)) ? -EIO : 0;
}

static int run_server(uint32_t channel)
{
	uint8_t buffer[BEAU_IPC_MESSAGE_MAX];
	int fd = open_channel(channel);
	if (fd < 0)
		return fd;
	printf("ipc echo server: channel %u\n", channel);
	for (;;) {
		ssize_t length;
		int ret = wait_readable(fd);
		if (ret)
			continue;
		length = read(fd, buffer, sizeof(buffer));
		if (length <= 0 || write(fd, buffer, (size_t)length) != length) {
			fprintf(stderr, "ipc server I/O failed: %s\n", strerror(errno));
			close(fd);
			return 1;
		}
	}
}

static int run_client(uint32_t channel, const char *message)
{
	uint8_t reply[BEAU_IPC_MESSAGE_MAX];
	size_t length = strlen(message);
	int fd, ret;
	ssize_t received;
	if (!length || length > sizeof(reply))
		return -EMSGSIZE;
	fd = open_channel(channel);
	if (fd < 0)
		return fd;
	if (write(fd, message, length) != (ssize_t)length) {
		ret = -errno;
		close(fd);
		return ret;
	}
	ret = wait_readable(fd);
	if (!ret) {
		received = read(fd, reply, sizeof(reply));
		if (received != (ssize_t)length || memcmp(reply, message, length))
			ret = -EBADMSG;
	}
	close(fd);
	if (ret)
		return ret;
	printf("ipc: channel=%u echo=%s\n", channel, message);
	return 0;
}

int main(int argc, char **argv)
{
	uint32_t channel;
	int ret;
	if (argc < 3 || parse_channel(argv[2], &channel)) {
		fprintf(stderr, "usage: %s server <channel> | client <channel> <payload>\n", argv[0]);
		return 2;
	}
	if (!strcmp(argv[1], "server") && argc == 3)
		ret = run_server(channel);
	else if (!strcmp(argv[1], "client") && argc == 4)
		ret = run_client(channel, argv[3]);
	else
		ret = -EINVAL;
	if (ret) {
		fprintf(stderr, "ipc failed: %s\n", strerror(-ret));
		return 1;
	}
	return 0;
}
