/*
 * Copyright (C) 2026 The BEAU OS Authors
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <stdbool.h>

void platform_dputc(char c) {
    (void)c;
}

int platform_dgetc(char* c, bool wait) {
    (void)c;
    (void)wait;
    return -1;
}
