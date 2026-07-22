// SPDX-License-Identifier: GPL-2.0-only
/*
 * BEAU static remoteproc transport.
 *
 * The remote VM is already running. Linux only attaches standard remoteproc
 * and virtio-rpmsg services to the shared resource table; no firmware or VM
 * lifecycle operation is exposed here.
 */

#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/overflow.h>
#include <linux/platform_device.h>
#include <linux/remoteproc.h>
#include <linux/sizes.h>
#include <linux/virtio_ids.h>
#include <linux/vringh.h>

#include "remoteproc_internal.h"

#define BEAU_RPROC_RSC_SIZE		SZ_4K
#define BEAU_RPROC_VRING_COUNT		2U
#define BEAU_RPROC_VRING_NUM_MAX	256U
#define BEAU_RPROC_VRING_ALIGN_MIN	SZ_4K

struct beau_static_rproc {
	struct device *dev;
	struct rproc *rproc;
	void __iomem *shared;
	void __iomem *doorbell;
	phys_addr_t shared_pa;
	size_t shared_size;
	int irq;
	bool irq_enabled;
	u32 irq_trace_count;
};

static bool beau_rproc_pow2(u32 value)
{
	return value && !(value & (value - 1));
}

static int beau_rproc_validate_rsc(struct beau_static_rproc *priv)
{
	struct resource_table *table = (struct resource_table __force *)priv->shared;
	struct fw_rsc_hdr *hdr;
	struct fw_rsc_vdev *vdev;
	u32 offset;
	u32 vring_num;
	u32 vring_align;
	unsigned int i;

	if (readl(&table->ver) != 1 || readl(&table->num) != 1 ||
		readl(&table->reserved[0]) || readl(&table->reserved[1]))
		return -EINVAL;

	offset = readl(&table->offset[0]);
	if (offset > BEAU_RPROC_RSC_SIZE - sizeof(*hdr) -
		    struct_size(vdev, vring, BEAU_RPROC_VRING_COUNT))
		return -EINVAL;
	hdr = (struct fw_rsc_hdr __force *)(priv->shared + offset);
	if (readl(&hdr->type) != RSC_VDEV)
		return -EINVAL;
	vdev = (struct fw_rsc_vdev __force *)(hdr + 1);
	if (readl(&vdev->id) != VIRTIO_ID_RPMSG ||
		vdev->num_of_vrings != BEAU_RPROC_VRING_COUNT ||
		vdev->reserved[0] || vdev->reserved[1])
		return -EINVAL;

	for (i = 0; i < BEAU_RPROC_VRING_COUNT; i++) {
		vring_num = readl(&vdev->vring[i].num);
		vring_align = readl(&vdev->vring[i].align);
		if (!beau_rproc_pow2(vring_num) || vring_num > BEAU_RPROC_VRING_NUM_MAX ||
			!beau_rproc_pow2(vring_align) || vring_align < BEAU_RPROC_VRING_ALIGN_MIN)
			return -EINVAL;
	}

	return 0;
}

static irqreturn_t beau_rproc_irq(int irq, void *data)
{
	struct beau_static_rproc *priv = data;
	irqreturn_t vq0;
	irqreturn_t vq1;

	/* EL2 validates each kick. The virtual interrupt carries no payload, so
	 * remoteproc checks both standard vrings and ignores empty ones.
	 */
	vq0 = rproc_vq_interrupt(priv->rproc, 0);
	vq1 = rproc_vq_interrupt(priv->rproc, 1);
	if (priv->irq_trace_count++ < 4)
		dev_info(priv->dev, "doorbell: vq0=%s vq1=%s\n",
			vq0 == IRQ_HANDLED ? "handled" : "empty",
			vq1 == IRQ_HANDLED ? "handled" : "empty");
	return vq0 == IRQ_HANDLED || vq1 == IRQ_HANDLED ? IRQ_HANDLED : IRQ_NONE;
}

static int beau_rproc_attach(struct rproc *rproc)
{
	struct beau_static_rproc *priv = rproc->priv;
	int ret;

	ret = beau_rproc_validate_rsc(priv);
	if (ret)
		return dev_err_probe(priv->dev, ret, "invalid shared resource table\n");
	if (!priv->irq_enabled) {
		enable_irq(priv->irq);
		priv->irq_enabled = true;
	}
	return 0;
}

static int beau_rproc_detach(struct rproc *rproc)
{
	struct beau_static_rproc *priv = rproc->priv;

	if (priv->irq_enabled) {
		disable_irq(priv->irq);
		priv->irq_enabled = false;
	}
	return 0;
}

static void beau_rproc_kick(struct rproc *rproc, int vqid)
{
	struct beau_static_rproc *priv = rproc->priv;

	if (vqid >= 0 && vqid < BEAU_RPROC_VRING_COUNT)
		writel((u32)vqid, priv->doorbell);
}

static void *beau_rproc_da_to_va(struct rproc *rproc, u64 da, size_t len,
				 bool *is_iomem)
{
	struct beau_static_rproc *priv = rproc->priv;
	u64 end;

	if (check_add_overflow(da, len, &end) || da < priv->shared_pa ||
		end > priv->shared_pa + priv->shared_size)
		return NULL;
	*is_iomem = true;
	return (void __force *)(priv->shared + (da - priv->shared_pa));
}

static struct resource_table *beau_rproc_get_loaded_rsc_table(struct rproc *rproc,
							      size_t *table_sz)
{
	struct beau_static_rproc *priv = rproc->priv;

	if (beau_rproc_validate_rsc(priv))
		return ERR_PTR(-EINVAL);
	*table_sz = BEAU_RPROC_RSC_SIZE;
	return (struct resource_table __force *)priv->shared;
}

static const struct rproc_ops beau_rproc_ops = {
	.attach = beau_rproc_attach,
	.detach = beau_rproc_detach,
	.kick = beau_rproc_kick,
	.da_to_va = beau_rproc_da_to_va,
	.get_loaded_rsc_table = beau_rproc_get_loaded_rsc_table,
};

static int beau_rproc_static_mem_alloc(struct rproc *rproc,
				       struct rproc_mem_entry *mem)
{
	/* EL2 already owns and maps this static region into both endpoints. */
	return 0;
}

static int beau_rproc_static_mem_release(struct rproc *rproc,
					  struct rproc_mem_entry *mem)
{
	return 0;
}

static int beau_rproc_add_carveout(struct beau_static_rproc *priv, void *va,
				   size_t offset, size_t len, const char *name)
{
	struct rproc_mem_entry *mem;

	mem = rproc_mem_entry_init(priv->dev, va, priv->shared_pa + offset, len,
		priv->shared_pa + offset, beau_rproc_static_mem_alloc,
		beau_rproc_static_mem_release, "%s", name);
	if (!mem)
		return -ENOMEM;
	rproc_add_carveout(priv->rproc, mem);
	return 0;
}

static int beau_rproc_add_static_carveouts(struct beau_static_rproc *priv)
{
	size_t ring_bytes = PAGE_ALIGN(vring_size(BEAU_RPROC_VRING_NUM_MAX,
		BEAU_RPROC_VRING_ALIGN_MIN));
	size_t offset = BEAU_RPROC_RSC_SIZE;
	int ret;

	if (ring_bytes > (priv->shared_size - offset) / BEAU_RPROC_VRING_COUNT)
		return -EINVAL;
	ret = beau_rproc_add_carveout(priv, (void __force *)(priv->shared + offset),
		offset, ring_bytes, "vdev0vring0");
	if (ret)
		return ret;
	offset += ring_bytes;
	ret = beau_rproc_add_carveout(priv, (void __force *)(priv->shared + offset),
		offset, ring_bytes, "vdev0vring1");
	if (ret)
		return ret;
	offset += ring_bytes;
	return beau_rproc_add_carveout(priv, NULL, offset, priv->shared_size - offset,
		"vdev0buffer");
}

static int beau_rproc_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct beau_static_rproc *priv;
	struct resource *shared_res;
	struct resource *doorbell_res;
	struct rproc *rproc;
	int ret;

	shared_res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "shared");
	doorbell_res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "doorbell");
	if (!shared_res || !doorbell_res || resource_size(shared_res) <= BEAU_RPROC_RSC_SIZE)
		return dev_err_probe(dev, -EINVAL, "invalid shared or doorbell resource\n");

	rproc = devm_rproc_alloc(dev, dev_name(dev), &beau_rproc_ops, NULL, sizeof(*priv));
	if (!rproc)
		return -ENOMEM;
	priv = rproc->priv;
	priv->dev = dev;
	priv->rproc = rproc;
	priv->shared_pa = shared_res->start;
	priv->shared_size = resource_size(shared_res);
	priv->shared = devm_ioremap_resource(dev, shared_res);
	if (IS_ERR(priv->shared))
		return PTR_ERR(priv->shared);
	priv->doorbell = devm_ioremap_resource(dev, doorbell_res);
	if (IS_ERR(priv->doorbell))
		return PTR_ERR(priv->doorbell);

	ret = beau_rproc_add_static_carveouts(priv);
	if (ret)
		return dev_err_probe(dev, ret, "cannot register static RPMsg carveouts\n");

	priv->irq = platform_get_irq(pdev, 0);
	if (priv->irq < 0)
		return priv->irq;
	/* RPMsg name-service may register a character device, which can sleep.
	 * Run virtqueue callbacks in the IRQ thread rather than hard-IRQ context.
	 */
	ret = devm_request_threaded_irq(dev, priv->irq, NULL, beau_rproc_irq,
		IRQF_NO_AUTOEN | IRQF_ONESHOT, dev_name(dev), priv);
	if (ret)
		return dev_err_probe(dev, ret, "cannot request doorbell irq\n");

	rproc->state = RPROC_DETACHED;
	rproc->auto_boot = false;
	ret = devm_rproc_add(dev, rproc);
	if (ret)
		return dev_err_probe(dev, ret, "rproc registration failed\n");

	return 0;
}

static const struct of_device_id beau_rproc_of_match[] = {
	{ .compatible = "beau,static-rproc" },
	{ }
};
MODULE_DEVICE_TABLE(of, beau_rproc_of_match);

static struct platform_driver beau_rproc_driver = {
	.probe = beau_rproc_probe,
	.driver = {
		.name = "beau-static-rproc",
		.of_match_table = beau_rproc_of_match,
	},
};
module_platform_driver(beau_rproc_driver);

MODULE_DESCRIPTION("BEAU static remoteproc RPMsg transport");
MODULE_LICENSE("GPL");
