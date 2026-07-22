/*
 * Copyright (C) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <types.h>
#include <errno.h>
#include <sprintf.h>
#include <util.h>
#include <vm.h>
#include <vconfig.h>
#include <bsp/pci.h>
#include <bsp/vpci.h>
#include <asm/guest/stage2.h>
#include <asm/guest/vsmmu.h>
#include <asm/vtd.h>

#include "shell_cmds.h"

#define PCISTAT_MAX_STREAMS	64U

static struct arm_smmu_stream_config shell_smmu_streams[PCISTAT_MAX_STREAMS];
static void shell_smmustat_vsmmu(void);
static void shell_format_vmid(char *buf, size_t size, uint16_t vmid)
{
	if (vmid == ACRN_INVALID_VMID) {
		snprintf(buf, size, "none");
	} else {
		snprintf(buf, size, "vm%hu", vmid);
	}
}

static const char *shell_smmu_ste_cfg_to_str(uint32_t cfg)
{
	const char *str;

	switch (cfg) {
	case 0U:
		str = "abort";
		break;
	case 4U:
		str = "bypass";
		break;
	case 6U:
		str = "s2";
		break;
	default:
		str = "unknown";
		break;
	}

	return str;
}

static const char *shell_smmu_state_to_str(enum arm_smmu_state state)
{
	const char *str;

	switch (state) {
	case ARM_SMMU_STATE_ABORT:
		str = "abort";
		break;
	case ARM_SMMU_STATE_READY:
		str = "ready";
		break;
	case ARM_SMMU_STATE_DEGRADED:
		str = "degraded";
		break;
	case ARM_SMMU_STATE_FAILED:
		str = "failed";
		break;
	case ARM_SMMU_STATE_UNDISCOVERED:
	default:
		str = "undiscovered";
		break;
	}

	return str;
}

int32_t shell_smmustat(int32_t argc, __unused char **argv)
{
	struct arm_smmu_hw_info info;
	struct arm64_gicv3_its_stats its;
	uint32_t stream_count;
	uint32_t idx;

	if (argc > 1) {
		shell_puts("usage: smmustat\r\n");
		return -EINVAL;
	}

	(void)memset(&info, 0U, sizeof(info));
	(void)memset(&its, 0U, sizeof(its));
	arm_smmu_poll_events();
	arm_smmu_get_hw_info(&info);
	arm64_gicv3_its_get_stats(&its);
	stream_count = arm_smmu_get_stream_configs(shell_smmu_streams,
		ARRAY_SIZE(shell_smmu_streams));

	shell_puts("\r\nsmmustat:\r\n");
	shell_item_begin("SMMU");
	/* Fields separate discovery/readiness from capability, queue progress, and
	 * assignment. Stream rows compare software owner with STE VM; quarantine or
	 * fault rows are DMA isolation diagnostics.
	 */
	shell_item_line("discovered:%s probed:%s abort:%s ready:%s",
		shell_yes_no(info.discovered), shell_yes_no(info.probed),
		shell_yes_no(info.aborted), shell_yes_no(info.ready));
	shell_item_line("strict:%s caps.valid:%s state:%s",
		shell_yes_no(info.strict), shell_yes_no(info.caps_valid),
		shell_smmu_state_to_str(info.state));
	shell_item_line("init.status:%d", info.init_status);
	if (!info.discovered) {
		shell_item_line("hardware:none");
		shell_item_end();
		shell_smmustat_vsmmu();
		return 0;
	}

	shell_item_line("mmio:base:0x%016lx size:0x%016lx", info.base, info.size);
	shell_item_line("caps:sid.bits:%u oas.bits:%u streams:%u s2p:%s",
		info.sid_bits, info.oas_bits, 1U << info.strtab_log2_entries,
		shell_yes_no((info.idr0 & 0x1U) != 0U));
	shell_item_line("caps:vmid.bits:%u oas.effective:%u required:%u fail:0x%016lx",
		info.vmid_bits, info.effective_oas_bits, info.required_oas_bits,
		info.cap_fail);
	shell_item_line("policy:sid.present:%s max:0x%x",
		shell_yes_no(info.policy_present), info.policy_max_sid);
	shell_item_line("regs:cr0:0x%08x cr1:0x%08x cr2:0x%08x gbpa:0x%08x",
		info.cr0, info.cr1, info.cr2, info.gbpa);
	shell_item_line("strtab:0x%016lx cmdq:0x%016lx evtq:0x%016lx",
		info.strtab_base, info.cmdq_base, info.evtq_base);
	shell_item_line("queue.entries:cmd:%u evt:%u cmdq.en:%s evtq.en:%s",
		info.cmdq_entries, info.evtq_entries, shell_yes_no(info.cmdq_enabled),
		shell_yes_no(info.evtq_enabled));
	shell_item_line("cmdq:prod:0x%08x cons:0x%08x last-cons:0x%08x last-ret:%d",
		info.cmdq_prod, info.cmdq_cons, info.cmdq_last_cons, info.cmdq_last_ret);
	shell_item_line("cmdq.ops:issued:%u sync:%u err:%u full:%u timeout:%u",
		info.cmdq_issued, info.cmdq_syncs, info.cmdq_errors,
		info.cmdq_full, info.cmdq_timeouts);
	shell_item_line("evtq:prod:0x%08x cons:0x%08x last-prod:0x%08x last-cons:0x%08x",
		info.evtq_prod, info.evtq_cons, info.evtq_last_prod,
		info.evtq_last_cons);
	shell_item_line("evtq.ops:poll:%u events:%u err:%u overflow:%u quarantine:%u",
		info.evtq_polled, info.evtq_events, info.evtq_errors,
		info.evtq_overflow, info.evtq_quarantined);
	shell_item_line("evtq.last:w0:0x%016lx w1:0x%016lx w2:0x%016lx w3:0x%016lx",
		info.evtq_last_word0, info.evtq_last_word1,
		info.evtq_last_word2, info.evtq_last_word3);
	shell_item_line("idr0:0x%08x idr1:0x%08x idr5:0x%08x",
		info.idr0, info.idr1, info.idr5);
	shell_item_line("iidr:0x%08x aidr:0x%08x", info.iidr, info.aidr);
	shell_item_line("ready.scope:abort-default + vm-stage2 STE");
	shell_item_line("assignment:%s streams:%u ok:%u fail:%u unassign.ok:%u unassign.fail:%u",
		shell_yes_no(arm_smmu_assignment_ready()), stream_count,
		info.assign_ok, info.assign_fail, info.unassign_ok, info.unassign_fail);
	shell_item_line("its:ready:%s base:0x%016lx size:0x%016lx target:0x%016lx",
		shell_yes_no(its.ready), its.base, its.size, its.target);
	shell_item_line("its:typer:0x%016lx cmd.writer:0x%08x vectors:%u/%u programmed:%u devs:%u",
		its.typer, its.cmdq_writer, its.vectors_used, its.vector_capacity,
		its.vectors_programmed, its.devices_used);
	shell_item_line("its.ops:msi:%u/%u msix:%u/%u rel:%u/%u map:%u/%u unmap:%u/%u",
		its.alloc_msi_ok, its.alloc_msi_fail,
		its.alloc_msix_ok, its.alloc_msix_fail,
		its.release_msi, its.release_msix,
		its.map_event_ok, its.map_event_fail,
		its.unmap_event_ok, its.unmap_event_fail);
	shell_item_line("its.cmd:issued:%u err:%u timeout:%u stall:%u last-ret:%d",
		its.cmd_issued, its.cmd_errors, its.cmd_timeouts,
		its.cmd_stalls, its.last_ret);
	for (idx = 0U; idx < stream_count; idx++) {
		char owner[16U];
		char domain[16U];

		shell_format_vmid(owner, sizeof(owner), shell_smmu_streams[idx].owner_vmid);
		shell_format_vmid(domain, sizeof(domain), shell_smmu_streams[idx].domain_vmid);
		shell_item_line("stream[0x%04x] sw-owner:%s ste-vm:%s assigned:%s quarantine:%s strtab:%s idx:0x%04x",
			shell_smmu_streams[idx].stream_id, owner, domain,
			shell_yes_no(shell_smmu_streams[idx].assigned),
			shell_yes_no(shell_smmu_streams[idx].quarantined),
			shell_yes_no(shell_smmu_streams[idx].in_strtab),
			shell_smmu_streams[idx].strtab_index);
		shell_item_line("     s2:ipa:%u root:0x%016lx",
			shell_smmu_streams[idx].ipa_width,
			shell_smmu_streams[idx].root_table_hpa);
		if (shell_smmu_streams[idx].in_strtab) {
			shell_item_line("     ste:valid:%s cfg:%s(%u) w0:0x%016lx w1:0x%016lx",
				shell_yes_no(shell_smmu_streams[idx].ste_valid),
				shell_smmu_ste_cfg_to_str(shell_smmu_streams[idx].ste_cfg),
				shell_smmu_streams[idx].ste_cfg, shell_smmu_streams[idx].ste[0],
				shell_smmu_streams[idx].ste[1]);
			shell_item_line("     ste:w2:0x%016lx w3:0x%016lx",
				shell_smmu_streams[idx].ste[2], shell_smmu_streams[idx].ste[3]);
		}
		if (shell_smmu_streams[idx].fault_count != 0U) {
			shell_item_line("     fault:count:%u code:0x%02x iova:0x%016lx",
				shell_smmu_streams[idx].fault_count,
				shell_smmu_streams[idx].last_fault_code,
				shell_smmu_streams[idx].last_fault_iova);
		}
		shell_output_checkpoint();
	}
	shell_item_end();
	shell_smmustat_vsmmu();

	return 0;
}

static void shell_smmustat_vsmmu_one(uint16_t vm_id,
	const struct arm64_vsmmu_debug *debug)
{
	shell_item_begin("vm%hu vSMMU", vm_id);
	shell_item_line("configured:%s available:%s mmio:0x%016lx+0x%016lx",
		shell_yes_no(debug->size != 0UL), shell_yes_no(debug->available),
		debug->base, debug->size);
	if (!debug->available) {
		shell_item_line("state:hidden");
		shell_item_end();
		return;
	}
	shell_item_line("regs:cr0:0x%08x irq.ctrl:0x%08x gerror:0x%08x/%08x",
		debug->cr0, debug->irq_ctrl, debug->gerror, debug->gerrorn);
	shell_item_line("tables:strtab:0x%016lx cmdq:0x%016lx evtq:0x%016lx",
		debug->strtab_base, debug->cmdq_base, debug->evtq_base);
	shell_item_line("cmdq:prod:0x%08x cons:0x%08x processed:%lu rejected:%lu",
		debug->cmdq_prod, debug->cmdq_cons, debug->commands_processed,
		debug->commands_rejected);
	shell_item_line("evtq:prod:0x%08x cons:0x%08x", debug->evtq_prod,
		debug->evtq_cons);
	shell_item_line("worker:cpu%hu pending:%s budget:%lu generation:%lu irq:%s",
		debug->worker_pcpu, shell_yes_no(debug->worker_pending),
		debug->budget_exhausted, debug->generation,
		shell_yes_no(debug->irq_asserted));
	shell_item_line("sid-map:none broker:S1+S2-pending");
	shell_item_end();
}

static void shell_smmustat_vsmmu(void)
{
	struct arm64_vsmmu_debug debug;
	uint16_t vm_id;
	bool found = false;

	for (vm_id = 0U; vm_id < CONFIG_MAX_VM_NUM; vm_id++) {
		if (arm64_vsmmu_get_debug(vm_id, &debug)) {
			shell_smmustat_vsmmu_one(vm_id, &debug);
			found = true;
			shell_output_checkpoint();
		}
	}
	if (!found) {
		shell_item_begin("vSMMU");
		shell_item_line("instances:none");
		shell_item_end();
	}
}

static void shell_format_bdf(char *buf, size_t size, union pci_bdf bdf)
{
	snprintf(buf, size, "%02x:%02x.%x", bdf.bits.b, bdf.bits.d, bdf.bits.f);
}

static uint32_t shell_pci_vcfg_read(const struct pci_vdev *vdev, uint32_t offset,
	uint32_t bytes)
{
	uint32_t val = 0U;
	uint32_t idx;

	if ((vdev == NULL) || (bytes == 0U) || (bytes > 4U) ||
		((offset + bytes) > PCIE_CONFIG_SPACE_SIZE)) {
		return 0U;
	}

	for (idx = 0U; idx < bytes; idx++) {
		val |= (uint32_t)vdev->cfgdata.data_8[offset + idx] << (idx * 8U);
	}

	return val;
}

static void shell_pcistat_print_bars(const struct pci_vdev *vdev)
{
	uint32_t bar_idx;

	for (bar_idx = 0U; bar_idx < vdev->nr_bars; bar_idx++) {
		const struct pci_vbar *vbar = &vdev->vbars[bar_idx];
		const char *type;

		if (vbar->is_mem64hi || (vbar->size == 0UL)) {
			continue;
		}
		type = ((vbar->bar_type.io_space.indicator == 1U) && !vbar->is_mem64hi) ?
			"io" : "mem";
		shell_item_line("     bar%u:%s gpa:0x%016lx hpa:0x%016lx size:0x%016lx",
			bar_idx, type, vbar->base_gpa, vbar->base_hpa, vbar->size);
	}
}

static void shell_pcistat_print_msi(const struct pci_vdev *vdev)
{
	uint32_t ctrl;
	uint32_t mask = 0U;
	bool pvm;

	if (vdev->msi.capoff == 0U) {
		return;
	}

	ctrl = shell_pci_vcfg_read(vdev, vdev->msi.capoff + PCIR_MSI_CTRL, 2U);
	pvm = ((ctrl & PCIM_MSICTRL_PVMC) != 0U);
	if (pvm) {
		uint32_t mask_offset = vdev->msi.is_64bit ? PCIR_MSI_MASK : (PCIR_MSI_MASK - 4U);

		mask = shell_pci_vcfg_read(vdev, vdev->msi.capoff + mask_offset, 4U);
	}

	shell_item_line("     msi:cap:0x%02x len:%u vectors:%u 64:%s pvm:%s enabled:%s ctrl:0x%04x mask:0x%08x",
		vdev->msi.capoff, vdev->msi.caplen, vdev->msi.vector_count,
		shell_yes_no(vdev->msi.is_64bit),
		shell_yes_no(pvm), shell_yes_no((ctrl & PCIM_MSICTRL_MSI_ENABLE) != 0U),
		ctrl, mask);
}

static void shell_pcistat_print_msix(const struct pci_vdev *vdev)
{
	uint32_t ctrl;
	uint64_t hole_gpa = 0UL;
	uint64_t hole_hpa = 0UL;
	uint64_t hole_size = 0UL;

	if (vdev->msix.capoff == 0U) {
		return;
	}

	ctrl = shell_pci_vcfg_read(vdev, vdev->msix.capoff + PCIR_MSIX_CTRL, 2U);
	if ((vdev->msix.mmio_gpa != 0UL) && (vdev->msix.table_count != 0U)) {
		uint64_t table_gpa = vdev->msix.mmio_gpa + vdev->msix.table_offset;
		uint64_t table_hpa = vdev->msix.mmio_hpa + vdev->msix.table_offset;
		uint64_t table_size = (uint64_t)vdev->msix.table_count * MSIX_TABLE_ENTRY_SIZE;

		hole_gpa = table_gpa & PAGE_MASK;
		hole_hpa = table_hpa & PAGE_MASK;
		hole_size = ((table_gpa + table_size + PAGE_SIZE - 1UL) & PAGE_MASK) - hole_gpa;
	}

	shell_item_line("     msix:cap:0x%02x len:%u table:bar%u off:0x%08x count:%u enabled:%s masked:%s on-msi:%s programmed:%s",
		vdev->msix.capoff, vdev->msix.caplen, vdev->msix.table_bar,
		vdev->msix.table_offset, vdev->msix.table_count,
		shell_yes_no((ctrl & PCIM_MSIXCTRL_MSIX_ENABLE) != 0U),
		shell_yes_no((ctrl & PCIM_MSIXCTRL_FUNCTION_MASK) != 0U),
		shell_yes_no(vdev->msix.is_vmsix_on_msi),
		shell_yes_no(vdev->msix.is_vmsix_on_msi_programmed));
	if (hole_size != 0UL) {
		shell_item_line("     msix-hole:gpa:0x%016lx hpa:0x%016lx size:0x%016lx",
			hole_gpa, hole_hpa, hole_size);
	}
}

static const struct arm_smmu_stream_config *shell_find_stream_config(
	const struct arm_smmu_stream_config *streams, uint32_t stream_count,
	uint32_t stream_id)
{
	const struct arm_smmu_stream_config *found = NULL;
	uint32_t idx;

	for (idx = 0U; idx < stream_count; idx++) {
		if (streams[idx].stream_id == stream_id) {
			found = &streams[idx];
			break;
		}
	}

	return found;
}

static const char *shell_pcistat_stream_state(const struct arm_smmu_stream_config *stream, uint16_t vm_id)
{
	const char *state;

	if (stream == NULL) {
		state = "missing";
	} else if (stream->quarantined) {
		state = "quarantine";
	} else if (!stream->assigned) {
		state = "free";
	} else if (stream->owner_vmid == vm_id) {
		state = "owned";
	} else {
		state = "wrong-vm";
	}

	return state;
}

static void shell_pcistat_host(void)
{
	struct pci_mmcfg_region *mmcfg = get_mmcfg_region();
	uint32_t pdev_count = get_pci_pdev_num();
	uint32_t idx;

	shell_item_begin("host PCI");
	shell_item_line("ecam:0x%016lx bus:%u-%u pdevs:%u",
		mmcfg->address, mmcfg->start_bus, mmcfg->end_bus, pdev_count);
	for (idx = 0U; idx < pdev_count; idx++) {
		const struct pci_pdev *pdev = get_pci_pdev(idx);
		char bdf[16U];

		if (pdev == NULL) {
			continue;
		}
		shell_format_bdf(bdf, sizeof(bdf), pdev->bdf);
		shell_item_line("[%02u] %s class:%02x:%02x hdr:0x%02x bars:%u drhd:%u",
			idx, bdf, pdev->base_class, pdev->sub_class, pdev->hdr_type,
			pdev->nr_bars, pdev->drhd_index);
		shell_output_checkpoint();
	}
	shell_item_end();
}

static void shell_pcistat_vm(uint16_t vm_id,
	const struct arm_smmu_stream_config *streams, uint32_t stream_count)
{
	struct acrn_vm_config *vm_config = get_vm_config(vm_id);
	struct acrn_vm *vm = get_vm_from_vmid(vm_id);
	uint16_t idx;

	if ((vm_config->pci_devs == NULL) || (vm_config->pci_dev_num == 0U)) {
		shell_item_begin("vm-%hu PCI", vm_id);
		shell_item_line("devices:none");
		shell_item_end();
		return;
	}

	shell_item_begin("vm-%hu PCI:%s", vm_id, vm_config->name);
	shell_item_line("state:%s configured:%hu vpci:%s",
		is_poweroff_vm(vm) ? "poweroff" : "created",
		vm_config->pci_dev_num,
		(vm->vpci.pci_mmcfg.address != 0UL) ? "Y" : "N");
	for (idx = 0U; idx < vm_config->pci_dev_num; idx++) {
		const struct acrn_vm_pci_dev_config *dev_config = &vm_config->pci_devs[idx];
		const struct arm_smmu_stream_config *stream;
		struct pci_vdev *vdev = NULL;
		char pbdf[16U];
		char vbdf[16U];

		if (!is_poweroff_vm(vm) && (vm->vpci.pci_mmcfg.address != 0UL)) {
			vdev = pci_find_vdev(&vm->vpci, dev_config->vbdf);
		}
		stream = shell_find_stream_config(streams, stream_count,
			(uint32_t)dev_config->pbdf.value);
		shell_format_bdf(pbdf, sizeof(pbdf), dev_config->pbdf);
		shell_format_bdf(vbdf, sizeof(vbdf), dev_config->vbdf);
		shell_item_line("[%02hu] p:%s v:%s pdev:%s vdev:%s stream:0x%04x smmu:%s",
			idx, pbdf, vbdf,
			shell_yes_no(dev_config->pdev != NULL),
			shell_yes_no(vdev != NULL),
			dev_config->pbdf.value,
			shell_pcistat_stream_state(stream, vm_id));
		if (stream != NULL) {
			shell_item_line("     owner:vm%hu ipa:%u root:0x%016lx quarantine:%s",
				stream->owner_vmid, stream->ipa_width, stream->root_table_hpa,
				shell_yes_no(stream->quarantined));
			if (stream->fault_count != 0U) {
				shell_item_line("     fault:count:%u code:0x%02x iova:0x%016lx",
					stream->fault_count, stream->last_fault_code,
					stream->last_fault_iova);
			}
		}
		if (vdev != NULL) {
			shell_pcistat_print_bars(vdev);
			shell_pcistat_print_msi(vdev);
			shell_pcistat_print_msix(vdev);
		}
		shell_output_checkpoint();
	}
	shell_item_end();
}

int32_t shell_pcistat(int32_t argc, __unused char **argv)
{
	uint32_t stream_count;
	uint16_t vm_id;

	if (argc > 1) {
		shell_puts("usage: pcistat\r\n");
		return -EINVAL;
	}

	arm_smmu_poll_events();
	stream_count = arm_smmu_get_stream_configs(shell_smmu_streams,
		ARRAY_SIZE(shell_smmu_streams));
	/* PCI rows identify physical/virtual presence and StreamID. BAR rows show
	 * guest/host windows; MSI/MSI-X rows show virtual enablement and programming;
	 * fault rows are cumulative SMMU evidence.
	 */
	shell_puts("\r\npcistat:\r\n");
	shell_pcistat_host();

	for (vm_id = 0U; vm_id < CONFIG_MAX_VM_NUM; vm_id++) {
		struct acrn_vm_config *vm_config = get_vm_config(vm_id);

		if ((vm_config->pci_devs != NULL) && (vm_config->pci_dev_num != 0U)) {
			shell_pcistat_vm(vm_id, shell_smmu_streams, stream_count);
		}
	}

	return 0;
}
