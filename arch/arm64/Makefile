CROSS_COMPILE ?= aarch64-linux-gnu-

CC      = $(CROSS_COMPILE)gcc
AS      = $(CROSS_COMPILE)as
AR      = $(CROSS_COMPILE)ar
LD      = $(CROSS_COMPILE)ld
OBJCOPY = $(CROSS_COMPILE)objcopy

ARCH_LDSCRIPT_IN = arch/lds/link_ram.ld.in

CFLAGS += -std=gnu99
CFLAGS += -DCONFIG_ARM64
CFLAGS += -DSMP
ifeq ($(CONFIG_ARM64_GICV5),y)
CFLAGS += -DCONFIG_ARM64_GICV5=1
endif

LDFLAGS += -Wl,--no-warn-rwx-segments
LDFLAGS += -Wl,-z,norelro
LDFLAGS += -Wl,-z,noexecstack

ifeq (y, $(CONFIG_RELOC))
CFLAGS += -fpie
ASFLAGS += -fpie
CFLAGS += -fvisibility=hidden
LDFLAGS += -shared
LDFLAGS += -Wl,-z,notext
LDFLAGS += -Wl,-Bsymbolic
LDFLAGS += -Wl,-z,defs
else
CFLAGS += -fno-pie -static
ASFLAGS += -fno-pie -static
LDFLAGS += -static
endif

ARCH_CFLAGS += -mgeneral-regs-only
ARCH_CFLAGS += -mstrict-align
ARCH_CFLAGS += -fno-omit-frame-pointer
ARCH_CFLAGS += -mno-outline-atomics

ARCH_ASFLAGS +=

ARM64_OFFSETS_SRC := arch/arm64/offsets/offsets.c
ARM64_OFFSETS_OBJ := $(HV_OBJDIR)/arch/arm64/offsets/offsets.o
ARM64_OFFSETS_DEP := $(ARM64_OFFSETS_OBJ:.o=.d)
ARM64_OFFSETS_H := $(HV_OBJDIR)/include/generated/offsets.h
ARM64_OFFSETS_GENERATOR := scripts/gen_offset_header.py
ARM64_OFFSETS_SYMBOLS := scripts/arm64_offset_symbols.txt

ARCH_PRE_BUILD_TARGETS += $(ARM64_OFFSETS_H)

-include $(ARM64_OFFSETS_DEP)

.PHONY: offsets
offsets: $(ARM64_OFFSETS_H)

$(ARM64_OFFSETS_OBJ): $(ARM64_OFFSETS_SRC) $(HV_CONFIG_H) $(HV_CONFIG_TIMESTAMP) | $(HV_OBJDIR)/include/generated
	$(Q)mkdir -p $(dir $@)
	$(Q)echo "CC                 $(notdir $@)"
	$(Q)$(CC) $(patsubst %, -I%, $(INCLUDE_PATH)) -I. -c $(CFLAGS) \
		$(ARCH_CFLAGS) -fno-stack-protector $< -o $@ -MMD -MT $@

$(ARM64_OFFSETS_H): $(ARM64_OFFSETS_OBJ) $(ARM64_OFFSETS_GENERATOR) $(ARM64_OFFSETS_SYMBOLS) | $(HV_OBJDIR)/include/generated
	$(Q)echo "OFFSETS            $(notdir $@)"
	$(Q)python3 $(ARM64_OFFSETS_GENERATOR) --input $< --output $@ \
		--symbols $(ARM64_OFFSETS_SYMBOLS)

ifeq (y, $(CONFIG_AARCH64_IMAGE_HEADER))
HOST_S_SRCS += arch/arm64/boot/header.S
ARCH_ASFLAGS += -DCONFIG_AARCH64_IMAGE_HEADER=1
LDFLAGS += -Wl,-u,_image_header
endif
HOST_S_SRCS += arch/arm64/boot/entry.S
HOST_S_SRCS += arch/arm64/vector.S
HOST_S_SRCS += arch/arm64/sched.S
HOST_S_SRCS += arch/arm64/suspend.S
HOST_S_SRCS += arch/arm64/lib/aes.S
HOST_S_SRCS += arch/arm64/ddb/ddb_probe.S

HOST_C_SRCS += arch/arm64/init.c
HOST_C_SRCS += arch/arm64/coredump.c
HOST_C_SRCS += arch/arm64/ddb/ddb.c
HOST_C_SRCS += arch/arm64/ddb/ddb_console.c
HOST_C_SRCS += arch/arm64/ddb/ddb_mem.c
HOST_C_SRCS += arch/arm64/ddb/ddb_stack.c
HOST_C_SRCS += arch/arm64/boot/reloc.c
HOST_C_SRCS += arch/arm64/lib/random.c
HOST_C_SRCS += arch/arm64/cpu.c
HOST_C_SRCS += arch/arm64/psci.c
HOST_C_SRCS += arch/arm64/mmu.c
ifeq ($(CONFIG_ARM64_MTE),y)
HOST_C_SRCS += arch/arm64/mte.c
endif
HOST_C_SRCS += arch/arm64/cache.c
HOST_C_SRCS += arch/arm64/pgtable.c
HOST_C_SRCS += arch/arm64/irq.c
HOST_C_SRCS += arch/arm64/panic.c
ifeq ($(CONFIG_ARM64_GICV5),y)
HOST_C_SRCS += arch/arm64/gic/gicv5.c
HOST_C_SRCS += arch/arm64/gic/gicv5_its.c
HOST_C_SRCS += arch/arm64/gic/gicv5_iwb.c
else
HOST_C_SRCS += arch/arm64/gic/gicv3.c
HOST_C_SRCS += arch/arm64/gic/gicv3_its.c
endif
HOST_C_SRCS += arch/arm64/timer.c
HOST_C_SRCS += arch/arm64/notify.c
HOST_C_SRCS += arch/arm64/trap.c
HOST_C_SRCS += arch/arm64/security.c
HOST_C_SRCS += arch/arm64/pm.c
HOST_C_SRCS += arch/arm64/pmu.c
HOST_C_SRCS += arch/arm64/sve.c
HOST_C_SRCS += arch/arm64/mpu.c
HOST_C_SRCS += arch/arm64/smmu/smmu.c
HOST_C_SRCS += arch/arm64/smmu/iommu.c
ifeq ($(PLATFORM),qemu)
HOST_C_SRCS += arch/arm64/platform/qemu/pm.c
endif
ifeq ($(CONFIG_CPUFREQ),y)
HOST_C_SRCS += arch/arm64/platform/$(PLATFORM)/cpufreq.c
endif

VP_S_SRCS +=
VP_S_SRCS += arch/arm64/guest/entry.S

VP_C_SRCS += arch/arm64/guest/memory.c
VP_C_SRCS += arch/arm64/guest/vcpu.c
VP_C_SRCS += arch/arm64/guest/vmpu.c
VP_C_SRCS += arch/arm64/guest/vsve.c
VP_C_SRCS += arch/arm64/guest/vipc.c
VP_C_SRCS += arch/arm64/guest/vm.c
VP_C_SRCS += arch/arm64/guest/vcpu_exit.c
VP_C_SRCS += arch/arm64/guest/vpsci.c
VP_C_SRCS += arch/arm64/guest/virq.c
VP_C_SRCS += arch/arm64/guest/vgicv3.c
VP_C_SRCS += arch/arm64/guest/vgicv3_its.c
VP_C_SRCS += arch/arm64/guest/vtimer.c
VP_C_SRCS += arch/arm64/guest/vpl011.c
VP_C_SRCS += arch/arm64/guest/vsmmu.c
ifeq ($(STATIC_ARM64_PLATFORM),y)
VM_CFG_S_SRCS += arch/arm64/platform/$(PLATFORM)/platform.S
else
VM_CFG_C_SRCS += $(SCENARIO_CFG_DIR)/vm_configurations.c
VM_CFG_C_SRCS += $(BOARD_CFG_DIR)/pci_dev.c
VM_CFG_C_SRCS += $(BOARD_CFG_DIR)/pt_intx.c
VM_CFG_C_SRCS += $(BOARD_INFO_DIR)/board.c
VM_CFG_C_SRCS += $(BOARD_INFO_DIR)/bare.c
endif

HOST_C_OBJS := $(patsubst %.c,$(HV_OBJDIR)/%.o,$(HOST_C_SRCS))
HOST_S_OBJS := $(patsubst %.S,$(HV_OBJDIR)/%.o,$(HOST_S_SRCS))
VP_C_OBJS := $(patsubst %.c,$(HV_OBJDIR)/%.o,$(VP_C_SRCS))
VP_S_OBJS := $(patsubst %.S,$(HV_OBJDIR)/%.o,$(VP_S_SRCS))
VM_CFG_C_OBJS := $(patsubst %.c,$(HV_OBJDIR)/%.o,$(VM_CFG_C_SRCS))
VM_CFG_S_OBJS := $(patsubst %.S,$(HV_OBJDIR)/%.o,$(VM_CFG_S_SRCS))

HOST_MOD := $(HV_MODDIR)/arch.a
VP_MOD := $(HV_MODDIR)/libvmm.a

MODULES += $(HOST_MOD)
MODULES += $(VP_MOD)

$(HOST_MOD): $(HOST_C_OBJS) $(HOST_S_OBJS)
	$(Q)echo "AR                 $(notdir $@)"
	$(Q)rm -f $@
	$(Q)$(AR) $(ARFLAGS) $(HOST_MOD) $(HOST_C_OBJS) $(HOST_S_OBJS)

$(VP_MOD): $(VP_C_OBJS) $(VP_S_OBJS) $(VM_CFG_C_OBJS) $(VM_CFG_S_OBJS)
	$(Q)echo "AR                 $(notdir $@)"
	$(Q)$(AR) $(ARFLAGS) $(VP_MOD) $(VP_C_OBJS) $(VP_S_OBJS) $(VM_CFG_C_OBJS) $(VM_CFG_S_OBJS)
