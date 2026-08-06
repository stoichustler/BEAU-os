# BEAU Linux Vsock Frontend

This directory retains the complete Linux virtio-vsock frontend source used by
BEAU VM2 and VM3.  It is intentionally separate from `sdk/kbe`, which owns the
VM1 HVC backend.

## Files

- `virtio_transport.c`: complete virtio-vsock frontend, including deferred
  AF_VSOCK core registration at virtio device probe time.
- `Kconfig`, `Makefile`: complete `net/vmw_vsock` integration files with
  `CONFIG_BEAU_VSOCKETS` enabled for the paired VM1 backend.
- `beau-vsock-test.c`: static userspace echo server/client used by the
  initramfs validation command.

## Porting To A Linux Tree

1. Copy `sdk/kbe/beau_transport.c` and `sdk/kbe/beau_vsock.h` to
   `net/vmw_vsock`.
2. Copy `virtio_transport.c`, `Kconfig`, and `Makefile` from this directory
   to the same Linux subsystem.
3. Enable `CONFIG_VSOCKETS`, `CONFIG_VIRTIO_VSOCKETS`, and
   `CONFIG_BEAU_VSOCKETS` in the shared Linux Image.
4. Put a `beau,vsock-backend` node with CID 3 only in VM1's DTB.  Put standard
   virtio-mmio vsock nodes in VM2 and VM3 DTBs with CIDs 4 and 5.

VM1 selects the BEAU HVC backend from its DTB.  VM2 and VM3 have virtio-vsock
devices, so the retained frontend registers the normal AF_VSOCK transport when
each device probes.  One Linux Image can therefore serve all three roles.
