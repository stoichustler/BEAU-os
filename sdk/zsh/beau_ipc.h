/*
 * Copyright (c) 2026 Hustler Lo.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef BEAU_IPC_H
#define BEAU_IPC_H

#include <stdint.h>

int beau_ipc_exchange(const uint8_t *request, uint16_t request_len,
	uint8_t *reply, uint16_t reply_capacity, uint16_t *reply_len,
	uint16_t *peer_vmid, int32_t timeout_ms);

#endif /* BEAU_IPC_H */
