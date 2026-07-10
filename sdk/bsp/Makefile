#
# ACRN Hypervisor Static Library for Debug Features
#

FILE_PATH := $(dir $(MKFL_NAME))
SRCS += $(filter-out $(FILE_PATH)/ns16550.c $(FILE_PATH)/pl011.c,$(wildcard $(FILE_PATH)/*.c))
SRCS += $(FILE_PATH)/pl011.c
SRCS += $(wildcard $(FILE_PATH)/arm64/*.c)
SRCS += $(wildcard $(FILE_PATH)/virtio/*.c)
OBJS += $(patsubst %.c,$(HV_OBJDIR)/%.o,$(SRCS))

.PHONY: default
default: $(LIB_BSP)

ifneq ($(CONFIG_RELEASE),y)
$(LIB_BSP): $(OBJS)
	@echo "ar                 $(notdir $@)"
	@$(AR) $(ARFLAGS) $(LIB_BSP) $(OBJS)
endif

.PHONY: clean
clean:
	@rm -f $(OBJS)

-include $(OBJS:.o=.d)

$(HV_OBJDIR)/%.o: %.c
	@[ ! -e $@ ] && mkdir -p $(dir $@) && mkdir -p $(HV_MODDIR); \
	echo "cc                 $(notdir $@)"; \
	$(CC) $(patsubst %, -I%, $(INCLUDE_PATH)) -I. -c $(CFLAGS) $(ARCH_CFLAGS) $< -o $@ -MMD -MT $@
