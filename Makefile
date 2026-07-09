#
# BEAU hypervisor Makefile
#

API_MAJOR_VERSION=1
API_MINOR_VERSION=0

GCC_MAJOR=$(shell echo __GNUC__ | $(CC) -E -x c - | tail -n 1)
GCC_MINOR=$(shell echo __GNUC_MINOR__ | $(CC) -E -x c - | tail -n 1)

#enable stack overflow check
STACK_PROTECTOR := 1

MAKEFLAGS += -rR --no-print-directory

BASEDIR := $(shell pwd)
GIT_TOPDIR := $(shell git rev-parse --show-toplevel 2>/dev/null)
LICENSE_FILE := $(or $(firstword $(wildcard ../LICENSE $(if $(GIT_TOPDIR),$(GIT_TOPDIR)/LICENSE))),/dev/null)
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
BOARD := $(PLATFORM)
SCENARIO := $(PLATFORM)
RELEASE ?= n
CROSS_COMPILE := aarch64-none-elf-
CONFIG_BOARD := $(PLATFORM)
CONFIG_SCENARIO := $(PLATFORM)
CONFIG_RELEASE := n
CONFIG_RELOC := n
CONFIG_MULTIBOOT2 := n
CONFIG_FDT_PARSE_ENABLED := n
CONFIG_STATIC_VFDT := y
CONFIG_ARM64_GICV5 ?= n
CONFIG_GUEST_KERNEL_RAWIMAGE := y
CONFIG_GUEST_KERNEL_BZIMAGE := n
CONFIG_GUEST_KERNEL_ELF := n
CONFIG_SCHED_BVT := y
CONFIG_SCHED_RTDS := y
CONFIG_ENABLE_VM1_LK ?= 0
HV_CONFIG_DIR := $(HV_OBJDIR)/configs
HV_CONFIG_H := $(HV_OBJDIR)/include/arm64_platform_config.h
ifeq ($(PLATFORM),qemu)
CONFIG_HV_RAM_START := 0x50000000
CONFIG_MAX_PCPU_NUM := 8U
else ifeq ($(PLATFORM),rk356x)
CONFIG_HV_RAM_START := 0x00A00000
CONFIG_MAX_PCPU_NUM := 4U
endif
CFLAGS += -include $(HV_CONFIG_H)
HV_CONFIG_MK := $(HV_CONFIG_DIR)/config.mk
HV_CONFIG_TIMESTAMP := $(HV_CONFIG_DIR)/.configfiles.timestamp
HV_DIFFCONFIG_LIST := $(HV_CONFIG_DIR)/.diffconfig
ARM64_PLATFORM_CFG_STAMP := $(HV_CONFIG_DIR)/config-vm1-lk-$(CONFIG_ENABLE_VM1_LK).stamp
ARM64_PLATFORM_DTB := $(HV_OBJDIR)/platform.dtb
LINUX_VM1_IMAGE := sdk/image/linux/vm1/Image
LINUX_VM2_IMAGE := sdk/image/linux/vm2/Image
LINUX_INITRAMFS := sdk/image/linux/Initramfs.cpio.gz
ASFLAGS += -DARM64_PLATFORM_DTB_PATH=\"$(ARM64_PLATFORM_DTB)\"
else
include scripts/makefile/config.mk
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

ifeq (y, $(CONFIG_RELEASE))
LDFLAGS += -s
endif

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
export HV_OBJDIR HV_MODDIR CONFIG_RELEASE INCLUDE_PATH
export LIB_BSP

ifneq ($(CONFIG_RELEASE),y)
CFLAGS += -DHV_DEBUG -DPROFILING_ON -fno-omit-frame-pointer
endif

COMMON_C_SRCS += core/vcpu.c
COMMON_C_SRCS += core/vm.c
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
COMMON_C_SRCS += core/mmu.c
ifeq ($(CONFIG_SCHED_NOOP),y)
COMMON_C_SRCS += core/sched_noop.c
endif
ifeq ($(CONFIG_SCHED_BVT),y)
COMMON_C_SRCS += core/sched_bvt.c
endif
ifeq ($(CONFIG_SCHED_RTDS),y)
COMMON_C_SRCS += core/sched_rtds.c
endif
ifeq ($(CONFIG_SCHED_IORR),y)
COMMON_C_SRCS += core/sched_iorr.c
endif
ifeq ($(CONFIG_SCHED_PRIO),y)
COMMON_C_SRCS += core/sched_prio.c
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
COMMON_C_SRCS += core/vm_config.c
COMMON_C_SRCS += core/event.c
COMMON_C_SRCS += core/fdt.c
COMMON_C_SRCS += sdk/bsp/boot/guest/vboot_info.c
COMMON_C_SRCS += core/vm_load.c
ifeq ($(CONFIG_GUEST_KERNEL_RAWIMAGE),y)
COMMON_C_SRCS += sdk/bsp/boot/guest/rawimage_loader.c
endif
COMMON_C_SRCS += sdk/bsp/boot/boot.c
COMMON_C_SRCS += sdk/bsp/boot/multiboot/multiboot.c
ifeq ($(CONFIG_MULTIBOOT2),y)
COMMON_C_SRCS += sdk/bsp/boot/multiboot/multiboot2.c
endif
COMMON_C_SRCS += sdk/bsp/boot/bare.c

# bsp device-emulation component
COMMON_C_SRCS += sdk/bsp/vuart.c
COMMON_C_SRCS += sdk/bsp/io_req.c
BSP_LIB_SRCS := $(filter-out sdk/bsp/ns16550.c sdk/bsp/pl011.c,$(wildcard sdk/bsp/*.c))
BSP_LIB_SRCS += sdk/bsp/pl011.c
BSP_LIB_SRCS += $(wildcard sdk/bsp/arm64/*.c)
BSP_LIB_SRCS += $(wildcard sdk/bsp/virtio/*.c)

ifeq ($(ARCH),arm64)
COMMON_C_SRCS += arch/arm64/guest/hypercall.c
endif

COMMON_C_OBJS := $(patsubst %.c,$(HV_OBJDIR)/%.o,$(COMMON_C_SRCS))

COMMON_MOD = $(HV_MODDIR)/core_mod.a

MODULES += $(COMMON_MOD)

MODULES += $(LIB_BSP)
LIB_BUILD = $(LIB_BSP)
LIB_MK = sdk/bsp/Makefile

DISTCLEAN_OBJS := $(shell find $(BASEDIR) -name '*.o')
VERSION := $(HV_OBJDIR)/include/version.h
BANNER_H := $(HV_OBJDIR)/include/banner.h
LINUX_IMAGE_SIZE_H :=
ifeq ($(STATIC_ARM64_PLATFORM),y)
LINUX_IMAGE_SIZE_H := $(HV_OBJDIR)/include/linux_image_sizes.h
endif
HEADERS := $(VERSION) $(BANNER_H) $(HV_CONFIG_H) $(HV_CONFIG_TIMESTAMP) $(LINUX_IMAGE_SIZE_H)

ifeq ($(STATIC_ARM64_PLATFORM),y)
$(HV_CONFIG_MK): | $(HV_CONFIG_DIR)
	@{ \
		echo "CONFIG_HV_RAM_START=$(CONFIG_HV_RAM_START)"; \
		echo "CONFIG_ARM64_GICV5=$(CONFIG_ARM64_GICV5)"; \
	} > $@

$(HV_CONFIG_H): Makefile $(ARM64_PLATFORM_CFG_STAMP) | $(HV_OBJDIR)/include
	@echo "config    $(notdir $@)"
	@{ \
		echo "/* Auto-generated ARM64 static platform configuration. */"; \
		echo "#ifndef ARM64_PLATFORM_CONFIG_H"; \
		echo "#define ARM64_PLATFORM_CONFIG_H"; \
		echo "#define CONFIG_STATIC_ARM64_PLATFORM 1"; \
		if [ "$(PLATFORM)" = "qemu" ]; then \
			echo "#define CONFIG_STATIC_QEMU_PLATFORM 1"; \
		fi; \
		if [ "$(PLATFORM)" = "rk356x" ]; then \
			echo "#define CONFIG_STATIC_RK356X_PLATFORM 1"; \
		fi; \
		echo "#ifndef CONFIG_ARM64_GICV5"; \
		echo "#define CONFIG_ARM64_GICV5 0"; \
		echo "#endif"; \
		echo "#define CONFIG_BOARD $(PLATFORM)"; \
		echo "#define CONFIG_SCENARIO $(PLATFORM)"; \
		echo "#define CONFIG_GUEST_KERNEL_RAWIMAGE 1"; \
		echo "#define CONFIG_HAS_HSM 0"; \
		echo "#define CONFIG_AUTOSTART_VM 1"; \
		echo "#define CONFIG_LAUNCH_VMS_FROM_BSP 1"; \
		echo "#define CONFIG_STATIC_VFDT 1"; \
		echo "#define CONFIG_HV_RAM_START $(CONFIG_HV_RAM_START)UL"; \
		echo "#define CONFIG_STACK_SIZE 8192U"; \
		echo "#define CONFIG_SCHED_BVT y"; \
		echo "#define CONFIG_SCHED_RTDS y"; \
		echo "#define CONFIG_ENABLE_VM1_LK $(CONFIG_ENABLE_VM1_LK)"; \
		echo "#define CONFIG_SERIAL_8250_PCI 0"; \
		echo "#define CONFIG_CONSOLE_DEFAULT_VM ACRN_INVALID_VMID"; \
		echo "#define CONFIG_CONSOLE_LOGLEVEL_DEFAULT 6U"; \
		echo "#define CONFIG_MEM_LOGLEVEL_DEFAULT 0U"; \
		echo "#define CONFIG_NPK_LOGLEVEL_DEFAULT 0U"; \
		echo "#define CONFIG_VUART_TIMER_PCPU BSP_CPU_ID"; \
		echo "#define CONFIG_VUART_RX_BUF_SIZE 256U"; \
		echo "#define CONFIG_VUART_TX_BUF_SIZE 256U"; \
		echo "#define CONFIG_VM_CONSOLE_RINGBUF_VM_NUM (PRE_VM_NUM + SERVICE_VM_NUM)"; \
		echo "#define CONFIG_VM_CONSOLE_RINGBUF_SIZE 4096U"; \
		echo "#define CONFIG_MAX_EMULATED_MMIO_REGIONS 8U"; \
		echo "#define CONFIG_MAX_MSIX_TABLE_NUM 16U"; \
		echo "#define CONFIG_MAX_PCI_DEV_NUM 16U"; \
		echo "#define CONFIG_MAX_PT_IRQ_ENTRIES 32U"; \
		echo "#define CONFIG_MAX_IOAPIC_NUM 0U"; \
		echo "#define CONFIG_SPACE_SIZE 0x10000000UL"; \
		echo "#define CONFIG_ADDR 0xcf8U"; \
		echo "#define CONFIG_DATA 0xcfcU"; \
		echo "#define CONFIG_IGD_SBDF 0U"; \
		echo "#define MAX_PCPU_NUM $(CONFIG_MAX_PCPU_NUM)"; \
		echo "#define PRE_VM_NUM 2U"; \
		echo "#define SERVICE_VM_NUM 1U"; \
		echo "#define MAX_POST_VM_NUM 0U"; \
		echo "#define MAX_TRUSTY_VM_NUM 0U"; \
		echo "#define MAX_VUART_NUM_PER_VM 2U"; \
		echo "#define RTVM_SEVERITY_LEVEL 0x30U"; \
		echo "#endif /* ARM64_PLATFORM_CONFIG_H */"; \
	} > $@

$(HV_CONFIG_TIMESTAMP): $(HV_CONFIG_MK)
	@touch $@

$(HV_CONFIG_DIR):
	@mkdir -p $@

$(ARM64_PLATFORM_CFG_STAMP): | $(HV_CONFIG_DIR)
	@touch $@

ifneq ($(LINUX_IMAGE_SIZE_H),)
$(LINUX_IMAGE_SIZE_H): Makefile $(LINUX_VM1_IMAGE) $(LINUX_VM2_IMAGE) $(LINUX_INITRAMFS) | $(HV_OBJDIR)/include
	@echo "image     $(notdir $@)"
	@{ \
		vm1_image_size=$$(stat -c %s $(LINUX_VM1_IMAGE)); \
		vm2_image_size=$$(stat -c %s $(LINUX_VM2_IMAGE)); \
		initramfs_size=$$(stat -c %s $(LINUX_INITRAMFS)); \
		echo "/* Auto-generated from sdk/image. */"; \
		echo "#ifndef LINUX_IMAGE_SIZES_H"; \
		echo "#define LINUX_IMAGE_SIZES_H"; \
		echo "#define BEAU_LINUX_IMAGE_SIZE $${vm1_image_size}U"; \
		echo "#define BEAU_LINUX_VM1_IMAGE_SIZE $${vm1_image_size}U"; \
		echo "#define BEAU_LINUX_VM2_IMAGE_SIZE $${vm2_image_size}U"; \
		echo "#define BEAU_LINUX_INITRAMFS_SIZE $${initramfs_size}U"; \
		echo "#endif /* LINUX_IMAGE_SIZES_H */"; \
	} > $@

$(HV_OBJDIR)/include:
	@mkdir -p $@
endif
endif

$(BANNER_H): sdk/BANNER | $(HV_OBJDIR)/include
	@echo "banner    $(notdir $@)"
	@{ \
		echo "/* Auto-generated from sdk/BANNER. */"; \
		echo "#ifndef BANNER_H"; \
		echo "#define BANNER_H"; \
		echo "static const char beau_banner[] ="; \
		awk '{ gsub(/\\/, "\\\\"); gsub(/"/, "\\\""); printf "\"%s\\r\\n\"\n", $$0 }' $<; \
		echo ";"; \
		echo "#endif /* BANNER_H */"; \
	} > $@

ifneq ($(ARM64_PLATFORM_DTB),)
$(ARM64_PLATFORM_DTB): arch/arm64/platform/$(PLATFORM)/platform.dts $(ARM64_PLATFORM_CFG_STAMP) | $(HV_OBJDIR)
	@echo "dtc       $(notdir $@)"
	$(Q)$(CC) -E -x assembler-with-cpp -P \
		-DCONFIG_ENABLE_VM1_LK=$(CONFIG_ENABLE_VM1_LK) $< -o $(HV_OBJDIR)/platform.pp.dts
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
	$(Q)echo "ar        $(notdir $@)"
	$(Q)$(AR) $(ARFLAGS) $(COMMON_MOD) $(COMMON_C_OBJS)

$(HV_OBJDIR)/$(HV_DEBUG_FILE).bin: $(HV_OBJDIR)/$(HV_DEBUG_FILE).out
	$(Q)echo "objcopy   $(notdir $@)"
	$(Q)$(OBJCOPY) -O binary $< $@
	$(Q)rm -f $(UPDATE_RESULT)

$(HV_OBJDIR)/$(HV_FILE).out: $(MODULES)
	$(Q)echo "cc        $(notdir $@)"
	$(Q)${BASH} ${LD_IN_TOOL} $(ARCH_LDSCRIPT_IN) $(ARCH_LDSCRIPT) ${HV_CONFIG_MK}
	$(Q)$(CC) -Wl,-Map=$(HV_OBJDIR)/$(HV_FILE).map -o $@ $(LDFLAGS) $(ARCH_LDFLAGS) -T$(ARCH_LDSCRIPT) \
		-Wl,--start-group $^ -Wl,--end-group

$(HV_OBJDIR)/symtab.c: $(HV_OBJDIR)/$(HV_FILE).out scripts/gen_symtab.py
	$(Q)echo "symtab    $(notdir $@)"
	$(Q)python3 scripts/gen_symtab.py --nm $(NM) --elf $< --out $@

$(HV_OBJDIR)/symtab.o: $(HV_OBJDIR)/symtab.c $(HEADERS)
	$(Q)echo "cc        $(notdir $@)"
	$(Q)$(CC) $(patsubst %, -I%, $(INCLUDE_PATH)) -I. -c $(CFLAGS) $(ARCH_CFLAGS) $< -o $@ -MMD -MT $@

$(HV_OBJDIR)/$(HV_DEBUG_FILE).out: $(MODULES) $(HV_OBJDIR)/symtab.o
	$(Q)echo "cc        $(notdir $@)"
	$(Q)${BASH} ${LD_IN_TOOL} $(ARCH_LDSCRIPT_IN) $(ARCH_LDSCRIPT) ${HV_CONFIG_MK}
	$(Q)$(CC) -Wl,-Map=$(HV_OBJDIR)/$(HV_DEBUG_FILE).map -o $@ $(LDFLAGS) $(ARCH_LDFLAGS) -T$(ARCH_LDSCRIPT) \
		-Wl,--start-group $(HV_OBJDIR)/symtab.o $(MODULES) -Wl,--end-group

.PHONY: tags
tags:
	$(Q)ctags --languages=Asm,c,c++ -R

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
$(VERSION): $(HV_CONFIG_H)
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
	if [ x$(CONFIG_RELEASE) = "xy" ];then BUILD_TYPE="REL";else BUILD_TYPE="DBG";fi;\
	echo "/*" > $(VERSION); \
	sed 's/^/ * /' "$(LICENSE_FILE)" >> $(VERSION); \
	echo " */" >> $(VERSION); \
	echo "" >> $(VERSION); \
	echo "#ifndef VERSION_H" >> $(VERSION); \
	echo "#define VERSION_H" >> $(VERSION); \
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
	echo "cc        $(notdir $@)"; \
	$(CC) $(patsubst %, -I%, $(INCLUDE_PATH)) -I. -c $(CFLAGS) $(ARCH_CFLAGS) $< -o $@ -MMD -MT $@

$(VM_CFG_C_SRCS): %.c: $(HV_CONFIG_TIMESTAMP)

$(VM_CFG_C_OBJS): $(HV_OBJDIR)/%.o: %.c $(HEADERS) $(ARCH_PRE_BUILD_TARGETS)
	$(Q)[ ! -e $@ ] && mkdir -p $(dir $@) && mkdir -p $(HV_MODDIR); \
	echo "cc        $(notdir $@)"; \
	$(CC) $(patsubst %, -I%, $(INCLUDE_PATH)) -I. -c $(CFLAGS) $(ARCH_CFLAGS) $< -o $@ -MMD -MT $@

ifeq ($(ARCH),arm64)
sdk/image/linux/vm1/beau-linux.dtb: sdk/image/linux/vm1/beau-linux.dts
	$(Q)echo "dtc       $@"
	$(Q)$(DTC) -I dts -O dtb -o $@ $<

sdk/image/linux/vm2/beau-linux.dtb: sdk/image/linux/vm2/beau-linux.dts
	$(Q)echo "dtc       $@"
	$(Q)$(DTC) -I dts -O dtb -o $@ $<

$(HV_OBJDIR)/arch/arm64/platform/$(PLATFORM)/platform.o: sdk/image/lk.bin sdk/image/zephyr.bin $(ARM64_PLATFORM_DTB)
$(HV_OBJDIR)/arch/arm64/platform/qemu/platform.o: sdk/image/linux/vm1/beau-linux.dtb sdk/image/linux/vm2/beau-linux.dtb
endif

$(HV_OBJDIR)/%.o: %.S $(HEADERS) $(ARCH_PRE_BUILD_TARGETS)
	$(Q)[ ! -e $@ ] && mkdir -p $(dir $@) && mkdir -p $(HV_MODDIR); \
	echo "cc        $(notdir $@)"; \
	$(CC) $(patsubst %, -I%, $(INCLUDE_PATH)) -I. $(ASFLAGS) $(ARCH_ASFLAGS) -c $< -o $@ -MMD -MT $@

.DEFAULT_GOAL := all
