#
# BEAU HYPERVISOR 2026
#

API_MAJOR_VERSION=1
API_MINOR_VERSION=0
BEAU_OS_VERSION ?= 0.1.1

ifneq ($(filter command line,$(origin RELEASE)),)
$(error RELEASE is no longer supported; BEAU always builds the full image)
endif
ifneq ($(filter command line,$(origin CONFIG_RELEASE)),)
$(error CONFIG_RELEASE is no longer supported; BEAU always builds the full image)
endif

GCC_MAJOR=$(shell echo __GNUC__ | $(CC) -E -x c - | tail -n 1)
GCC_MINOR=$(shell echo __GNUC_MINOR__ | $(CC) -E -x c - | tail -n 1)

#enable stack overflow check
STACK_PROTECTOR := 1

MAKEFLAGS += -rR --no-print-directory

BASEDIR := $(shell pwd)
BEAU_ROOT := $(abspath $(dir $(firstword $(MAKEFILE_LIST))))
GIT_TOPDIR := $(shell git rev-parse --show-toplevel 2>/dev/null)
LICENSE_FILE ?= $(or $(firstword $(wildcard $(GIT_TOPDIR)/LICENSE LICENSE ../LICENSE)),/dev/null)
ARCH ?= arm64
PLATFORM ?= qemu
ifeq ($(strip $(PLATFORM)),)
HV_DEFAULT_OBJDIR := $(CURDIR)/out/default_out
else
HV_DEFAULT_OBJDIR := $(CURDIR)/out/$(PLATFORM)_out
endif
HV_OBJDIR ?= $(HV_DEFAULT_OBJDIR)
HV_MODDIR ?= $(HV_OBJDIR)/modules
HV_FILE := beau
HV_DEBUG_FILE := beau.debug

# initialize the flags we used
CFLAGS :=
ASFLAGS :=
LDFLAGS :=
ARFLAGS :=
ARCH_CFLAGS :=
ARCH_ASFLAGS :=
ARCH_ARFLAGS :=
ARCH_LDFLAGS :=

STATIC_ARM64_PLATFORM := $(if $(filter arm64,$(ARCH)),$(if $(filter qemu rk356x,$(PLATFORM)),y,))

ifeq ($(STATIC_ARM64_PLATFORM),y)
CROSS_COMPILE := aarch64-none-elf-
HV_CONFIG_DIR := $(HV_OBJDIR)/configs
HV_CONFIG_H := $(HV_OBJDIR)/include/bconfig.h
HV_AUTOCONF_H := $(HV_OBJDIR)/include/generated/autoconf.h
HV_CONFIG_MK := $(HV_CONFIG_DIR)/config.mk
HV_DOTCONFIG := $(HV_CONFIG_DIR)/.config
HV_PLATFORM_BCONFIG := arch/arm64/platform/$(PLATFORM)/Bconfig
HV_KCONFIG := Kconfig
HV_KCONFIG_GENERATOR := scripts/Kconfiglib/genconfig.py
HV_KCONFIG_DEFCONFIG := scripts/Kconfiglib/defconfig.py
HV_KCONFIG_SETCONFIG := scripts/Kconfiglib/setconfig.py
HV_KCONFIG_MENUCONFIG := scripts/Kconfiglib/menuconfig.py
HV_KCONFIG_CHECKCONFIG := scripts/checkconfig.py
HV_KCONFIG_FILES := $(HV_KCONFIG) $(shell find arch core lib sdk/bsp -name Kconfig -print)
HV_PLATFORM_BCONFIGS := $(wildcard arch/arm64/platform/*/Bconfig)
HV_KCONFIG_FILE_LIST := $(HV_CONFIG_DIR)/Kconfig.files
HV_KCONFIG_DEPS_DIR := $(HV_CONFIG_DIR)/deps
HV_KCONFIG_CHECK_STAMP := $(HV_CONFIG_DIR)/.checkconfig.timestamp
HV_CONFIG_TIMESTAMP := $(HV_CONFIG_DIR)/.configfiles.timestamp
define normalize_kconfig_bool
ifeq ($(origin $(1)),command line)
ifeq ($($(1)),1)
override $(1) := y
else ifeq ($($(1)),0)
override $(1) := n
endif
endif
endef
KCONFIG_BOOL_VARS := \
	CONFIG_AARCH64_IMAGE_HEADER \
	CONFIG_ARM64_PTRAUTH \
	CONFIG_ACPI_PARSE_ENABLED \
	CONFIG_ARM64_MTE \
	CONFIG_ARM64_SPE \
	CONFIG_ARM64_GICV5 \
	CONFIG_AUTOSTART_VM \
	CONFIG_CPUFREQ \
	CONFIG_CPUFREQ_BACKEND_STUB \
	CONFIG_CPUFREQ_POLICY_PERFORMANCE \
	CONFIG_ENABLE_VM1_LK \
	CONFIG_FDT_PARSE_ENABLED \
	CONFIG_GUEST_KERNEL_BZIMAGE \
	CONFIG_GUEST_KERNEL_ELF \
	CONFIG_GUEST_KERNEL_RAWIMAGE \
	CONFIG_HAS_HSM \
	CONFIG_IRQSTAT_LATENCY \
	CONFIG_LAUNCH_VMS_FROM_BSP \
	CONFIG_MULTIBOOT2 \
	CONFIG_PERF \
	CONFIG_PLATFORM_QEMU \
	CONFIG_PLATFORM_RK356X \
	CONFIG_RELOC \
	CONFIG_SCHED_BVT \
	CONFIG_SCHED_CBS \
	CONFIG_SCHED_DTS \
	CONFIG_SCHED_IORR \
	CONFIG_SCHED_NOOP \
	CONFIG_SCHED_PRIO \
	CONFIG_SCHED_RTDS \
	CONFIG_SECURITY_VM_FIXUP \
	CONFIG_SERIAL_8250_PCI \
	CONFIG_STATIC_ARM64_PLATFORM \
	CONFIG_STATIC_QEMU_PLATFORM \
	CONFIG_STATIC_RK356X_PLATFORM \
	CONFIG_STATIC_VFDT
$(foreach v,$(KCONFIG_BOOL_VARS),$(eval $(call normalize_kconfig_bool,$(v))))
KCONFIG_CLI_ASSIGNMENTS = $(foreach v,$(filter CONFIG_%,$(.VARIABLES)),$(if $(filter command line override,$(origin $(v))),'$(patsubst CONFIG_%,%,$(v))=$($(v))'))
ifneq ($(strip $(KCONFIG_CLI_ASSIGNMENTS)),)
KCONFIG_CLI_SYNC_TARGET := kconfig-cli-sync
endif
CONFIG_FREE_GOALS := clean distclean tags Bconfig defconfig menuconfig syncconfig checkconfig
CONFIG_REQUIRED_GOALS := $(filter-out $(CONFIG_FREE_GOALS),$(if $(MAKECMDGOALS),$(MAKECMDGOALS),all))
ifneq ($(CONFIG_REQUIRED_GOALS),)
include $(HV_CONFIG_MK)
endif
BOARD := $(patsubst "%",%,$(CONFIG_BOARD))
SCENARIO := $(patsubst "%",%,$(CONFIG_SCENARIO))
CONFIG_ENABLE_VM1_LK_DTS := $(if $(filter y,$(CONFIG_ENABLE_VM1_LK)),1,0)
ARM64_PLATFORM_CFG_STAMP := $(HV_CONFIG_DIR)/config-vm1-lk-$(CONFIG_ENABLE_VM1_LK_DTS).stamp
ARM64_PLATFORM_DTB := $(HV_OBJDIR)/platform.dtb
ZEPHYR_IMAGE   ?= sdk/imgs/zephyr.bin
LINUX_IMAGE     := sdk/imgs/linux/Image
LINUX_VM1_IMAGE ?= $(LINUX_IMAGE)
LINUX_VM2_IMAGE ?= $(LINUX_IMAGE)
LINUX_VM3_IMAGE ?= $(LINUX_IMAGE)
LINUX_INITRAMFS ?= sdk/imgs/linux/Initramfs.cpio.gz
CFLAGS += -include $(HV_CONFIG_H)
ASFLAGS += -DARM64_PLATFORM_DTB_PATH=\"$(ARM64_PLATFORM_DTB)\"
ASFLAGS += -DBEAU_QEMU_ZEPHYR_IMAGE_PATH=\"$(ZEPHYR_IMAGE)\"
else
$(error Unsupported build configuration: ARCH=$(ARCH) PLATFORM=$(PLATFORM). Use ARCH=arm64 PLATFORM=qemu or PLATFORM=rk356x)
endif

ifeq ($(STATIC_ARM64_PLATFORM),y)
BOARD_INFO_DIR := arch/arm64/platform/$(PLATFORM)
SCENARIO_CFG_DIR := arch/arm64/platform/$(PLATFORM)
BOARD_CFG_DIR := arch/arm64/platform/$(PLATFORM)
else
BOARD_INFO_DIR := $(HV_CONFIG_DIR)/boards
SCENARIO_CFG_DIR := $(HV_CONFIG_DIR)/scenarios/$(SCENARIO)
BOARD_CFG_DIR := $(SCENARIO_CFG_DIR)
endif

ifeq ($(V), 1)
	Q :=
else
	Q := @
endif

libdir ?= /usr/lib
sysconfdir ?= /etc

LD_IN_TOOL = scripts/genld.sh
ARM64_LINKER_DEPS := $(ARCH_LDSCRIPT_IN) $(LD_IN_TOOL) $(HV_CONFIG_MK)
BASH = $(shell which bash)

ARFLAGS += crs

CFLAGS += -Wall -W
CFLAGS += -ffunction-sections -fdata-sections
CFLAGS += -fshort-wchar -ffreestanding
CFLAGS += -fsigned-char
CFLAGS += -nostdinc -nostdlib -fno-common
CFLAGS += -Werror

# ACRN depends on zero length array. Silence the gcc if Warrary-bounds is default option
CFLAGS += -Wno-array-bounds
CFLAGS += -O2

ifdef STACK_PROTECTOR
ifeq (true, $(shell [ $(GCC_MAJOR) -gt 4 ] && echo true))
CFLAGS += -fstack-protector-strong
else
ifeq (true, $(shell [ $(GCC_MAJOR) -eq 4 ] && [ $(GCC_MINOR) -ge 9 ] && echo true))
CFLAGS += -fstack-protector-strong
else
CFLAGS += -fstack-protector
endif
endif
CFLAGS += -DSTACK_PROTECTOR
endif

ifeq (y, $(CONFIG_MULTIBOOT2))
ASFLAGS += -DCONFIG_MULTIBOOT2
endif

ifeq (y, $(CONFIG_RELOC))
ASFLAGS += -DCONFIG_RELOC
endif

LDFLAGS += -Wl,--gc-sections -nostartfiles -nostdlib
LDFLAGS += -Wl,-n,-z,max-page-size=0x1000
LDFLAGS += -Wl,--no-dynamic-linker

ARCH_CFLAGS  += -gdwarf-2
ARCH_ASFLAGS += -gdwarf-2 -DASSEMBLER=1
ARCH_ARFLAGS +=
ARCH_LDFLAGS +=

ARCH_LDSCRIPT = $(HV_OBJDIR)/link_ram.ld

REL_INCLUDE_PATH += include
REL_INCLUDE_PATH += include/lib
REL_INCLUDE_PATH += include/lib/crypto
REL_INCLUDE_PATH += include/lib/libfdt
REL_INCLUDE_PATH += include/common
REL_INCLUDE_PATH += include/debug
REL_INCLUDE_PATH += include/public
REL_INCLUDE_PATH += sdk/bsp/include
REL_INCLUDE_PATH += sdk/bsp/boot/include
REL_INCLUDE_PATH += sdk/bsp/boot/include/guest

REL_INCLUDE_PATH += include/arch/$(ARCH)

INCLUDE_PATH := $(realpath $(REL_INCLUDE_PATH))
INCLUDE_PATH += $(HV_OBJDIR)/include
INCLUDE_PATH += $(BOARD_INFO_DIR)
INCLUDE_PATH += $(BOARD_CFG_DIR)
INCLUDE_PATH += $(SCENARIO_CFG_DIR)

CC ?= gcc
AS ?= as
AR ?= ar
LD ?= ld
OBJCOPY ?= objcopy
NM ?= $(CROSS_COMPILE)nm
DTC ?= dtc

include arch/$(ARCH)/Makefile

LIB_BSP = $(HV_MODDIR)/libbsp.a

export ARCH
export CC AS AR LD OBJCOPY
export CFLAGS ASFLAGS ARFLAGS LDFLAGS ARCH_CFLAGS ARCH_ASFLAGS ARCH_ARFLAGS ARCH_LDFLAGS
export HV_OBJDIR HV_MODDIR INCLUDE_PATH
export LIB_BSP

CFLAGS += -DHV_DEBUG -DPROFILING_ON -fno-omit-frame-pointer

COMMON_C_SRCS += core/vcpu.c
COMMON_C_SRCS += core/vm.c
COMMON_C_SRCS += core/pm.c
COMMON_C_SRCS += core/vm_wdt.c

COMMON_C_SRCS += core/notify.c
COMMON_C_SRCS += core/percpu.c
COMMON_C_SRCS += core/cpu.c
COMMON_C_SRCS += core/ticks.c
COMMON_C_SRCS += core/delay.c
COMMON_C_SRCS += core/timer.c
COMMON_C_SRCS += core/softirq.c
COMMON_C_SRCS += core/trace.c
COMMON_C_SRCS += core/schedule.c
COMMON_C_SRCS += core/sched/arinc653.c
COMMON_C_SRCS += core/sched/bfp.c
COMMON_C_SRCS += core/sched/smart.c
COMMON_C_SRCS += core/mmu.c
ifeq ($(CONFIG_SCHED_NOOP),y)
COMMON_C_SRCS += core/sched/noop.c
endif
ifeq ($(CONFIG_SCHED_BVT),y)
COMMON_C_SRCS += core/sched/bvt.c
endif
ifeq ($(CONFIG_SCHED_RTDS),y)
COMMON_C_SRCS += core/sched/rtds.c
endif
ifeq ($(CONFIG_SCHED_CBS),y)
COMMON_C_SRCS += core/sched/cbs.c
endif
ifeq ($(CONFIG_SCHED_IORR),y)
COMMON_C_SRCS += core/sched/iorr.c
endif
ifeq ($(CONFIG_SCHED_PRIO),y)
COMMON_C_SRCS += core/sched/prio.c
endif
COMMON_C_SRCS += core/sbuf.c
COMMON_C_SRCS += core/logmsg.c
COMMON_C_SRCS += core/irq.c
ifeq ($(ARCH),arm64)
COMMON_C_SRCS += core/ptdev.c
endif

# library componment
COMMON_C_SRCS += lib/memory.c
COMMON_C_SRCS += lib/bits.c
COMMON_C_SRCS += lib/string.c
COMMON_C_SRCS += lib/crypto/crypto_api.c
COMMON_C_SRCS += lib/crypto/mbedtls/hkdf.c
COMMON_C_SRCS += lib/crypto/mbedtls/sha256.c
COMMON_C_SRCS += lib/crypto/mbedtls/md.c
COMMON_C_SRCS += lib/crypto/mbedtls/md_wrap.c
COMMON_C_SRCS += lib/sprintf.c

FDT_LIB_ENABLED := n
ifeq ($(CONFIG_FDT_PARSE_ENABLED),y)
FDT_LIB_ENABLED := y
endif
ifeq ($(CONFIG_STATIC_VFDT),y)
FDT_LIB_ENABLED := y
endif

ifeq ($(FDT_LIB_ENABLED),y)
COMMON_C_SRCS += lib/fdt/fdt.c
COMMON_C_SRCS += lib/fdt/fdt_addresses.c
COMMON_C_SRCS += lib/fdt/fdt_check.c
COMMON_C_SRCS += lib/fdt/fdt_empty_tree.c
COMMON_C_SRCS += lib/fdt/fdt_overlay.c
COMMON_C_SRCS += lib/fdt/fdt_ro.c
COMMON_C_SRCS += lib/fdt/fdt_rw.c
COMMON_C_SRCS += lib/fdt/fdt_strerror.c
COMMON_C_SRCS += lib/fdt/fdt_sw.c
COMMON_C_SRCS += lib/fdt/fdt_wip.c
endif

ifeq ($(CONFIG_FDT_PARSE_ENABLED),y)
COMMON_C_SRCS += core/vfdt.c
else
COMMON_C_SRCS += core/vfdt_static.c
endif

ifdef STACK_PROTECTOR
COMMON_C_SRCS += lib/stack_protector.c
endif
COMMON_C_SRCS += core/vconfig.c
COMMON_C_SRCS += core/event.c
COMMON_C_SRCS += core/fdt.c
COMMON_C_SRCS += sdk/bsp/boot/guest/vboot_info.c
COMMON_C_SRCS += core/vm_load.c
ifeq ($(CONFIG_GUEST_KERNEL_RAWIMAGE),y)
COMMON_C_SRCS += sdk/bsp/boot/guest/rwloader.c
endif
COMMON_C_SRCS += sdk/bsp/boot/boot.c
COMMON_C_SRCS += sdk/bsp/boot/multiboot/multiboot.c
ifeq ($(CONFIG_MULTIBOOT2),y)
COMMON_C_SRCS += sdk/bsp/boot/multiboot/multiboot2.c
endif
COMMON_C_SRCS += sdk/bsp/boot/bare.c

# bsp device-emulation component
COMMON_C_SRCS += sdk/bsp/vuart.c
COMMON_C_SRCS += sdk/bsp/ioreq.c
COMMON_C_SRCS += sdk/bsp/pm.c
BSP_LIB_SRCS := $(filter-out sdk/bsp/ns16550.c sdk/bsp/pl011.c,$(wildcard sdk/bsp/*.c))
BSP_LIB_SRCS += sdk/bsp/pl011.c
BSP_LIB_SRCS += $(wildcard sdk/bsp/arm64/*.c)
BSP_LIB_SRCS += $(wildcard sdk/bsp/cmds/*.c)
BSP_LIB_SRCS += $(wildcard sdk/bsp/virtio/*.c)
BSP_LIB_SRCS += $(wildcard sdk/bsp/pci/*.c)
BSP_LIB_SRCS += sdk/bsp/benchmark/rt-tests/rt-test.c
BSP_LIB_SRCS += sdk/bsp/benchmark/rt-tests/oslat.c
BSP_LIB_SRCS += sdk/bsp/benchmark/rt-tests/ipilat.c
BSP_LIB_SRCS += sdk/bsp/vpci/vpci_core.c
BSP_LIB_SRCS += sdk/bsp/vpci/vpci_pt.c
BSP_LIB_SRCS += sdk/bsp/vpci/vpci_msi.c
BSP_LIB_SRCS += sdk/bsp/vpci/vpci_rc.c
BSP_LIB_SRCS += sdk/bsp/vpci/vpci_sriov.c
ifeq ($(CONFIG_COREMARK),y)
BSP_LIB_SRCS += sdk/bsp/benchmark/coremark/coremark_entry.c
BSP_LIB_SRCS += sdk/bsp/benchmark/coremark/coremark_engine.c
BSP_LIB_SRCS += sdk/bsp/benchmark/coremark/coremark_port.c
BSP_LIB_SRCS += sdk/bsp/benchmark/coremark/coremark_runner.c
BSP_LIB_SRCS += sdk/bsp/benchmark/coremark/core_list_join.c
BSP_LIB_SRCS += sdk/bsp/benchmark/coremark/core_main.c
BSP_LIB_SRCS += sdk/bsp/benchmark/coremark/core_matrix.c
BSP_LIB_SRCS += sdk/bsp/benchmark/coremark/core_state.c
BSP_LIB_SRCS += sdk/bsp/benchmark/coremark/core_util.c
BSP_LIB_SRCS += sdk/bsp/benchmark/coremark/coremark.h
BSP_LIB_SRCS += sdk/bsp/benchmark/coremark/core_portme.h
BSP_LIB_SRCS += sdk/bsp/benchmark/coremark/coremark_engine.h
BSP_LIB_SRCS += sdk/bsp/benchmark/coremark/coremark_port.h
endif
ifeq ($(CONFIG_DHRYSTONE),y)
BSP_LIB_SRCS += sdk/bsp/benchmark/dhrystone/dry.c
BSP_LIB_SRCS += sdk/bsp/benchmark/dhrystone/dhrystone_1.c
BSP_LIB_SRCS += sdk/bsp/benchmark/dhrystone/dhrystone_2.c
BSP_LIB_SRCS += sdk/bsp/benchmark/dhrystone/dhrystone_port.c
BSP_LIB_SRCS += sdk/bsp/benchmark/dhrystone/dhrystone_runner.c
BSP_LIB_SRCS += sdk/bsp/benchmark/dhrystone/dhrystone_port.h
endif

ifeq ($(ARCH),arm64)
COMMON_C_SRCS += arch/arm64/guest/hcall.c
endif

COMMON_C_OBJS := $(patsubst %.c,$(HV_OBJDIR)/%.o,$(COMMON_C_SRCS))

COMMON_MOD = $(HV_MODDIR)/core.a

MODULES += $(COMMON_MOD)

MODULES += $(LIB_BSP)
LIB_BUILD = $(LIB_BSP)
LIB_MK = sdk/bsp/Makefile

DISTCLEAN_OBJS := $(shell find $(BASEDIR) -name '*.o')
VERSION := $(HV_OBJDIR)/include/version.h
BANNER_H := $(HV_OBJDIR)/include/banner.h
BIMAGE_H :=
ifeq ($(STATIC_ARM64_PLATFORM),y)
BIMAGE_H := $(HV_OBJDIR)/include/bimage.h
endif
HEADERS := $(VERSION) $(BANNER_H) $(HV_CONFIG_H) $(HV_CONFIG_TIMESTAMP) $(BIMAGE_H) $(KCONFIG_CLI_SYNC_TARGET)

ifeq ($(STATIC_ARM64_PLATFORM),y)
define WRITE_CONFIG_HEADER
	$(Q){ \
		echo "/* Auto-generated wrapper around Kconfiglib autoconf.h. */"; \
		echo "#ifndef BCONFIG_H"; \
		echo "#define BCONFIG_H"; \
		echo ""; \
		echo "#include <generated/autoconf.h>"; \
		echo ""; \
		for sym in ARM64_GICV5 AUTOSTART_VM ENABLE_VM1_LK HAS_HSM IRQSTAT_LATENCY LAUNCH_VMS_FROM_BSP SERIAL_8250_PCI STATIC_ARM64_PLATFORM STATIC_VFDT; do \
			echo "#ifndef CONFIG_$${sym}"; \
			echo "#define CONFIG_$${sym} 0"; \
			echo "#endif"; \
		done; \
		echo ""; \
		echo "#undef CONFIG_CONSOLE_DEFAULT_VM"; \
		echo "#define CONFIG_CONSOLE_DEFAULT_VM ACRN_INVALID_VMID"; \
		echo "#undef CONFIG_VUART_TIMER_PCPU"; \
		echo "#define CONFIG_VUART_TIMER_PCPU BSP_CPU_ID"; \
		echo "#undef CONFIG_VM_CONSOLE_RINGBUF_VM_NUM"; \
		echo "#define CONFIG_VM_CONSOLE_RINGBUF_VM_NUM (PRE_VM_NUM + SERVICE_VM_NUM)"; \
		echo ""; \
		echo "#define MAX_PCPU_NUM CONFIG_MAX_PCPU_NUM"; \
		echo "#define PRE_VM_NUM CONFIG_PRE_VM_NUM"; \
		echo "#define SERVICE_VM_NUM CONFIG_SERVICE_VM_NUM"; \
		echo "#define MAX_POST_VM_NUM CONFIG_MAX_POST_VM_NUM"; \
		echo "#define MAX_TRUSTY_VM_NUM CONFIG_MAX_TRUSTY_VM_NUM"; \
		echo "#define MAX_VUART_NUM_PER_VM CONFIG_MAX_VUART_NUM_PER_VM"; \
		echo "#define RTVM_SEVERITY_LEVEL CONFIG_RTVM_SEVERITY_LEVEL"; \
		echo ""; \
		echo "#endif /* BCONFIG_H */"; \
	} > $(HV_CONFIG_H)
endef

define RUN_GENCONFIG
	$(Q)KCONFIG_CONFIG=$(HV_DOTCONFIG) python3 $(HV_KCONFIG_GENERATOR) \
		--header-path $(HV_AUTOCONF_H) \
		--config-out $(HV_DOTCONFIG) \
		--sync-deps $(HV_KCONFIG_DEPS_DIR) \
		--file-list $(HV_KCONFIG_FILE_LIST) \
		$(HV_KCONFIG)
	$(Q)cp $(HV_KCONFIG_DEPS_DIR)/auto.conf $(HV_CONFIG_MK)
	$(Q)touch $(HV_CONFIG_MK) $(HV_AUTOCONF_H) $(HV_KCONFIG_FILE_LIST)
	@echo "CONFIG             $(notdir $(HV_CONFIG_H))"
	$(WRITE_CONFIG_HEADER)
endef

$(HV_CONFIG_MK) $(HV_AUTOCONF_H) $(HV_DOTCONFIG) $(HV_KCONFIG_FILE_LIST): Makefile $(HV_KCONFIG_FILES) $(HV_KCONFIG_GENERATOR) $(HV_KCONFIG_DEFCONFIG) $(HV_KCONFIG_SETCONFIG) $(HV_PLATFORM_BCONFIG) $(HV_KCONFIG_CHECK_STAMP) | $(HV_CONFIG_DIR) $(HV_KCONFIG_DEPS_DIR) $(HV_OBJDIR)/include/generated
	@echo "CONFIG             $(notdir $@)"
	$(Q)if [ ! -f $(HV_DOTCONFIG) ]; then \
		KCONFIG_CONFIG=$(HV_DOTCONFIG) python3 $(HV_KCONFIG_DEFCONFIG) --kconfig $(HV_KCONFIG) $(HV_PLATFORM_BCONFIG); \
	fi
	$(Q)if [ -n "$(strip $(KCONFIG_CLI_ASSIGNMENTS))" ]; then \
		KCONFIG_CONFIG=$(HV_DOTCONFIG) python3 $(HV_KCONFIG_SETCONFIG) --kconfig $(HV_KCONFIG) $(KCONFIG_CLI_ASSIGNMENTS); \
	fi
	$(RUN_GENCONFIG)

$(HV_CONFIG_H): $(HV_AUTOCONF_H) $(HV_CONFIG_MK) Makefile | $(HV_OBJDIR)/include
	@echo "CONFIG             $(notdir $@)"
	$(WRITE_CONFIG_HEADER)

$(HV_CONFIG_TIMESTAMP): $(HV_CONFIG_MK) $(HV_CONFIG_H)
	@touch $@

$(HV_KCONFIG_CHECK_STAMP): Makefile $(HV_KCONFIG_FILES) $(HV_KCONFIG_CHECKCONFIG) $(HV_PLATFORM_BCONFIGS) | $(HV_CONFIG_DIR)
	@echo "BUILDING BEAU OS 2026\n"
	@echo "CHECK              Bconfig"
	$(Q)python3 $(HV_KCONFIG_CHECKCONFIG) --kconfig $(HV_KCONFIG) $(HV_PLATFORM_BCONFIGS)
	@touch $@

$(HV_CONFIG_DIR) $(HV_KCONFIG_DEPS_DIR):
	@mkdir -p $@

$(ARM64_PLATFORM_CFG_STAMP): $(HV_CONFIG_TIMESTAMP) | $(HV_CONFIG_DIR)
	@touch $@

.PHONY: Bconfig defconfig syncconfig menuconfig kconfig-cli-sync checkconfig
checkconfig: $(HV_KCONFIG_CHECK_STAMP)

Bconfig defconfig: $(HV_PLATFORM_BCONFIG) $(HV_KCONFIG_CHECK_STAMP) | $(HV_CONFIG_DIR) $(HV_KCONFIG_DEPS_DIR) $(HV_OBJDIR)/include/generated
	@echo "CONFIG             $(notdir $(HV_PLATFORM_BCONFIG))"
	$(Q)KCONFIG_CONFIG=$(HV_DOTCONFIG) python3 $(HV_KCONFIG_DEFCONFIG) --kconfig $(HV_KCONFIG) $(HV_PLATFORM_BCONFIG)
	$(Q)$(MAKE) syncconfig ARCH=$(ARCH) PLATFORM=$(PLATFORM) HV_OBJDIR=$(HV_OBJDIR)

kconfig-cli-sync: | $(HV_CONFIG_DIR) $(HV_KCONFIG_DEPS_DIR) $(HV_OBJDIR)/include/generated
	@echo "CONFIG             command-line"
	$(Q)if [ ! -f $(HV_DOTCONFIG) ]; then \
		KCONFIG_CONFIG=$(HV_DOTCONFIG) python3 $(HV_KCONFIG_DEFCONFIG) --kconfig $(HV_KCONFIG) $(HV_PLATFORM_BCONFIG); \
	fi
	$(Q)if [ -n "$(strip $(KCONFIG_CLI_ASSIGNMENTS))" ]; then \
		KCONFIG_CONFIG=$(HV_DOTCONFIG) python3 $(HV_KCONFIG_SETCONFIG) --kconfig $(HV_KCONFIG) $(KCONFIG_CLI_ASSIGNMENTS); \
	fi
	$(RUN_GENCONFIG)

syncconfig: | $(HV_CONFIG_DIR) $(HV_KCONFIG_DEPS_DIR) $(HV_OBJDIR)/include/generated
	@echo "CONFIG             $(notdir $(HV_DOTCONFIG))"
	$(Q)if [ ! -f $(HV_DOTCONFIG) ]; then \
		KCONFIG_CONFIG=$(HV_DOTCONFIG) python3 $(HV_KCONFIG_DEFCONFIG) --kconfig $(HV_KCONFIG) $(HV_PLATFORM_BCONFIG); \
	fi
	$(Q)if [ -n "$(strip $(KCONFIG_CLI_ASSIGNMENTS))" ]; then \
		KCONFIG_CONFIG=$(HV_DOTCONFIG) python3 $(HV_KCONFIG_SETCONFIG) --kconfig $(HV_KCONFIG) $(KCONFIG_CLI_ASSIGNMENTS); \
	fi
	$(RUN_GENCONFIG)

menuconfig: | $(HV_CONFIG_DIR) $(HV_KCONFIG_DEPS_DIR) $(HV_OBJDIR)/include/generated
	$(Q)if [ ! -f $(HV_DOTCONFIG) ]; then \
		KCONFIG_CONFIG=$(HV_DOTCONFIG) python3 $(HV_KCONFIG_DEFCONFIG) --kconfig $(HV_KCONFIG) $(HV_PLATFORM_BCONFIG); \
	fi
	$(Q)KCONFIG_CONFIG=$(HV_DOTCONFIG) python3 $(HV_KCONFIG_MENUCONFIG) $(HV_KCONFIG)
	$(Q)$(MAKE) syncconfig ARCH=$(ARCH) PLATFORM=$(PLATFORM) HV_OBJDIR=$(HV_OBJDIR)

ifneq ($(BIMAGE_H),)
$(BIMAGE_H): Makefile $(LINUX_VM1_IMAGE) $(LINUX_VM2_IMAGE) $(LINUX_VM3_IMAGE) $(LINUX_INITRAMFS) | $(HV_OBJDIR)/include
	@echo "IMGS               $(notdir $@)"
	@{ \
		vm1_image_size=$$(stat -c %s $(LINUX_VM1_IMAGE)); \
		vm2_image_size=$$(stat -c %s $(LINUX_VM2_IMAGE)); \
		vm3_image_size=$$(stat -c %s $(LINUX_VM3_IMAGE)); \
		initramfs_size=$$(stat -c %s $(LINUX_INITRAMFS)); \
		echo "/* Auto-generated from sdk/imgs. */"; \
		echo "#ifndef BIMAGE_H"; \
		echo "#define BIMAGE_H"; \
		echo "#define BEAU_LINUX_VM1_IMAGE_SIZE $${vm1_image_size}U"; \
		echo "#define BEAU_LINUX_VM2_IMAGE_SIZE $${vm2_image_size}U"; \
		echo "#define BEAU_LINUX_VM3_IMAGE_SIZE $${vm3_image_size}U"; \
		echo "#define BEAU_LINUX_INITRAMFS_SIZE $${initramfs_size}U"; \
		echo "#endif /* BIMAGE_H */"; \
	} > $@

$(HV_OBJDIR)/include $(HV_OBJDIR)/include/generated:
	@mkdir -p $@
endif
endif

# Escape the banner character-by-character; awk gsub backslash handling varies.
# BANNER-03
# BANNER-05
$(BANNER_H): sdk/tag/BANNER-04 Makefile | $(HV_OBJDIR)/include
	@echo "BANNER             $(notdir $@)"
	@{ \
		echo "/* Auto-generated from sdk/BANNER. */"; \
		echo "#ifndef BANNER_H"; \
		echo "#define BANNER_H"; \
		echo "static const char beau_banner[] ="; \
		awk 'BEGIN { bs = sprintf("%c", 92); dq = sprintf("%c", 34) } \
			{ out = ""; \
			  for (i = 1; i <= length($$0); i++) { \
			    c = substr($$0, i, 1); \
			    if (c == bs) out = out bs bs; \
			    else if (c == dq) out = out bs dq; \
			    else out = out c; \
			  } \
			  printf "%c%s%sr%sn%c\n", dq, out, bs, bs, dq; \
			}' $<; \
		echo ";"; \
		echo "#endif /* BANNER_H */"; \
	} > $@

ifneq ($(ARM64_PLATFORM_DTB),)
$(ARM64_PLATFORM_DTB): arch/arm64/platform/$(PLATFORM)/platform.dts $(ARM64_PLATFORM_CFG_STAMP) | $(HV_OBJDIR)
	@echo "DTC                $(notdir $@)"
	$(Q)$(CC) -E -x assembler-with-cpp -P \
		-DCONFIG_ENABLE_VM1_LK=$(CONFIG_ENABLE_VM1_LK_DTS) $< -o $(HV_OBJDIR)/platform.pp.dts
	$(Q)$(DTC) -I dts -O dtb -o $@ $(HV_OBJDIR)/platform.pp.dts

$(HV_OBJDIR):
	@mkdir -p $@
endif

.PHONY: all
all: $(ARCH_ALL_TARGETS) $(HV_OBJDIR)/$(HV_DEBUG_FILE).bin

.PHONY: lib

$(LIB_BUILD): $(HEADERS) $(LIB_MK) $(BSP_LIB_SRCS)
	$(Q)$(MAKE) -f $(LIB_MK) MKFL_NAME=$(LIB_MK)

lib: $(LIB_BUILD)

.PHONY: core-mod

core-mod: $(COMMON_MOD)

$(COMMON_MOD): $(COMMON_C_OBJS)
	$(Q)echo "AR                 $(notdir $@)"
	$(Q)$(AR) $(ARFLAGS) $(COMMON_MOD) $(COMMON_C_OBJS)

$(HV_OBJDIR)/$(HV_DEBUG_FILE).bin: $(HV_OBJDIR)/$(HV_DEBUG_FILE).out
	$(Q)echo "OBJCOPY            $(notdir $@)"
	$(Q)$(OBJCOPY) -O binary $< $@
	$(Q)rm -f $(UPDATE_RESULT)

$(HV_OBJDIR)/$(HV_FILE).out: $(MODULES) $(ARM64_LINKER_DEPS)
	$(Q)echo "CC                 $(notdir $@)"
	$(Q)${BASH} ${LD_IN_TOOL} $(ARCH_LDSCRIPT_IN) $(ARCH_LDSCRIPT) ${HV_CONFIG_MK}
	$(Q)$(CC) -Wl,-Map=$(HV_OBJDIR)/$(HV_FILE).map -o $@ $(LDFLAGS) $(ARCH_LDFLAGS) -T$(ARCH_LDSCRIPT) \
		-Wl,--start-group $(MODULES) -Wl,--end-group

$(HV_OBJDIR)/symtab.c: $(HV_OBJDIR)/$(HV_FILE).out scripts/gen_symtab.py
	$(Q)echo "SYMTAB             $(notdir $@)"
	$(Q)python3 scripts/gen_symtab.py --nm $(NM) --elf $< --out $@

$(HV_OBJDIR)/symtab.o: $(HV_OBJDIR)/symtab.c $(HEADERS)
	$(Q)echo "CC                 $(notdir $@)"
	$(Q)$(CC) $(patsubst %, -I%, $(INCLUDE_PATH)) -I. -c $(CFLAGS) $(ARCH_CFLAGS) $< -o $@ -MMD -MT $@

$(HV_OBJDIR)/$(HV_DEBUG_FILE).out: $(MODULES) $(HV_OBJDIR)/symtab.o $(ARM64_LINKER_DEPS)
	$(Q)echo "CC                 $(notdir $@)"
	$(Q)${BASH} ${LD_IN_TOOL} $(ARCH_LDSCRIPT_IN) $(ARCH_LDSCRIPT) ${HV_CONFIG_MK}
	$(Q)$(CC) -Wl,-Map=$(HV_OBJDIR)/$(HV_DEBUG_FILE).map -o $@ $(LDFLAGS) $(ARCH_LDFLAGS) -T$(ARCH_LDSCRIPT) \
		-Wl,--start-group $(HV_OBJDIR)/symtab.o $(MODULES) -Wl,--end-group

.PHONY: tags
tags:
	$(Q)ctags --languages=Asm,c,c++ -R

.PHONY: clean-all-out
clean-all-out:
	@rm -rf -- "$(BEAU_ROOT)/out" "$(BEAU_ROOT)/sdk/trusty/out"

.PHONY: clean
clean:
	@rm -rf $(VERSION)
	@rm -rf $(HV_OBJDIR)

.PHONY: distclean
distclean:
	@rm -f $(DISTCLEAN_OBJS)
	@rm -f $(VERSION)
	@rm -rf $(HV_OBJDIR)
	@rm -f tags TAGS cscope.files cscope.in.out cscope.out cscope.po.out GTAGS GPATH GRTAGS GSYMS

PHONY: (VERSION)
$(VERSION): $(HV_CONFIG_H) Makefile
	@mkdir -p $(dir $(VERSION))
	@touch $(VERSION)
	@if [ "$(BUILD_VERSION)"x = x ];then \
		COMMIT=`git rev-parse --verify --short HEAD 2>/dev/null`;\
		DIRTY=`git diff-index --name-only HEAD`;\
		if [ -n "$$DIRTY" ];then PATCH="$$COMMIT-dirty";else PATCH="$$COMMIT";fi;\
	else \
		PATCH="$(BUILD_VERSION)"; \
	fi; \
	COMMIT_TAGS=$$(git tag --points-at HEAD|tr -s "\n" " "); \
	COMMIT_TAGS=$$(eval echo $$COMMIT_TAGS);\
	COMMIT_TIME=$$(git log -1 --date=format:"%Y-%m-%d-%T" --format=%cd); \
	TIME=$$(date -u -d "@$${SOURCE_DATE_EPOCH:-$$(date +%s)}" "+%F %T"); \
	USER="$${USER:-$$(id -u -n)}"; \
	BUILD_TYPE="FULL";\
	echo "/*" > $(VERSION); \
	sed 's/^/ * /' "$(LICENSE_FILE)" >> $(VERSION); \
	echo " */" >> $(VERSION); \
	echo "" >> $(VERSION); \
	echo "#ifndef VERSION_H" >> $(VERSION); \
	echo "#define VERSION_H" >> $(VERSION); \
	echo "#define BEAU_OS_VERSION "\"$(BEAU_OS_VERSION)\""" >> $(VERSION);\
	echo "#define HV_API_MAJOR_VERSION $(API_MAJOR_VERSION)U" >> $(VERSION);\
	echo "#define HV_API_MINOR_VERSION $(API_MINOR_VERSION)U" >> $(VERSION);\
	echo "#define HV_BRANCH_VERSION "\"$(BRANCH_VERSION)\""" >> $(VERSION);\
	echo "#define HV_COMMIT_DIRTY "\""$$PATCH"\""" >> $(VERSION);\
	echo "#define HV_COMMIT_TAGS "\"$$COMMIT_TAGS\""" >> $(VERSION);\
	echo "#define HV_COMMIT_TIME "\"$$COMMIT_TIME\""" >> $(VERSION);\
	echo "#define HV_BUILD_TYPE "\""$$BUILD_TYPE"\""" >> $(VERSION);\
	echo "#define HV_BUILD_TIME "\""$$TIME"\""" >> $(VERSION);\
	echo "#define HV_BUILD_USER "\""$$USER"\""" >> $(VERSION);\
	echo "#define HV_BUILD_SCENARIO "\"$(SCENARIO)\""" >> $(VERSION);\
	echo "#define HV_BUILD_BOARD "\"$(BOARD)\""" >> $(VERSION);\
	echo "#endif" >> $(VERSION)

-include $(C_OBJS:.o=.d)
-include $(S_OBJS:.o=.d)

$(HV_OBJDIR)/%.o: %.c $(HEADERS) $(ARCH_PRE_BUILD_TARGETS)
	$(Q)[ ! -e $@ ] && mkdir -p $(dir $@) && mkdir -p $(HV_MODDIR); \
	echo "CC                 $(notdir $@)"; \
	$(CC) $(patsubst %, -I%, $(INCLUDE_PATH)) -I. -c $(CFLAGS) $(ARCH_CFLAGS) $< -o $@ -MMD -MT $@

$(VM_CFG_C_SRCS): %.c: $(HV_CONFIG_TIMESTAMP)

$(VM_CFG_C_OBJS): $(HV_OBJDIR)/%.o: %.c $(HEADERS) $(ARCH_PRE_BUILD_TARGETS)
	$(Q)[ ! -e $@ ] && mkdir -p $(dir $@) && mkdir -p $(HV_MODDIR); \
	echo "CC                 $(notdir $@)"; \
	$(CC) $(patsubst %, -I%, $(INCLUDE_PATH)) -I. -c $(CFLAGS) $(ARCH_CFLAGS) $< -o $@ -MMD -MT $@

ifeq ($(ARCH),arm64)
sdk/imgs/linux/vm1/beau-linux.dtb: sdk/imgs/linux/vm1/beau-linux.dts
	$(Q)echo "DTC                $@"
	$(Q)$(DTC) -I dts -O dtb -o $@ $<

sdk/imgs/linux/vm2/beau-linux.dtb: sdk/imgs/linux/vm2/beau-linux.dts
	$(Q)echo "DTC                $@"
	$(Q)$(DTC) -I dts -O dtb -o $@ $<

sdk/imgs/linux/vm3/beau-linux.dtb: sdk/imgs/linux/vm3/beau-linux.dts
	$(Q)echo "DTC                $@"
	$(Q)$(DTC) -I dts -O dtb -o $@ $<

$(HV_OBJDIR)/arch/arm64/platform/$(PLATFORM)/platform.o: sdk/imgs/lk.bin $(ARM64_PLATFORM_DTB)
$(HV_OBJDIR)/arch/arm64/platform/qemu/platform.o: $(ZEPHYR_IMAGE) sdk/imgs/linux/vm1/beau-linux.dtb sdk/imgs/linux/vm2/beau-linux.dtb sdk/imgs/linux/vm3/beau-linux.dtb
$(HV_OBJDIR)/arch/arm64/platform/rk356x/platform.o: sdk/imgs/zephyr.bin
endif

$(HV_OBJDIR)/%.o: %.S $(HEADERS) $(ARCH_PRE_BUILD_TARGETS)
	$(Q)[ ! -e $@ ] && mkdir -p $(dir $@) && mkdir -p $(HV_MODDIR); \
	echo "AS                 $(notdir $@)"; \
	$(CC) $(patsubst %, -I%, $(INCLUDE_PATH)) -I. $(ASFLAGS) $(ARCH_ASFLAGS) -c $< -o $@ -MMD -MT $@

.DEFAULT_GOAL := all
