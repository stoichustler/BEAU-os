// SPDX-License-Identifier: GPL-2.0-only
/* BEAU coordinated guest suspend-to-RAM agent. */

#include <linux/arm-smccc.h>
#include <linux/atomic.h>
#include <linux/fs.h>
#include <linux/interrupt.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/poll.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <asm/memory.h>

#include "beau-pm.h"

#define BEAU_HC_ID(x, y)		(((x) << 24) | (y))
#define BEAU_HC_CLASS			0x80UL
#define BEAU_HC_PM_BASE			0x80UL
#define HC_PM_CONTROL			BEAU_HC_ID(BEAU_HC_CLASS, BEAU_HC_PM_BASE + 0x01UL)

struct beau_pm_device {
	struct device *dev;
	struct miscdevice miscdev;
	wait_queue_head_t event_waitq;
	struct mutex hcall_lock;
	atomic_t event_pending;
	struct acrn_pm_ioc ioc __aligned(64);
	u64 pending_epoch;
	u64 last_epoch;
	u16 vmid;
	bool awaiting_resume;
};

static long beau_pm_hcall(struct acrn_pm_ioc *ioc)
{
	struct arm_smccc_res res;

	arm_smccc_1_1_hvc(HC_PM_CONTROL, virt_to_phys(ioc), &res);
	return res.a0;
}

static int beau_pm_call_locked(struct beau_pm_device *pm, u32 op, u64 epoch)
{
	long ret;

	memset(&pm->ioc, 0, sizeof(pm->ioc));
	pm->ioc.abi_version = ACRN_PM_ABI_VERSION;
	pm->ioc.ioc_size = sizeof(pm->ioc);
	pm->ioc.op = op;
	pm->ioc.epoch = epoch;
	pm->ioc.vmid = pm->vmid;
	ret = beau_pm_hcall(&pm->ioc);
	if (ret)
		return ret;

	return pm->ioc.status;
}

static irqreturn_t beau_pm_irq(int irq, void *data)
{
	struct beau_pm_device *pm = data;

	atomic_set(&pm->event_pending, 1);
	wake_up_interruptible(&pm->event_waitq);
	return IRQ_HANDLED;
}

static int beau_pm_wait_event(struct beau_pm_device *pm, bool nonblock)
{
	int ret;

	for (;;) {
		if (!atomic_read(&pm->event_pending)) {
			if (nonblock)
				return -EAGAIN;
			ret = wait_event_interruptible(pm->event_waitq,
				atomic_read(&pm->event_pending));
			if (ret)
				return ret;
		}

		mutex_lock(&pm->hcall_lock);
		if (!atomic_xchg(&pm->event_pending, 0)) {
			mutex_unlock(&pm->hcall_lock);
			continue;
		}
		ret = beau_pm_call_locked(pm, ACRN_PM_GET_EVENT, 0);
		if (!ret && (pm->ioc.flags & ACRN_PM_EVENT_PREPARE) &&
		    (pm->ioc.flags & ACRN_PM_FLAG_REQUIRED) &&
		    pm->ioc.epoch > pm->last_epoch) {
			pm->pending_epoch = pm->ioc.epoch;
			pm->awaiting_resume = true;
			mutex_unlock(&pm->hcall_lock);
			return 0;
		}
		mutex_unlock(&pm->hcall_lock);
		if (ret)
			return ret;
	}
}

static ssize_t beau_pm_read(struct file *file, char __user *buffer,
			    size_t count, loff_t *ppos)
{
	struct miscdevice *miscdev = file->private_data;
	struct beau_pm_device *pm = container_of(miscdev,
		struct beau_pm_device, miscdev);
	char event[32];
	int len;
	int ret;

	if (*ppos != 0)
		return 0;
	ret = beau_pm_wait_event(pm, file->f_flags & O_NONBLOCK);
	if (ret)
		return ret;

	len = scnprintf(event, sizeof(event), "%llu\n", pm->pending_epoch);
	return simple_read_from_buffer(buffer, count, ppos, event, len);
}

static ssize_t beau_pm_write(struct file *file, const char __user *buffer,
			     size_t count, loff_t *ppos)
{
	struct miscdevice *miscdev = file->private_data;
	struct beau_pm_device *pm = container_of(miscdev,
		struct beau_pm_device, miscdev);
	char value[32];
	u64 epoch;
	u64 wake_reason;
	int ret;

	if (!count || count >= sizeof(value))
		return -EINVAL;
	if (copy_from_user(value, buffer, count))
		return -EFAULT;
	value[count] = '\0';
	ret = kstrtoull(strim(value), 0, &epoch);
	if (ret)
		return ret;

	mutex_lock(&pm->hcall_lock);
	if (!pm->awaiting_resume || epoch != pm->pending_epoch ||
	    epoch <= pm->last_epoch) {
		ret = -ESTALE;
		goto out_unlock;
	}
	ret = beau_pm_call_locked(pm, ACRN_PM_GET_WAKE_REASON, epoch);
	if (ret)
		goto out_unlock;
	wake_reason = pm->ioc.wake_reason;
	ret = beau_pm_call_locked(pm, ACRN_PM_RESUME_COMPLETE, epoch);
	if (ret)
		goto out_unlock;

	pm->last_epoch = epoch;
	pm->pending_epoch = 0;
	pm->awaiting_resume = false;
	dev_info(pm->dev, "resume complete epoch:%llu wake:%llu\n",
		 epoch, wake_reason);

out_unlock:
	mutex_unlock(&pm->hcall_lock);
	return ret ? ret : count;
}

static __poll_t beau_pm_poll(struct file *file, poll_table *wait)
{
	struct miscdevice *miscdev = file->private_data;
	struct beau_pm_device *pm = container_of(miscdev,
		struct beau_pm_device, miscdev);

	poll_wait(file, &pm->event_waitq, wait);
	return atomic_read(&pm->event_pending) ? EPOLLIN | EPOLLRDNORM : 0;
}

static const struct file_operations beau_pm_fops = {
	.owner = THIS_MODULE,
	.read = beau_pm_read,
	.write = beau_pm_write,
	.poll = beau_pm_poll,
	.llseek = noop_llseek,
};

static int beau_pm_probe(struct platform_device *pdev)
{
	struct beau_pm_device *pm;
	int irq;
	int ret;

	pm = devm_kzalloc(&pdev->dev, sizeof(*pm), GFP_KERNEL);
	if (!pm)
		return -ENOMEM;
	pm->dev = &pdev->dev;
	pm->vmid = ACRN_INVALID_VMID;
	init_waitqueue_head(&pm->event_waitq);
	mutex_init(&pm->hcall_lock);
	atomic_set(&pm->event_pending, 0);

	mutex_lock(&pm->hcall_lock);
	ret = beau_pm_call_locked(pm, ACRN_PM_QUERY_CAPS, 0);
	if (!ret && !(pm->ioc.flags & ACRN_PM_CAP_SYSTEM_SUSPEND))
		ret = -EOPNOTSUPP;
	if (!ret)
		pm->vmid = pm->ioc.vmid;
	mutex_unlock(&pm->hcall_lock);
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
			"failed to negotiate PM ABI\n");

	irq = platform_get_irq(pdev, 0);
	if (irq < 0)
		return irq;
	if (pm->ioc.event_virq == 0)
		return dev_err_probe(&pdev->dev, -EINVAL,
			"PM ABI returned no event VIRQ\n");
	ret = devm_request_irq(&pdev->dev, irq, beau_pm_irq, IRQF_NO_SUSPEND,
		dev_name(&pdev->dev), pm);
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
			"failed to request PM event IRQ\n");

	pm->miscdev.minor = MISC_DYNAMIC_MINOR;
	pm->miscdev.name = "beau-pm";
	pm->miscdev.fops = &beau_pm_fops;
	pm->miscdev.parent = &pdev->dev;
	ret = misc_register(&pm->miscdev);
	if (ret)
		return dev_err_probe(&pdev->dev, ret,
			"failed to register PM event device\n");

	platform_set_drvdata(pdev, pm);
	dev_info(&pdev->dev,
		 "coordinated PM agent ready vm:%u irq:%d event-virq:%u\n",
		 pm->vmid, irq, pm->ioc.event_virq);
	return 0;
}

static void beau_pm_remove(struct platform_device *pdev)
{
	struct beau_pm_device *pm = platform_get_drvdata(pdev);

	misc_deregister(&pm->miscdev);
}

static const struct of_device_id beau_pm_of_match[] = {
	{ .compatible = "beau,pm" },
	{ }
};
MODULE_DEVICE_TABLE(of, beau_pm_of_match);

static struct platform_driver beau_pm_driver = {
	.probe = beau_pm_probe,
	.remove = beau_pm_remove,
	.driver = {
		.name = "beau-pm",
		.of_match_table = beau_pm_of_match,
	},
};
module_platform_driver(beau_pm_driver);

MODULE_DESCRIPTION("BEAU coordinated guest suspend-to-RAM agent");
MODULE_LICENSE("GPL");
