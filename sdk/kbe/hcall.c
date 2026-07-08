// SPDX-License-Identifier: GPL-2.0-only
/*
 * BEAU hypercall helpers shared by BEAU paravirtual drivers.
 */

#include <linux/arm-smccc.h>
#include <linux/export.h>
#include <asm/memory.h>

#include "hcall.h"

#define _HC_ID(x, y)			(((x) << 24) | (y))
#define HC_ID				0x80UL
#define HC_ID_DBG_BASE			0x60UL
#define HC_VM_WDT_KICK			_HC_ID(HC_ID, HC_ID_DBG_BASE + 0x04UL)
#define HC_VIRTIO_PROXY_BACKEND		_HC_ID(HC_ID, HC_ID_DBG_BASE + 0x05UL)

static long beau_hcall1(unsigned long hcall_id, unsigned long param1)
{
	struct arm_smccc_res res;

	arm_smccc_1_1_hvc(hcall_id, param1, &res);
	return res.a0;
}

long beau_hcall_vm_wdt_kick(unsigned long token)
{
	return beau_hcall1(HC_VM_WDT_KICK, token);
}
EXPORT_SYMBOL_GPL(beau_hcall_vm_wdt_kick);

long beau_hcall_virtio_proxy_backend(struct beau_proxy_ioc *ioc)
{
	return beau_hcall1(HC_VIRTIO_PROXY_BACKEND, virt_to_phys(ioc));
}
EXPORT_SYMBOL_GPL(beau_hcall_virtio_proxy_backend);
