# Library-style Trusty rules use the existing LK static-module implementation.
MODULE_DEPS += $(MODULE_LIBRARY_DEPS) $(MODULE_LIBRARY_EXPORTED_DEPS)
GLOBAL_INCLUDES += $(MODULE_EXPORT_INCLUDES)

include make/module.mk
