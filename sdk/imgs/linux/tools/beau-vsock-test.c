/*
 * Copyright (C) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <linux/vm_sockets.h>

#define BEAU_VSOCK_BUFFER_SIZE	4096U
#define BEAU_VSOCK_PAYLOAD_MAX	65536U
#define BEAU_VSOCK_CLOSE_TIMEOUT_MS	3000

static void vsock_usage(const char *program)
{
	fprintf(stderr, "usage: %s cid\n", program);
	fprintf(stderr, "       %s server <port>\n", program);
	fprintf(stderr, "       %s client <cid> <port> <payload>\n", program);
}

static int parse_u32(const char *text, uint32_t *value, bool allow_zero)
{
	char *end = NULL;
	unsigned long long parsed;

	if ((text == NULL) || (*text == '\0') || (*text == '-')) {
		return -EINVAL;
	}

	errno = 0;
	parsed = strtoull(text, &end, 0);
	if ((errno != 0) || (end == text) || (*end != '\0') ||
		(parsed > UINT32_MAX) || ((!allow_zero) && (parsed == 0U))) {
		return -EINVAL;
	}

	*value = (uint32_t)parsed;
	return 0;
}

static int write_all(int fd, const void *buffer, size_t length)
{
	const uint8_t *data = buffer;
	size_t written = 0U;

	while (written < length) {
		ssize_t ret = send(fd, data + written, length - written, MSG_NOSIGNAL);

		if (ret < 0) {
			if (errno == EINTR) {
				continue;
			}
			return -errno;
		}
		if (ret == 0) {
			return -EIO;
		}
		written += (size_t)ret;
	}

	return 0;
}

static int read_exact(int fd, void *buffer, size_t length)
{
	uint8_t *data = buffer;
	size_t received = 0U;

	while (received < length) {
		ssize_t ret = recv(fd, data + received, length - received, 0);

		if (ret < 0) {
			if (errno == EINTR) {
				continue;
			}
			return -errno;
		}
		if (ret == 0) {
			return -ECONNRESET;
		}
		received += (size_t)ret;
	}

	return 0;
}

static int print_local_cid(void)
{
	struct sockaddr_vm address;
	socklen_t address_length = sizeof(address);
	int fd;

	fd = socket(AF_VSOCK, SOCK_STREAM, 0);
	if (fd < 0) {
		fprintf(stderr, "vsock socket failed: %s\n", strerror(errno));
		return 1;
	}
	memset(&address, 0, sizeof(address));
	address.svm_family = AF_VSOCK;
	address.svm_cid = VMADDR_CID_ANY;
	address.svm_port = VMADDR_PORT_ANY;
	if (bind(fd, (struct sockaddr *)&address, sizeof(address)) != 0) {
		fprintf(stderr, "vsock local CID bind failed: %s\n", strerror(errno));
		close(fd);
		return 1;
	}
	memset(&address, 0, sizeof(address));
	if (getsockname(fd, (struct sockaddr *)&address, &address_length) != 0) {
		fprintf(stderr, "vsock local CID query failed: %s\n", strerror(errno));
		close(fd);
		return 1;
	}
	close(fd);

	if ((address_length != sizeof(address)) || (address.svm_family != AF_VSOCK)) {
		fprintf(stderr, "vsock local CID query returned an invalid address\n");
		return 1;
	}
	printf("cid: %" PRIu32 "\n", address.svm_cid);
	return 0;
}

static int echo_connection(int fd)
{
	uint8_t buffer[BEAU_VSOCK_BUFFER_SIZE];
	struct pollfd poll_fd;

	for (;;) {
		int poll_ret;
		ssize_t received;

		poll_fd.fd = fd;
		poll_fd.events = POLLIN;
		poll_fd.revents = 0;
		poll_ret = poll(&poll_fd, 1, BEAU_VSOCK_CLOSE_TIMEOUT_MS);
		if (poll_ret < 0) {
			if (errno == EINTR) {
				continue;
			}
			return -errno;
		}
		if (poll_ret == 0) {
			return -ETIMEDOUT;
		}
		if ((poll_fd.revents & (POLLERR | POLLNVAL)) != 0) {
			return -EIO;
		}

		received = recv(fd, buffer, sizeof(buffer), 0);
		if (received < 0) {
			if (errno == EINTR) {
				continue;
			}
			return -errno;
		}
		if (received == 0) {
			return 0;
		}
		{
			int ret = write_all(fd, buffer, (size_t)received);

			if (ret != 0) {
				return ret;
			}
		}
	}
}

static int run_server(uint32_t port)
{
	struct sockaddr_vm address;
	int listener;

	listener = socket(AF_VSOCK, SOCK_STREAM, 0);
	if (listener < 0) {
		fprintf(stderr, "vsock server socket failed: %s\n", strerror(errno));
		return 1;
	}
	memset(&address, 0, sizeof(address));
	address.svm_family = AF_VSOCK;
	address.svm_cid = VMADDR_CID_ANY;
	address.svm_port = port;
	if (bind(listener, (struct sockaddr *)&address, sizeof(address)) != 0) {
		fprintf(stderr, "vsock bind port %" PRIu32 " failed: %s\n", port,
			strerror(errno));
		close(listener);
		return 1;
	}
	if (listen(listener, SOMAXCONN) != 0) {
		fprintf(stderr, "vsock listen port %" PRIu32 " failed: %s\n", port,
			strerror(errno));
		close(listener);
		return 1;
	}

	printf("vsock echo server: port %" PRIu32 "\n", port);
	for (;;) {
		int peer = accept(listener, NULL, NULL);
		int ret;

		if (peer < 0) {
			if (errno == EINTR) {
				continue;
			}
			fprintf(stderr, "vsock accept failed: %s\n", strerror(errno));
			close(listener);
			return 1;
		}
		ret = echo_connection(peer);
		if (ret != 0) {
			fprintf(stderr, "vsock client closed with error: %s\n", strerror(-ret));
		}
		close(peer);
	}
}

static int confirm_end_of_echo(int fd)
{
	struct pollfd poll_fd;
	uint8_t extra;
	int ret;

	poll_fd.fd = fd;
	poll_fd.events = POLLIN;
	poll_fd.revents = 0;
	ret = poll(&poll_fd, 1, BEAU_VSOCK_CLOSE_TIMEOUT_MS);
	if (ret < 0) {
		return -errno;
	}
	if (ret == 0) {
		return -ETIMEDOUT;
	}
	if ((poll_fd.revents & (POLLERR | POLLNVAL)) != 0) {
		return -EIO;
	}
	ret = (int)recv(fd, &extra, sizeof(extra), 0);
	if (ret < 0) {
		return -errno;
	}
	if (ret != 0) {
		return -EBADMSG;
	}
	return 0;
}

static int run_client(uint32_t cid, uint32_t port, const char *payload)
{
	struct sockaddr_vm address;
	static uint8_t reply[BEAU_VSOCK_PAYLOAD_MAX];
	size_t payload_length;
	int fd;
	int ret;

	payload_length = strlen(payload);
	if ((payload_length == 0U) || (payload_length > sizeof(reply))) {
		fprintf(stderr, "vsock payload must be 1 through %u bytes\n",
			BEAU_VSOCK_PAYLOAD_MAX);
		return 2;
	}

	fd = socket(AF_VSOCK, SOCK_STREAM, 0);
	if (fd < 0) {
		fprintf(stderr, "vsock client socket failed: %s\n", strerror(errno));
		return 1;
	}
	memset(&address, 0, sizeof(address));
	address.svm_family = AF_VSOCK;
	address.svm_cid = cid;
	address.svm_port = port;
	if (connect(fd, (struct sockaddr *)&address, sizeof(address)) != 0) {
		fprintf(stderr, "vsock connect cid=%" PRIu32 " port=%" PRIu32 " failed: %s\n",
			cid, port, strerror(errno));
		close(fd);
		return 1;
	}
	ret = write_all(fd, payload, payload_length);
	if ((ret == 0) && (shutdown(fd, SHUT_WR) != 0)) {
		ret = -errno;
	}
	if (ret == 0) {
		ret = read_exact(fd, reply, payload_length);
	}
	if ((ret == 0) && (memcmp(reply, payload, payload_length) != 0)) {
		ret = -EBADMSG;
	}
	if (ret == 0) {
		ret = confirm_end_of_echo(fd);
	}
	close(fd);
	if (ret != 0) {
		fprintf(stderr, "vsock echo verification failed: %s\n", strerror(-ret));
		return 1;
	}

	printf("vsock: cid=%" PRIu32 " port=%" PRIu32 " echo=%s\n", cid,
		port, payload);
	return 0;
}

int main(int argc, char *argv[])
{
	uint32_t cid;
	uint32_t port;

	if ((argc == 2) && (strcmp(argv[1], "cid") == 0)) {
		return print_local_cid();
	}
	if ((argc == 3) && (strcmp(argv[1], "server") == 0)) {
		if (parse_u32(argv[2], &port, false) != 0) {
			vsock_usage(argv[0]);
			return 2;
		}
		return run_server(port);
	}
	if ((argc == 5) && (strcmp(argv[1], "client") == 0)) {
		if ((parse_u32(argv[2], &cid, false) != 0) ||
			(parse_u32(argv[3], &port, false) != 0) ||
			(cid <= VMADDR_CID_HOST) || (cid == VMADDR_CID_ANY)) {
			vsock_usage(argv[0]);
			return 2;
		}
		return run_client(cid, port, argv[4]);
	}

	vsock_usage(argv[0]);
	return 2;
}
