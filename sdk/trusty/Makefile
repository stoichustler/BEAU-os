# SPDX-License-Identifier: BSD-3-Clause

ROOT := $(abspath $(CURDIR)/../..)
OUT := $(CURDIR)/out/qemu
LK_BUILDROOT := $(OUT)/lk-build
LK_BIN := $(OUT)/lk.bin
BEAU_BL33 ?= $(ROOT)/out/qemu_out/beau.debug.bin
TF_A_BUILD := $(OUT)/tf-a-build
TF_A_FIRMWARE := $(OUT)/tf-a/qemu_fw.bios
CROSS_COMPILE ?= aarch64-none-elf-

.PHONY: lk tfa firmware clean check-bl33

check-bl33:
	@test -s "$(BEAU_BL33)" || \
		{ echo "BEAU BL33 raw image not found: $(BEAU_BL33)"; exit 1; }

lk:
	@mkdir -p "$(OUT)"
	@$(MAKE) -C lk PROJECT=qemu-arm64 BUILDROOT="$(LK_BUILDROOT)" \
		ARCH_arm64_TOOLCHAIN_PREFIX="$(CROSS_COMPILE)"
	@cp "$(LK_BUILDROOT)/build-qemu-arm64/lk.bin" "$(LK_BIN)"

tfa: check-bl33 lk
	@mkdir -p "$(OUT)/tf-a"
	@$(MAKE) -C tfa BUILD_BASE="$(TF_A_BUILD)" PLAT=qemu DEBUG=1 \
		SPD=trusty TRUSTY_SPD_WITH_GENERIC_SERVICES=1 \
		QEMU_USE_GIC_DRIVER=QEMU_GICV3 \
		ENABLE_SVE_FOR_NS=0 ENABLE_SME_FOR_NS=0 \
		BL32="$(LK_BIN)" BL33="$(BEAU_BL33)" \
		PRELOADED_BL33_BASE=0x50000000 \
		CROSS_COMPILE="$(CROSS_COMPILE)" qemu_fw.bios
	@cp "$(TF_A_BUILD)/qemu/debug/qemu_fw.bios" "$(TF_A_FIRMWARE)"

firmware: tfa

clean:
	@rm -rf "$(OUT)"
