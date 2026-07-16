/*
 * Copyright (C) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef ARM64_LD_SYM_H
#define ARM64_LD_SYM_H

extern const char	_text_start;
extern const char	_text_end;

extern const char	_rodata_start;
extern const char	_rodata_end;

extern char		_data_start;
extern char		_data_end;

extern char		_bss_start;
extern char		_bss_end;

extern char		ld_ram_start;
extern char		ld_image_end;
extern char		ld_ram_end;

#endif /* ARM64_LD_SYM_H */
