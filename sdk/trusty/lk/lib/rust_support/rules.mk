#
# Copyright (c) 2024, Google, Inc. All rights reserved
#
# Permission is hereby granted, free of charge, to any person obtaining
# a copy of this software and associated documentation files
# (the "Software"), to deal in the Software without restriction,
# including without limitation the rights to use, copy, modify, merge,
# publish, distribute, sublicense, and/or sell copies of the Software,
# and to permit persons to whom the Software is furnished to do so,
# subject to the following conditions:
#
# The above copyright notice and this permission notice shall be
# included in all copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
# EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
# MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
# IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
# CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
# TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
# SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
#

LOCAL_DIR := $(GET_LOCAL_DIR)

MODULE := $(LOCAL_DIR)

MODULE_CRATE_NAME := rust_support

MODULE_SRCS := \
	$(LOCAL_DIR)/lib.rs \

# Don't make this module depend on itself.
MODULE_ADD_IMPLICIT_DEPS := false

MODULE_DEPS := \
	external/rust/crates/num-derive \
	external/rust/crates/num-traits \
	external/rust/crates/log \
	trusty/user/base/lib/liballoc-rust \
	trusty/user/base/lib/libcompiler_builtins-rust \
	trusty/user/base/lib/libcore-rust \
	trusty/user/base/lib/trusty-std \
	$(LOCAL_DIR)/wrappers \

MODULE_BINDGEN_ALLOW_FUNCTIONS := \
	_panic \
	fflush \
	fputs \
	ipc_get_msg \
	ipc_port_connect_async \
	ipc_put_msg \
	ipc_read_msg \
	ipc_send_msg \
	lk_stdin \
	lk_stdout \
	lk_stderr \
	mutex_acquire_timeout \
	mutex_destroy \
	mutex_init \
	mutex_release \
	thread_create \
	thread_resume \
	vaddr_to_paddr \
	vmm_alloc_physical_etc \
	vmm_alloc_contiguous \
	vmm_free_region \

MODULE_BINDGEN_ALLOW_TYPES := \
	Error \
	iovec_kern \
	ipc_msg_.* \
	lk_init_.* \
	trusty_ipc_event_type \

MODULE_BINDGEN_ALLOW_VARS := \
	.*_PRIORITY \
	_kernel_aspace \
	ARCH_MMU_FLAG_.* \
	DEFAULT_STACK_SIZE \
	FILE \
	IPC_CONNECT_WAIT_FOR_PORT \
	IPC_HANDLE_POLL_.* \
	NUM_PRIORITIES \
	PAGE_SIZE \
	PAGE_SIZE_SHIFT \
	zero_uuid \

MODULE_BINDGEN_FLAGS := \
	--newtype-enum Error \
	--newtype-enum lk_init_level \
	--bitfield-enum lk_init_flags \
	--no-prepend-enum-name \
	--with-derive-custom Error=FromPrimitive \

MODULE_BINDGEN_SRC_HEADER := $(LOCAL_DIR)/bindings.h

include make/module.mk
