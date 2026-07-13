// SPDX-License-Identifier: BSD-3-Clause
/*
 * BEAU QEMU edu PCI passthrough validation.
 */

#include <linux/completion.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/jiffies.h>
#include <linux/pci.h>
#include <linux/slab.h>

#define BEAU_EDU_VENDOR_ID	0x1234
#define BEAU_EDU_DEVICE_ID	0x11e8
#define BEAU_EDU_IRQ_STATUS	0x24
#define BEAU_EDU_IRQ_RAISE	0x60
#define BEAU_EDU_IRQ_ACK	0x64
#define BEAU_EDU_IRQ_TEST	0x1

struct beau_edu_test {
	struct pci_dev *pdev;
	void __iomem *bar;
	struct completion irq_done;
};

static irqreturn_t beau_edu_irq(int irq, void *data)
{
	struct beau_edu_test *edu = data;
	u32 status;

	status = ioread32(edu->bar + BEAU_EDU_IRQ_STATUS);
	if ((status & BEAU_EDU_IRQ_TEST) == 0)
		return IRQ_NONE;

	iowrite32(status, edu->bar + BEAU_EDU_IRQ_ACK);
	complete(&edu->irq_done);

	return IRQ_HANDLED;
}

static int beau_edu_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
	struct beau_edu_test *edu;
	int irq;
	int ret;

	edu = kzalloc(sizeof(*edu), GFP_KERNEL);
	if (!edu)
		return -ENOMEM;

	edu->pdev = pdev;
	pci_set_drvdata(pdev, edu);

	ret = pci_enable_device_mem(pdev);
	if (ret) {
		dev_warn(&pdev->dev, "BEAU edu: enable MMIO failed: %d\n", ret);
		goto err_free;
	}

	edu->bar = pci_iomap(pdev, 0, 0);
	if (!edu->bar) {
		dev_warn(&pdev->dev, "BEAU edu: BAR0 map failed\n");
		ret = -ENOMEM;
		goto err_disable;
	}

	pci_set_master(pdev);

	ret = pci_alloc_irq_vectors(pdev, 1, 1, PCI_IRQ_MSI | PCI_IRQ_MSIX);
	if (ret < 0) {
		dev_warn(&pdev->dev, "BEAU edu: MSI/MSI-X alloc failed: %d\n", ret);
		goto out_keep_mmio;
	}

	irq = pci_irq_vector(pdev, 0);
	init_completion(&edu->irq_done);

	ret = request_irq(irq, beau_edu_irq, 0, "beau-edu-test", edu);
	if (ret) {
		dev_warn(&pdev->dev, "BEAU edu: request irq %d failed: %d\n", irq, ret);
		goto out_free_vectors;
	}

	iowrite32(ioread32(edu->bar + BEAU_EDU_IRQ_STATUS), edu->bar + BEAU_EDU_IRQ_ACK);
	iowrite32(BEAU_EDU_IRQ_TEST, edu->bar + BEAU_EDU_IRQ_RAISE);

	if (wait_for_completion_timeout(&edu->irq_done, msecs_to_jiffies(1000)) == 0)
		dev_warn(&pdev->dev, "BEAU edu: MSI/MSI-X interrupt timeout\n");
	else
		dev_info(&pdev->dev, "BEAU edu: MSI/MSI-X interrupt observed\n");

	free_irq(irq, edu);
out_free_vectors:
	pci_free_irq_vectors(pdev);
out_keep_mmio:
	pci_clear_master(pdev);
	return 0;

err_disable:
	pci_disable_device(pdev);
err_free:
	pci_set_drvdata(pdev, NULL);
	kfree(edu);

	return ret;
}

static void beau_edu_remove(struct pci_dev *pdev)
{
	struct beau_edu_test *edu = pci_get_drvdata(pdev);

	if (!edu)
		return;

	if (edu->bar)
		pci_iounmap(pdev, edu->bar);
	pci_disable_device(pdev);
	pci_set_drvdata(pdev, NULL);
	kfree(edu);
}

static const struct pci_device_id beau_edu_ids[] = {
	{ PCI_DEVICE(BEAU_EDU_VENDOR_ID, BEAU_EDU_DEVICE_ID) },
	{ }
};

static struct pci_driver beau_edu_driver = {
	.name = "beau-edu-test",
	.id_table = beau_edu_ids,
	.probe = beau_edu_probe,
	.remove = beau_edu_remove,
};

builtin_pci_driver(beau_edu_driver);
