#
# ACRN Hypervisor Static Library for Debug Features
#

FILE_PATH := $(dir $(MKFL_NAME))
-include $(HV_OBJDIR)/configs/config.mk
SRCS += $(filter-out $(FILE_PATH)/ns16550.c $(FILE_PATH)/pl011.c,$(wildcard $(FILE_PATH)/*.c))
SRCS += $(FILE_PATH)/pl011.c
SRCS += $(wildcard $(FILE_PATH)/arm64/*.c)
SRCS += $(wildcard $(FILE_PATH)/cmds/*.c)
SRCS += $(FILE_PATH)/benchmark/rt-tests/rt-test.c
SRCS += $(FILE_PATH)/benchmark/rt-tests/oslat.c
SRCS += $(FILE_PATH)/benchmark/rt-tests/ipilat.c
ifeq ($(CONFIG_COREMARK),y)
SRCS += $(FILE_PATH)/benchmark/coremark/coremark_entry.c
SRCS += $(FILE_PATH)/benchmark/coremark/coremark_engine.c
SRCS += $(FILE_PATH)/benchmark/coremark/coremark_port.c
SRCS += $(FILE_PATH)/benchmark/coremark/coremark_runner.c
SRCS += $(FILE_PATH)/benchmark/coremark/core_list_join.c
SRCS += $(FILE_PATH)/benchmark/coremark/core_matrix.c
SRCS += $(FILE_PATH)/benchmark/coremark/core_state.c
SRCS += $(FILE_PATH)/benchmark/coremark/core_util.c
CFLAGS += -I$(FILE_PATH)/benchmark/coremark
endif
ifeq ($(CONFIG_DHRYSTONE),y)
SRCS += $(FILE_PATH)/benchmark/dhrystone/dhrystone_1.c
SRCS += $(FILE_PATH)/benchmark/dhrystone/dhrystone_2.c
SRCS += $(FILE_PATH)/benchmark/dhrystone/dhrystone_port.c
SRCS += $(FILE_PATH)/benchmark/dhrystone/dhrystone_runner.c
CFLAGS += -I$(FILE_PATH)/benchmark/dhrystone
endif
SRCS += $(wildcard $(FILE_PATH)/virtio/*.c)
SRCS += $(wildcard $(FILE_PATH)/pci/*.c)
SRCS += $(FILE_PATH)/vpci/vpci_core.c
SRCS += $(FILE_PATH)/vpci/vpci_pt.c
SRCS += $(FILE_PATH)/vpci/vpci_msi.c
SRCS += $(FILE_PATH)/vpci/vpci_rc.c
SRCS += $(FILE_PATH)/vpci/vpci_sriov.c
OBJS += $(patsubst %.c,$(HV_OBJDIR)/%.o,$(SRCS))

.PHONY: default
default: $(LIB_BSP)

$(LIB_BSP): $(OBJS)
	@echo "AR                 $(notdir $@)"
	@$(AR) $(ARFLAGS) $(LIB_BSP) $(OBJS)

.PHONY: clean
clean:
	@rm -f $(OBJS)

-include $(OBJS:.o=.d)

$(HV_OBJDIR)/%.o: %.c
	@[ ! -e $@ ] && mkdir -p $(dir $@) && mkdir -p $(HV_MODDIR); \
	echo "CC                 $(notdir $@)"; \
	$(CC) $(patsubst %, -I%, $(INCLUDE_PATH)) -I. -c $(CFLAGS) $(ARCH_CFLAGS) $< -o $@ -MMD -MT $@
