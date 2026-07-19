/*
 * Copyright (C) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define EDU_VENDOR_ID		0x1234U
#define EDU_DEVICE_ID		0x11e8U
#define EDU_BAR_MAP_SIZE	0x1000UL
#define EDU_REG_ID		0x00U
#define EDU_REG_ALIVE		0x04U
#define EDU_REG_FACTORIAL	0x08U
#define EDU_REG_STATUS		0x20U
#define EDU_REG_IRQ_STATUS	0x24U
#define EDU_REG_IRQ_RAISE	0x60U
#define EDU_REG_IRQ_ACK		0x64U
#define EDU_STATUS_BUSY		0x01U
#define EDU_TEST_ALIVE		0x5a0f33c0U
#define EDU_TEST_IRQ		0x00000001U
#define PCI_COMMAND		0x04U
#define PCI_COMMAND_MEMORY	0x0002U
#define PCI_COMMAND_MASTER	0x0004U
#define PCI_STATUS		0x06U
#define PCI_STATUS_CAP_LIST	0x10U
#define PCI_CAP_PTR		0x34U
#define PCI_CAP_ID_MSI		0x05U
#define PCI_CAP_ID_MSIX		0x11U
#define PCI_CAP_NEXT_LIMIT	48U

struct pci_device {
	char bdf[64];
	char path[256];
};

static uint16_t le16_at(const uint8_t *buf, size_t off)
{
	return (uint16_t)buf[off] | ((uint16_t)buf[off + 1U] << 8U);
}

static void le16_put(uint8_t *buf, uint16_t value)
{
	buf[0] = (uint8_t)value;
	buf[1] = (uint8_t)(value >> 8U);
}

static uint32_t mmio_read32(const volatile uint8_t *bar, uint32_t off)
{
	return *(const volatile uint32_t *)(const volatile void *)(bar + off);
}

static void mmio_write32(volatile uint8_t *bar, uint32_t off, uint32_t value)
{
	*(volatile uint32_t *)(volatile void *)(bar + off) = value;
}

static int read_hex_file(const char *path, unsigned int *value)
{
	FILE *fp = fopen(path, "r");
	int ret;

	if (fp == NULL) {
		return -errno;
	}

	ret = fscanf(fp, "%x", value);
	fclose(fp);

	return (ret == 1) ? 0 : -EINVAL;
}

static int find_edu_device(struct pci_device *dev)
{
	const char *base = "/sys/bus/pci/devices";
	char cmd[512];
	FILE *fp;
	char bdf[64];

	snprintf(cmd, sizeof(cmd),
		"for d in %s/*; do "
		"[ -r \"$d/vendor\" ] || continue; "
		"[ \"$(cat \"$d/vendor\")\" = \"0x%04x\" ] || continue; "
		"[ \"$(cat \"$d/device\")\" = \"0x%04x\" ] || continue; "
		"basename \"$d\"; exit 0; "
		"done",
		base, EDU_VENDOR_ID, EDU_DEVICE_ID);

	fp = popen(cmd, "r");
	if (fp == NULL) {
		return -errno;
	}

	if (fgets(bdf, sizeof(bdf), fp) == NULL) {
		(void)pclose(fp);
		return -ENODEV;
	}
	(void)pclose(fp);

	bdf[strcspn(bdf, "\r\n")] = '\0';
	if (bdf[0] == '\0') {
		return -ENODEV;
	}

	snprintf(dev->bdf, sizeof(dev->bdf), "%s", bdf);
	snprintf(dev->path, sizeof(dev->path), "%s/%s", base, bdf);
	return 0;
}

static int read_resource0(const struct pci_device *dev, uint64_t *start,
	uint64_t *end, uint64_t *flags)
{
	char path[320];
	FILE *fp;
	int ret;

	snprintf(path, sizeof(path), "%s/resource", dev->path);
	fp = fopen(path, "r");
	if (fp == NULL) {
		return -errno;
	}

	ret = fscanf(fp, "%" SCNx64 " %" SCNx64 " %" SCNx64, start, end, flags);
	fclose(fp);

	return (ret == 3) ? 0 : -EINVAL;
}

static int show_config_caps(const struct pci_device *dev)
{
	char path[320];
	uint8_t cfg[256];
	int fd;
	ssize_t got;
	uint16_t status;
	uint8_t cap;
	uint32_t guard;
	bool found_msi = false;
	bool found_msix = false;

	snprintf(path, sizeof(path), "%s/config", dev->path);
	fd = open(path, O_RDONLY);
	if (fd < 0) {
		printf("config: unavailable: %s\n", strerror(errno));
		return -errno;
	}

	got = pread(fd, cfg, sizeof(cfg), 0);
	close(fd);
	if (got < (ssize_t)sizeof(cfg)) {
		printf("config: short read: %zd\n", got);
		return -EIO;
	}

	status = le16_at(cfg, PCI_STATUS);
	if ((status & PCI_STATUS_CAP_LIST) == 0U) {
		printf("cap: none\n");
		return 0;
	}

	cap = cfg[PCI_CAP_PTR] & 0xfcU;
	for (guard = 0U; (cap >= 0x40U) && (guard < PCI_CAP_NEXT_LIMIT); guard++) {
		uint8_t id = cfg[cap];
		uint8_t next = cfg[cap + 1U] & 0xfcU;

		if (id == PCI_CAP_ID_MSI) {
			uint16_t ctrl = le16_at(cfg, cap + 2U);

			found_msi = true;
			printf("cap: MSI  off:0x%02x enabled:%s vectors:%u 64bit:%s\n",
				cap, (ctrl & 0x1U) ? "Y" : "N",
				1U << ((ctrl >> 1U) & 0x7U),
				(ctrl & (1U << 7U)) ? "Y" : "N");
		} else if (id == PCI_CAP_ID_MSIX) {
			uint16_t ctrl = le16_at(cfg, cap + 2U);

			found_msix = true;
			printf("cap: MSI-X off:0x%02x enabled:%s masked:%s vectors:%u\n",
				cap, (ctrl & (1U << 15U)) ? "Y" : "N",
				(ctrl & (1U << 14U)) ? "Y" : "N",
				(ctrl & 0x7ffU) + 1U);
		}

		if ((next == 0U) || (next == cap)) {
			break;
		}
		cap = next;
	}

	if (!found_msi && !found_msix) {
		printf("cap: no MSI/MSI-X capability found\n");
	}

	return 0;
}

static int config_read16(const struct pci_device *dev, uint32_t offset, uint16_t *value)
{
	char path[320];
	uint8_t buf[2];
	int fd;
	ssize_t got;

	snprintf(path, sizeof(path), "%s/config", dev->path);
	fd = open(path, O_RDONLY);
	if (fd < 0) {
		return -errno;
	}

	got = pread(fd, buf, sizeof(buf), offset);
	close(fd);
	if (got != (ssize_t)sizeof(buf)) {
		return -EIO;
	}

	*value = le16_at(buf, 0U);
	return 0;
}

static int config_write16(const struct pci_device *dev, uint32_t offset, uint16_t value)
{
	char path[320];
	uint8_t buf[2];
	int fd;
	ssize_t wrote;

	snprintf(path, sizeof(path), "%s/config", dev->path);
	fd = open(path, O_RDWR);
	if (fd < 0) {
		return -errno;
	}

	le16_put(buf, value);
	wrote = pwrite(fd, buf, sizeof(buf), offset);
	close(fd);

	return (wrote == (ssize_t)sizeof(buf)) ? 0 : -EIO;
}

static int enable_pci_command_bits(const struct pci_device *dev)
{
	uint16_t command = 0U;
	uint16_t new_command;
	int ret;

	ret = config_read16(dev, PCI_COMMAND, &command);
	if (ret != 0) {
		printf("command: read failed: %s\n", strerror(-ret));
		return ret;
	}

	new_command = command | PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER;
	if (new_command != command) {
		ret = config_write16(dev, PCI_COMMAND, new_command);
		if (ret != 0) {
			printf("command: write 0x%04x failed: %s\n",
				new_command, strerror(-ret));
			return ret;
		}
		printf("command: 0x%04x -> 0x%04x (memory/master enabled)\n",
			command, new_command);
	} else {
		printf("command: 0x%04x (memory/master already enabled)\n", command);
	}

	return 0;
}

static void show_bound_driver(const struct pci_device *dev)
{
	char path[320];
	char link[320];
	ssize_t len;

	snprintf(path, sizeof(path), "%s/driver", dev->path);
	len = readlink(path, link, sizeof(link) - 1U);
	if (len < 0) {
		printf("driver: none\n");
		return;
	}

	link[len] = '\0';
	printf("driver: %s\n", strrchr(link, '/') != NULL ? strrchr(link, '/') + 1 : link);
}

static int run_bar_test(const struct pci_device *dev)
{
	char path[320];
	uint64_t start = 0U;
	uint64_t end = 0U;
	uint64_t flags = 0U;
	int fd;
	volatile uint8_t *bar;
	uint32_t id;
	uint32_t alive;
	uint32_t factorial;
	uint32_t irq_status;
	uint32_t i;
	int ret;

	ret = read_resource0(dev, &start, &end, &flags);
	if (ret != 0) {
		printf("resource0: unavailable: %s\n", strerror(-ret));
		return ret;
	}
	printf("resource0: [0x%016" PRIx64 "-0x%016" PRIx64 "] flags:0x%016" PRIx64 "\n",
		start, end, flags);

	snprintf(path, sizeof(path), "%s/resource0", dev->path);
	fd = open(path, O_RDWR | O_SYNC);
	if (fd < 0) {
		printf("resource0 mmap open failed: %s\n", strerror(errno));
		return -errno;
	}

	bar = mmap(NULL, EDU_BAR_MAP_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	close(fd);
	if (bar == MAP_FAILED) {
		printf("resource0 mmap failed: %s\n", strerror(errno));
		return -errno;
	}

	id = mmio_read32(bar, EDU_REG_ID);
	printf("mmio: id=0x%08x\n", id);

	mmio_write32(bar, EDU_REG_ALIVE, EDU_TEST_ALIVE);
	alive = mmio_read32(bar, EDU_REG_ALIVE);
	printf("mmio: alive write=0x%08x read=0x%08x expect=0x%08x %s\n",
		EDU_TEST_ALIVE, alive, ~EDU_TEST_ALIVE,
		(alive == ~EDU_TEST_ALIVE) ? "PASS" : "FAIL");
	if (alive != ~EDU_TEST_ALIVE) {
		(void)munmap((void *)bar, EDU_BAR_MAP_SIZE);
		return -EIO;
	}

	mmio_write32(bar, EDU_REG_FACTORIAL, 5U);
	for (i = 0U; i < 100000U; i++) {
		if ((mmio_read32(bar, EDU_REG_STATUS) & EDU_STATUS_BUSY) == 0U) {
			break;
		}
	}
	factorial = mmio_read32(bar, EDU_REG_FACTORIAL);
	printf("mmio: factorial(5)=%u %s\n", factorial,
		(factorial == 120U) ? "PASS" : "WARN");

	irq_status = mmio_read32(bar, EDU_REG_IRQ_STATUS);
	if (irq_status != 0U) {
		mmio_write32(bar, EDU_REG_IRQ_ACK, irq_status);
	}
	mmio_write32(bar, EDU_REG_IRQ_RAISE, EDU_TEST_IRQ);
	irq_status = mmio_read32(bar, EDU_REG_IRQ_STATUS);
	printf("mmio: irq-status after raise=0x%08x\n", irq_status);
	mmio_write32(bar, EDU_REG_IRQ_ACK, EDU_TEST_IRQ);
	printf("mmio: irq-status after ack=0x%08x\n",
		mmio_read32(bar, EDU_REG_IRQ_STATUS));

	(void)munmap((void *)bar, EDU_BAR_MAP_SIZE);
	return 0;
}

int main(void)
{
	struct pci_device dev;
	char path[320];
	unsigned int vendor = 0U;
	unsigned int device = 0U;
	int ret;

	ret = find_edu_device(&dev);
	if (ret != 0) {
		printf("edu device 0x%04x:0x%04x not found\n", EDU_VENDOR_ID, EDU_DEVICE_ID);
		return 2;
	}

	snprintf(path, sizeof(path), "%s/vendor", dev.path);
	(void)read_hex_file(path, &vendor);
	snprintf(path, sizeof(path), "%s/device", dev.path);
	(void)read_hex_file(path, &device);

	printf("edu: %s vendor:0x%04x device:0x%04x\n", dev.bdf, vendor, device);
	show_bound_driver(&dev);
	(void)show_config_caps(&dev);

	ret = enable_pci_command_bits(&dev);
	if (ret != 0) {
		return 1;
	}

	ret = run_bar_test(&dev);
	return (ret == 0) ? 0 : 1;
}
