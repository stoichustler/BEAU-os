// SPDX-License-Identifier: GPL-2.0-only
/*
 * BEAU hypercall helpers shared by BEAU paravirtual drivers.
 */

#include <linux/arm-smccc.h>
#include <linux/errno.h>
#include <linux/export.h>
#include <asm/memory.h>

#include "hcall.h"

#define _HC_ID(x, y)			(((x) << 24) | (y))
#define HC_ID				0x80UL
#define HC_ID_DBG_BASE			0x60UL
#define HC_VM_WDT_KICK			_HC_ID(HC_ID, HC_ID_DBG_BASE + 0x04UL)
#define HC_VIRTIO_PROXY_BACKEND		_HC_ID(HC_ID, HC_ID_DBG_BASE + 0x05UL)
#define HC_IPC				_HC_ID(HC_ID, HC_ID_DBG_BASE + 0x06UL)
#define HC_VM_CRASH_REPORT		_HC_ID(HC_ID, HC_ID_DBG_BASE + 0x08UL)

static long beau_hcall1(unsigned long hcall_id, unsigned long param1)
{
	struct arm_smccc_res res;

	arm_smccc_1_1_hvc(hcall_id, param1, &res);
	return res.a0;
}

static long beau_hcall2(unsigned long hcall_id, unsigned long param1,
	unsigned long param2)
{
	struct arm_smccc_res res;

	arm_smccc_1_1_hvc(hcall_id, param1, param2, &res);
	return res.a0;
}

long beau_hcall_vm_wdt_kick(unsigned long token)
{
	return beau_hcall2(HC_VM_WDT_KICK, token, 0UL);
}
EXPORT_SYMBOL_GPL(beau_hcall_vm_wdt_kick);

long beau_hcall_vm_wdt_kick_vcpu(unsigned long token)
{
	return beau_hcall2(HC_VM_WDT_KICK, token,
		BEAU_VM_WDT_KICK_F_PER_VCPU_V1);
}
EXPORT_SYMBOL_GPL(beau_hcall_vm_wdt_kick_vcpu);

long beau_hcall_vm_crash_report(const struct beau_vm_crash_report *report)
{
	if (report == NULL)
		return -EINVAL;

	return beau_hcall2(HC_VM_CRASH_REPORT, virt_to_phys(report), report->sequence);
}
EXPORT_SYMBOL_GPL(beau_hcall_vm_crash_report);

long beau_hcall_virtio_proxy_backend(struct beau_proxy_ioc *ioc)
{
	return beau_hcall1(HC_VIRTIO_PROXY_BACKEND, virt_to_phys(ioc));
}
EXPORT_SYMBOL_GPL(beau_hcall_virtio_proxy_backend);

long beau_hcall_ipc(struct beau_ipc_ioc *ioc)
{
	return beau_hcall1(HC_IPC, virt_to_phys(ioc));
}
EXPORT_SYMBOL_GPL(beau_hcall_ipc);
