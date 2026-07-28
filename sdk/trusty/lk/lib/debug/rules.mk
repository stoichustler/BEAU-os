LOCAL_DIR := $(GET_LOCAL_DIR)

MODULE := $(LOCAL_DIR)

MODULE_SRCS += \
	$(LOCAL_DIR)/debug.c

WITH_BACKTRACE ?= true

ifeq (true,$(call TOBOOL,$(WITH_BACKTRACE)))
MODULE_DEPS += \
	trusty/kernel/lib/backtrace \
GLOBAL_DEFINES += WITH_BACKTRACE=1
else
GLOBAL_DEFINES += WITH_BACKTRACE=0
endif

include make/module.mk
