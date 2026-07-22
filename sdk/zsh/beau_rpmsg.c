/*
 * Copyright (c) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/cache.h>
#include <zephyr/irq.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/barrier.h>
#include <zephyr/sys/device_mmio.h>
#include <zephyr/sys/sys_io.h>
#include <zephyr/sys/util.h>
#include <zephyr/sys/printk.h>

#include <string.h>

#include <openamp/open_amp.h>

LOG_MODULE_REGISTER(beau_rpmsg, LOG_LEVEL_INF);

#define BEAU_RPMSG_NODE		DT_NODELABEL(beau_rpmsg)
#define BEAU_RPMSG_SHARED	DT_REG_ADDR_BY_IDX(BEAU_RPMSG_NODE, 0)
#define BEAU_RPMSG_SHARED_SIZE	DT_REG_SIZE_BY_IDX(BEAU_RPMSG_NODE, 0)
#define BEAU_RPMSG_DOORBELL	DT_REG_ADDR_BY_IDX(BEAU_RPMSG_NODE, 1)
#define BEAU_RPMSG_VRING_NUM	256U
#define BEAU_RPMSG_VRING_ALIGN	4096U
#define BEAU_RPMSG_VRING_COUNT	2U
#define BEAU_RPMSG_RSC_SIZE	4096U
/* PAGE_ALIGN(vring_size(256, 4096)) shared with beau_static_rproc. */
#define BEAU_RPMSG_VRING_SIZE	(3U * BEAU_RPMSG_VRING_ALIGN)
#define BEAU_RPMSG_VRING0_DA	(BEAU_RPMSG_SHARED + BEAU_RPMSG_RSC_SIZE)
#define BEAU_RPMSG_VRING1_DA	(BEAU_RPMSG_VRING0_DA + BEAU_RPMSG_VRING_SIZE)
#define BEAU_RPMSG_RSC_VDEV	3U
#define BEAU_RPMSG_VIRTIO_ID	7U
#define BEAU_RPMSG_F_NS		0U
#define BEAU_RPMSG_STATUS_DRIVER_OK	4U

struct beau_rsc_vring {
	uint32_t da;
	uint32_t align;
	uint32_t num;
	uint32_t notifyid;
	uint32_t pa;
} __packed;

struct beau_rsc_vdev {
	uint32_t type;
	uint32_t id;
	uint32_t notifyid;
	uint32_t dfeatures;
	uint32_t gfeatures;
	uint32_t config_len;
	uint8_t status;
	uint8_t num_of_vrings;
	uint8_t reserved[2];
	struct beau_rsc_vring vring[BEAU_RPMSG_VRING_COUNT];
} __packed;

struct beau_rsc_table {
	uint32_t ver;
	uint32_t num;
	uint32_t reserved[2];
	uint32_t offset[1];
	struct beau_rsc_vdev vdev;
} __packed;

static mm_reg_t beau_rpmsg_map;
static mm_reg_t beau_rpmsg_doorbell_map;
static metal_phys_addr_t beau_rpmsg_physmap[] = { BEAU_RPMSG_SHARED };
static struct metal_io_region beau_rpmsg_io;
static struct virtio_vring_info beau_rpmsg_rvrings[BEAU_RPMSG_VRING_COUNT];
static struct virtqueue *beau_rpmsg_vq[BEAU_RPMSG_VRING_COUNT];
static struct virtio_device beau_rpmsg_vdev;
static struct rpmsg_virtio_device beau_rpmsg_rvdev;
static struct rpmsg_endpoint beau_rpmsg_ept;
static atomic_t beau_rpmsg_ready;
static atomic_t beau_rpmsg_wait_loops;
static atomic_t beau_rpmsg_notify_count;
static atomic_t beau_rpmsg_echo_count;

static struct beau_rsc_table *beau_rpmsg_table(void)
{
	return (struct beau_rsc_table *)beau_rpmsg_map;
}

static unsigned char beau_rpmsg_get_status(struct virtio_device *vdev)
{
	ARG_UNUSED(vdev);
	return beau_rpmsg_table()->vdev.status;
}

static uint32_t beau_rpmsg_get_features(struct virtio_device *vdev)
{
	ARG_UNUSED(vdev);
	return BIT(BEAU_RPMSG_F_NS);
}

static void beau_rpmsg_notify(struct virtqueue *vq)
{
	if (atomic_inc(&beau_rpmsg_notify_count) < 4) {
		LOG_INF("notify host: vq=%u notifyid=%u", vq->vq_queue_index,
			beau_rpmsg_rvrings[vq->vq_queue_index].notifyid);
	}
	barrier_dmem_fence_full();
	/* EL2 routes a validated kick to VM3. Its Linux ISR scans both vrings. */
	sys_write32(0U, beau_rpmsg_doorbell_map);
}

static struct virtio_dispatch beau_rpmsg_dispatch = {
	.get_status = beau_rpmsg_get_status,
	.get_features = beau_rpmsg_get_features,
	.notify = beau_rpmsg_notify,
};

static void beau_rpmsg_irq(const void *arg)
{
	ARG_UNUSED(arg);
	if (atomic_get(&beau_rpmsg_ready) == 0) {
		return;
	}
#if defined(CONFIG_CACHE_MANAGEMENT)
	sys_cache_data_invd_range((void *)beau_rpmsg_map, BEAU_RPMSG_SHARED_SIZE);
#endif
	virtqueue_notification(beau_rpmsg_vq[0]);
	virtqueue_notification(beau_rpmsg_vq[1]);
}

static int beau_rpmsg_echo(struct rpmsg_endpoint *ept, void *data,
			   size_t len, uint32_t src, void *priv)
{
	int ret;

	ARG_UNUSED(priv);
	ret = rpmsg_send_offchannel(ept, ept->addr, src, data, (int)len);
	if (atomic_inc(&beau_rpmsg_echo_count) < 4) {
		LOG_INF("echo: src=0x%08x len=%u ret=%d", src, (uint32_t)len, ret);
	}
	return ret;
}

static void beau_rpmsg_unbind(struct rpmsg_endpoint *ept)
{
	rpmsg_destroy_ept(ept);
}

static void beau_rpmsg_publish_table(void)
{
	struct beau_rsc_table *table = beau_rpmsg_table();

	memset(table, 0, sizeof(*table));
	table->ver = 1U;
	table->num = 1U;
	table->offset[0] = offsetof(struct beau_rsc_table, vdev);
	table->vdev.type = BEAU_RPMSG_RSC_VDEV;
	table->vdev.id = BEAU_RPMSG_VIRTIO_ID;
	table->vdev.dfeatures = BIT(BEAU_RPMSG_F_NS);
	table->vdev.num_of_vrings = BEAU_RPMSG_VRING_COUNT;
	table->vdev.vring[0].da = BEAU_RPMSG_VRING0_DA;
	table->vdev.vring[1].da = BEAU_RPMSG_VRING1_DA;
	for (uint32_t i = 0U; i < BEAU_RPMSG_VRING_COUNT; i++) {
		table->vdev.vring[i].align = BEAU_RPMSG_VRING_ALIGN;
		table->vdev.vring[i].num = BEAU_RPMSG_VRING_NUM;
		table->vdev.vring[i].notifyid = i;
	}
	barrier_dmem_fence_full();
#if defined(CONFIG_CACHE_MANAGEMENT)
	sys_cache_data_flush_range((void *)beau_rpmsg_map, BEAU_RPMSG_RSC_SIZE);
#endif
	LOG_INF("resource table published: vring0=0x%08x vring1=0x%08x",
		BEAU_RPMSG_VRING0_DA, BEAU_RPMSG_VRING1_DA);
}

static bool beau_rpmsg_attached(void)
{
	const struct beau_rsc_vdev *vdev = &beau_rpmsg_table()->vdev;

#if defined(CONFIG_CACHE_MANAGEMENT)
	sys_cache_data_invd_range((void *)beau_rpmsg_map, BEAU_RPMSG_RSC_SIZE);
#endif
	barrier_dmem_fence_full();
	return (vdev->status & BEAU_RPMSG_STATUS_DRIVER_OK) != 0U;
}

static int beau_rpmsg_start(void)
{
	struct beau_rsc_table *table = beau_rpmsg_table();
	struct rpmsg_device *rdev;
	struct metal_init_params metal_params = METAL_INIT_DEFAULTS;
	int ret;

	ret = metal_init(&metal_params);
	if (ret != 0) {
		return ret;
	}
	metal_io_init(&beau_rpmsg_io, (void *)beau_rpmsg_map,
		beau_rpmsg_physmap, BEAU_RPMSG_SHARED_SIZE, -1, 0, NULL);
	for (uint32_t i = 0U; i < BEAU_RPMSG_VRING_COUNT; i++) {
		beau_rpmsg_vq[i] = virtqueue_allocate(table->vdev.vring[i].num);
		if (beau_rpmsg_vq[i] == NULL) {
			return -ENOMEM;
		}
		beau_rpmsg_rvrings[i].io = &beau_rpmsg_io;
		beau_rpmsg_rvrings[i].notifyid = table->vdev.vring[i].notifyid;
		beau_rpmsg_rvrings[i].info.vaddr = (void *)(beau_rpmsg_map +
			(table->vdev.vring[i].da - BEAU_RPMSG_SHARED));
		beau_rpmsg_rvrings[i].info.num_descs = table->vdev.vring[i].num;
		beau_rpmsg_rvrings[i].info.align = table->vdev.vring[i].align;
		beau_rpmsg_rvrings[i].vq = beau_rpmsg_vq[i];
	}
	beau_rpmsg_vdev.role = RPMSG_REMOTE;
	beau_rpmsg_vdev.vrings_num = BEAU_RPMSG_VRING_COUNT;
	beau_rpmsg_vdev.func = &beau_rpmsg_dispatch;
	beau_rpmsg_vdev.vrings_info = &beau_rpmsg_rvrings[0];
	ret = rpmsg_init_vdev(&beau_rpmsg_rvdev, &beau_rpmsg_vdev, NULL,
		&beau_rpmsg_io, NULL);
	if (ret != 0) {
		return ret;
	}
	rdev = rpmsg_virtio_get_rpmsg_device(&beau_rpmsg_rvdev);
	ret = rpmsg_create_ept(&beau_rpmsg_ept, rdev, "rpmsg-raw", RPMSG_ADDR_ANY,
		RPMSG_ADDR_ANY, beau_rpmsg_echo, beau_rpmsg_unbind);
	if (ret == 0) {
		atomic_set(&beau_rpmsg_ready, 1);
		LOG_INF("rpmsg service rpmsg-raw announced");
	}
	return ret;
}

static void beau_rpmsg_thread(void *a, void *b, void *c)
{
	struct beau_rsc_table *table;
	int ret;

	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);
	/* OpenAMP accesses rings and buffers directly; retain a single coherent
	 * transport view across VM0 and VM3 without platform-specific cache hooks.
	 */
	device_map(&beau_rpmsg_map, BEAU_RPMSG_SHARED, BEAU_RPMSG_SHARED_SIZE,
		K_MEM_CACHE_NONE);
	device_map(&beau_rpmsg_doorbell_map, BEAU_RPMSG_DOORBELL, sizeof(uint32_t),
		K_MEM_CACHE_NONE);
	table = beau_rpmsg_table();
	beau_rpmsg_publish_table();
	while (!beau_rpmsg_attached()) {
		if (atomic_inc(&beau_rpmsg_wait_loops) % 100 == 0) {
			LOG_INF("waiting host: status=0x%02x vring0=0x%08x vring1=0x%08x",
				table->vdev.status, table->vdev.vring[0].da,
				table->vdev.vring[1].da);
		}
		k_sleep(K_MSEC(20));
	}
	LOG_INF("host attached: status=0x%02x", table->vdev.status);
	ret = beau_rpmsg_start();
	if (ret != 0) {
		LOG_ERR("OpenAMP start failed: %d", ret);
	} else {
		LOG_INF("OpenAMP vdev ready");
	}
}

K_THREAD_DEFINE(beau_rpmsg_tid, 2048, beau_rpmsg_thread, NULL, NULL, NULL,
		K_PRIO_COOP(8), 0, 0);

static int beau_rpmsg_irq_init(void)
{
	IRQ_CONNECT(DT_IRQN(BEAU_RPMSG_NODE), DT_IRQ(BEAU_RPMSG_NODE, priority),
		beau_rpmsg_irq, NULL, 0);
	irq_enable(DT_IRQN(BEAU_RPMSG_NODE));
	return 0;
}

SYS_INIT(beau_rpmsg_irq_init, POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT);
