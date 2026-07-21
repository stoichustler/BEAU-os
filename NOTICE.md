# Notices

BEAU is based on the Project ACRN hypervisor source tree. The upstream ACRN
source is licensed under the BSD 3-Clause License.

The ARM64 QEMU porting work and local BEAU-specific files marked with
`Copyright (C) 2026 Hustler Lo.` are licensed under the same BSD 3-Clause
License unless an individual file says otherwise.

Third-party and inherited components keep their original notices:

- Project ACRN derived code: BSD 3-Clause.
- FreeBSD/NetApp/other inherited device-model code: permissive BSD-style
  notices embedded in each source file.
- libfdt under `lib/fdt` and `include/lib/libfdt`: `GPL-2.0-or-later OR
  BSD-2-Clause`; BEAU should be distributed under the BSD-2-Clause option for
  this component to avoid GPL copyleft obligations. See
  `LICENSES/BSD-2-Clause.txt`.
- Mbed TLS subset under `lib/crypto/mbedtls`: Apache License 2.0. See
  `LICENSES/Apache-2.0.txt`.
- emlearn under `sdk/ai-sched/emlearn`: MIT License. See
  `sdk/ai-sched/emlearn/LICENSE.md` and `sdk/ai-sched/UPSTREAM.md`.

License-risk summary: no GPL-only source was found in the current tree. The
main compliance risk was missing top-level license/notice documentation after
the project rename. Keep upstream copyright and SPDX headers intact when
changing ACRN-derived or third-party files.
