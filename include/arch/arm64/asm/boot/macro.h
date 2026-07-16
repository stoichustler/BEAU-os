/*
 * Copyright (C) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef ARM64_MACRO_H
#define ARM64_MACRO_H

#define FUNC(fn)         \
	.global fn;          \
	.type fn, %function; \
fn:

#define END(fn)         \
	.size fn, . - fn

#endif /* ARM64_MACRO_H */
