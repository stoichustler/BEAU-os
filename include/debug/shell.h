/*
 * Copyright (C) 2018-2022 Intel Corporation.
 * Copyright (C) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef SHELL_H
#define SHELL_H

#include <types.h>

#define SHELL_COLOR_RESET	"\033[0m"
#define SHELL_COLOR_RED		"\033[31m"
#define SHELL_COLOR_GREEN	"\033[32m"
#define SHELL_COLOR_YELLOW	"\033[33m"
#define SHELL_COLOR_BLUE	"\033[34m"
#define SHELL_COLOR_MAGENTA	"\033[35m"
#define SHELL_COLOR_CYAN	"\033[36m"
#define SHELL_COLOR_WHITE	"\033[37m"
#define SHELL_COLOR_GREY	"\033[90m"

void shell_init(void);
void shell_start(void);
void arm64_rttest_init(void);
void oslat_init(void);
void ipilat_init(void);
#ifdef CONFIG_COREMARK
void coremark_init(void);
#endif
#ifdef CONFIG_DHRYSTONE
void dhrystone_init(void);
#endif
void shell_kick(void);
bool shell_is_open(void);
bool shell_async_puts(const char *string_ptr);
bool shell_async_puts_raw(const char *string_ptr);

#endif /* SHELL_H */
