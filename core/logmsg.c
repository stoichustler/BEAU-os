/*
 * Copyright (C) 2018-2025 Intel Corporation.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <types.h>
#include <per_cpu.h>
#include <atomic.h>
#include <sprintf.h>
#include <logmsg.h>
#include <ticks.h>
#include <console.h>
#include <npk_log.h>

/* [20260710] log service principle:
 *
 * do_logmsg() is the common fanout point for BEAU OS diagnostics. It formats
 * one plain log line, then sends that same line to the enabled sinks: memory
 * sbuf, host console, and optional NPK backend. Sink policy is level-gated, but
 * formatting stays centralized so cross-sink correlation uses the same time,
 * pCPU, severity, and sequence fields.
 *
 *   LOG_* caller
 *        |
 *        v
 *   do_logmsg()
 *     - timestamp + cpu + severity + seq
 *        |
 *        +-- mem_log()     -> ACRN_HVLOG sbuf
 *        +-- console_log() -> active host console/shell
 *        +-- npk_log()     -> platform debug sink
 *
 * Console colors are presentation-only. Memory and NPK sinks receive the plain
 * BEAU prefix so tooling can parse the same message without terminal escapes.
 */

/* buf size should be identical to the size in hvlog option, which is
 * transfered to Service VM:
 * bsp/uefi/clearlinux/acrn.conf: hvlog=2M@0x1FE00000
 */

static int32_t log_seq = 0;

uint16_t mem_loglevel = CONFIG_MEM_LOGLEVEL_DEFAULT;

__attribute__((weak)) void panic_dump_context(void)
{
}

#define LOG_USEC_PER_MSEC	1000UL
#define LOG_USEC_PER_SEC	1000000UL
#define LOG_SEC_PER_MIN		60UL
#define LOG_SEC_PER_HOUR	(60UL * LOG_SEC_PER_MIN)

const char *logmsg_severity_color(uint32_t severity)
{
	const char *color;

	/*
	 * VT100 SGR color is a console presentation concern. The log buffer itself
	 * keeps the plain BEAU prefix so memory/NPK logs remain machine-parseable:
	 *
	 *   do_logmsg() -> plain "[κ][time][cpu][sev][seq] msg"
	 *        console_log() wraps that line with severity color + reset
	 */
	switch (severity) {
	case LOG_FATAL:
		color = LOG_VT100_BOLD_INDIGO;
		break;
	case LOG_ERROR:
		color = LOG_VT100_BOLD_RED;
		break;
	case LOG_WARNING:
		color = LOG_VT100_BOLD_YELLOW;
		break;
	case LOG_INFO:
		color = LOG_VT100_BOLD_WHITE;
		break;
	case LOG_DEBUG:
	default:
		color = LOG_VT100_BRIGHT_BLACK;
		break;
	}

	return color;
}

void format_log_timestamp(char *buffer, size_t size, uint64_t timestamp_us)
{
	uint64_t hour;
	uint64_t total_seconds;
	uint32_t sec_of_hour;
	uint32_t min;
	uint32_t sec;
	uint32_t remainder_us;
	uint32_t msec;
	uint32_t usec;

	if ((buffer == NULL) || (size == 0U)) {
		return;
	}

	/* [20260708] Zephyr RTOS like timestamp format
	 *
	 * Timestamp decomposition: split the unbounded seconds counter from the sub-second
	 * remainder first. The bounded remainders keep every later operation small and
	 * avoid overflow-prone time-unit products.
	 */
	total_seconds = timestamp_us / LOG_USEC_PER_SEC;
	remainder_us = (uint32_t)(timestamp_us % LOG_USEC_PER_SEC);
	msec = remainder_us / LOG_USEC_PER_MSEC;
	usec = remainder_us % LOG_USEC_PER_MSEC;
	hour = total_seconds / LOG_SEC_PER_HOUR;
	sec_of_hour = (uint32_t)(total_seconds % LOG_SEC_PER_HOUR);
	min = sec_of_hour / LOG_SEC_PER_MIN;
	sec = sec_of_hour % LOG_SEC_PER_MIN;

	(void)snprintf(buffer, size, "%02lu:%02u:%02u.%03u,%03u",
		hour, min, sec, msec, usec);
}

static inline bool mem_need_log(uint32_t severity)
{
	return (severity <= mem_loglevel);
}

static void mem_log(uint16_t pcpu_id, char *buffer)
{
	uint32_t msg_len;
	struct shared_buf *sbuf = per_cpu(sbuf, pcpu_id)[ACRN_HVLOG];

	/* If sbuf is not ready, we just drop the massage */
	if (sbuf != NULL) {
		msg_len = strnlen_s(buffer, LOG_MESSAGE_MAX_SIZE);
		(void)sbuf_put_many(sbuf, LOG_ENTRY_SIZE, (uint8_t *)buffer,
			LOG_ENTRY_SIZE * (((msg_len - 1U) / LOG_ENTRY_SIZE) + 1));
	}
}

void do_logmsg(uint32_t severity, const char *fmt, ...)
{
	va_list args;
	uint64_t timestamp;
	uint16_t pcpu_id;
	char *buffer;
	char timestamp_str[LOG_TIMESTAMP_MAX_SIZE];

	if (!mem_need_log(severity) && !console_need_log(severity) && !npk_need_log(severity)) {
		return;
	}

	/* Get time-stamp value */
	timestamp = cpu_ticks();

	/* Scale time-stamp appropriately */
	timestamp = ticks_to_us(timestamp);
	format_log_timestamp(timestamp_str, sizeof(timestamp_str), timestamp);

	/* Get CPU ID */
	pcpu_id = get_pcpu_id();
	buffer = per_cpu(logbuf, pcpu_id);

	(void)memset(buffer, 0U, LOG_MESSAGE_MAX_SIZE);
	/* Put time-stamp, CPU ID and severity into buffer */
	snprintf(buffer, LOG_MESSAGE_MAX_SIZE,
		"[κ][%s][cpu%hu][sev%u][seq%4u] ",
		timestamp_str, pcpu_id, severity, atomic_inc_return(&log_seq));

	/* Put message into remaining portion of local buffer */
	va_start(args, fmt);
	vsnprintf(buffer + strnlen_s(buffer, LOG_MESSAGE_MAX_SIZE),
		LOG_MESSAGE_MAX_SIZE
		- strnlen_s(buffer, LOG_MESSAGE_MAX_SIZE), fmt, args);
	va_end(args);

	/* Check whether output to memory */
	if (mem_need_log(severity)) {
		mem_log(pcpu_id, buffer);
	}

	/* Check whether output to stdout */
	if (console_need_log(severity)) {
		console_log(severity, buffer);
	}

	/* Check whether output to NPK */
	if (npk_need_log(severity)) {
		npk_log(buffer);
	}
}
