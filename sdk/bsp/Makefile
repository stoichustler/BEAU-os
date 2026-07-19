#
# ACRN Hypervisor Static Library for Debug Features
#

FILE_PATH := $(dir $(MKFL_NAME))
SRCS += $(filter-out $(FILE_PATH)/ns16550.c $(FILE_PATH)/pl011.c,$(wildcard $(FILE_PATH)/*.c))
SRCS += $(FILE_PATH)/pl011.c
SRCS += $(wildcard $(FILE_PATH)/arm64/*.c)
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

ifneq ($(CONFIG_RELEASE),y)
$(LIB_BSP): $(OBJS)
	@echo "AR                 $(notdir $@)"
	@$(AR) $(ARFLAGS) $(LIB_BSP) $(OBJS)
endif

.PHONY: clean
clean:
	@rm -f $(OBJS)

-include $(OBJS:.o=.d)

$(HV_OBJDIR)/%.o: %.c
	@[ ! -e $@ ] && mkdir -p $(dir $@) && mkdir -p $(HV_MODDIR); \
	echo "CC                 $(notdir $@)"; \
	$(CC) $(patsubst %, -I%, $(INCLUDE_PATH)) -I. -c $(CFLAGS) $(ARCH_CFLAGS) $< -o $@ -MMD -MT $@
