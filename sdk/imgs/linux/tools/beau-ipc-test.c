/* SPDX-License-Identifier: BSD-3-Clause */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define BEAU_IPC_MESSAGE_MAX 192U
#define BEAU_IPC_SYSFS_MISC "/sys/class/misc"

static int parse_channel(const char *text, uint32_t *channel)
{
	char *end;
	unsigned long value;
	if (!text || !*text || *text == '-')
		return -EINVAL;
	errno = 0;
	value = strtoul(text, &end, 0);
	while (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n')
		end++;
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

static int read_channel_peer(const char *name, uint32_t *peer_vmid)
{
	char path[160];
	char value[32];
	ssize_t length;
	int fd;

	if (snprintf(path, sizeof(path), BEAU_IPC_SYSFS_MISC "/%s/device/peer_vmid",
		name) >= (int)sizeof(path))
		return -ENAMETOOLONG;
	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return -errno;
	length = read(fd, value, sizeof(value) - 1U);
	close(fd);
	if (length < 0)
		return -errno;
	if (length == 0)
		return -EIO;
	value[length] = '\0';
	return parse_channel(value, peer_vmid);
}

static int find_channel_for_peer(uint32_t peer_vmid, uint32_t *channel)
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
		uint32_t candidate, candidate_peer;

		if (strncmp(entry->d_name, prefix, sizeof(prefix) - 1U) != 0 ||
		    parse_channel(entry->d_name + sizeof(prefix) - 1U, &candidate))
			continue;
		if (read_channel_peer(entry->d_name, &candidate_peer) != 0 ||
		    candidate_peer != peer_vmid)
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

static int send_to_peer(uint32_t peer_vmid, const char *message)
{
	size_t length = strnlen(message, BEAU_IPC_MESSAGE_MAX + 1U);
	uint32_t channel = UINT32_MAX;
	int fd;
	int ret;

	if (!length || length > BEAU_IPC_MESSAGE_MAX)
		return -EMSGSIZE;
	ret = find_channel_for_peer(peer_vmid, &channel);
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
	printf("hipc: channel=%u peer-vm=%u sent:%s\n", channel, peer_vmid, message);
	fflush(stdout);
	return 0;
}

int main(int argc, char **argv)
{
	uint32_t peer_vmid;
	int ret;
	if (argc < 2) {
		fprintf(stderr, "usage: %s client send <payload> | server send <vmid> <payload> | secure send <payload>\n", argv[0]);
		return 2;
	}
	if (!strcmp(argv[1], "client") && argc == 4 && !strcmp(argv[2], "send"))
		ret = send_to_peer(1U, argv[3]);
	else if (!strcmp(argv[1], "server") && argc == 5 && !strcmp(argv[2], "send") &&
		 !parse_channel(argv[3], &peer_vmid))
		ret = send_to_peer(peer_vmid, argv[4]);
	else if (!strcmp(argv[1], "secure") && argc == 4 && !strcmp(argv[2], "send"))
		ret = send_to_peer(0U, argv[3]);
	else
		ret = -EINVAL;
	if (ret) {
		fprintf(stderr, "ipc failed: %s\n", strerror(-ret));
		return 1;
	}
	return 0;
}
