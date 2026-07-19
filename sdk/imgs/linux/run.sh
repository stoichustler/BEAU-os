qemu-system-aarch64 \
	-nographic -M virt,gic-version=3,virtualization=on \
	-cpu cortex-a57 \
	-smp 4 -m 4G \
	-kernel Image \
	-append "rdinit=/init console=ttyAMA0 loglevel=7" \
	-initrd Initramfs.cpio.gz

