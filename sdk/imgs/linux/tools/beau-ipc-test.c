/* SPDX-License-Identifier: BSD-3-Clause */
#include <dirent.h>
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

static int find_unique_channel(uint32_t *channel)
{
	static const char prefix[] = "beau-ipc-";
	DIR *dir;
	struct dirent *entry;
	uint32_t found = UINT32_MAX;
	int ret = -ENODEV;

	if (channel == NULL)
		return -EINVAL;
	dir = opendir("/dev");
	if (dir == NULL)
		return -errno;

	while ((entry = readdir(dir)) != NULL) {
		uint32_t candidate;

		if (strncmp(entry->d_name, prefix, sizeof(prefix) - 1U) != 0 ||
		    parse_channel(entry->d_name + sizeof(prefix) - 1U, &candidate))
			continue;
		if (found != UINT32_MAX) {
			ret = -ENOTUNIQ;
			goto out;
		}
		found = candidate;
	}
	if (found != UINT32_MAX) {
		*channel = found;
		ret = 0;
	}
out:
	closedir(dir);
	return ret;
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
	printf("hipc server: channel %u\n", channel);
	fflush(stdout);
	for (;;) {
		ssize_t length;
		int ret = wait_readable(fd);
		if (ret)
			continue;
		length = read(fd, buffer, sizeof(buffer));
		if (length <= 0) {
			ret = length < 0 ? -errno : -EIO;
			fprintf(stderr, "hipc server read failed: %s\n", strerror(-ret));
			close(fd);
			return ret;
		}
		if (printf("hipc: channel=%u recv:", channel) < 0 ||
		    fwrite(buffer, 1U, (size_t)length, stdout) != (size_t)length ||
		    fputc('\n', stdout) == EOF || fflush(stdout) == EOF) {
			ret = errno != 0 ? -errno : -EIO;
			close(fd);
			return ret;
		}
	}
}

static int run_client(const char *message)
{
	size_t length = strnlen(message, BEAU_IPC_MESSAGE_MAX + 1U);
	uint32_t channel = UINT32_MAX;
	int fd;
	int ret;

	if (!length || length > BEAU_IPC_MESSAGE_MAX)
		return -EMSGSIZE;
	ret = find_unique_channel(&channel);
	if (ret != 0)
		return ret;
	fd = open_channel(channel);
	if (fd < 0)
		return fd;
	if (write(fd, message, length) != (ssize_t)length) {
		ret = -errno;
		close(fd);
		return ret;
	}
	close(fd);
	printf("hipc: channel=%u sent:%s\n", channel, message);
	fflush(stdout);
	return 0;
}

int main(int argc, char **argv)
{
	uint32_t channel;
	int ret;
	if (argc < 2) {
		fprintf(stderr, "usage: %s server <channel> | client send <payload>\n", argv[0]);
		return 2;
	}
	if (!strcmp(argv[1], "server") && argc == 3 && !parse_channel(argv[2], &channel))
		ret = run_server(channel);
	else if (!strcmp(argv[1], "client") && argc == 4 && !strcmp(argv[2], "send"))
		ret = run_client(argv[3]);
	else
		ret = -EINVAL;
	if (ret) {
		fprintf(stderr, "ipc failed: %s\n", strerror(-ret));
		return 1;
	}
	return 0;
}
