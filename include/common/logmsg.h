/*
 * Copyright (C) 2018-2022 Intel Corporation.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef LOGMSG_H
#define LOGMSG_H
#include <cpu.h>

/* Logging severity levels */
#define LOG_ERROR		3U
#define LOG_WARNING		4U
#define LOG_INFO		5U
#define LOG_DEBUG		6U

#define LOG_VT100_RESET		"\x1B[0m"
#define LOG_VT100_BOLD_RED	"\x1B[1;31m"
#define LOG_VT100_BOLD_YELLOW	"\x1B[1;33m"
#define LOG_VT100_BOLD_GREEN	"\x1B[1;32m"
#define LOG_VT100_BOLD_WHITE	"\x1B[1;37m"
#define LOG_VT100_BRIGHT_BLACK	"\x1B[90m"

#define LOG_ENTRY_SIZE	80U
/* Size of buffer used to store a message being logged,
 * should align to LOG_ENTRY_SIZE.
 */
#define LOG_MESSAGE_MAX_SIZE	(4U * LOG_ENTRY_SIZE)
#define LOG_TIMESTAMP_MAX_SIZE	32U

struct host_dmesg_record {
	uint64_t index;
	uint32_t sequence;
	uint16_t length;
	char message[LOG_MESSAGE_MAX_SIZE];
};

struct host_dmesg_stats {
	uint32_t capacity;
	uint32_t queued;
	uint64_t oldest;
	uint64_t next;
	uint64_t stored;
	uint64_t overwritten;
};

#define DBG_LEVEL_LAPICPT	5U

#if defined(HV_DEBUG)

void asm_assert(int32_t line, const char *file, const char *txt);

#define ASSERT(x, ...) \
	do { \
		if (!(x)) {\
			asm_assert(__LINE__, __FILE__, "fatal error");\
		} \
	} while (0)

#else /* HV_DEBUG */

#define ASSERT(x, ...)	do { } while (0)

#endif /* HV_DEBUG */

/*
 * @pre the severity > 0
 */
void do_logmsg(uint32_t severity, const char *fmt, ...);
void panic_dump_context(void);
const char *logmsg_severity_color(uint32_t severity);
/* Format uptime as HH:MM:SS.mmm,uuu to match SEAU/Zephyr log timestamps. */
void format_log_timestamp(char *buffer, size_t size, uint64_t timestamp_us);
bool daemon_log(uint32_t severity, const char *fmt, ...);
bool host_dmesg_get_stats(struct host_dmesg_stats *stats);
bool host_dmesg_read(uint64_t *cursor, struct host_dmesg_record *record,
	uint64_t *skipped);

/** The well known printf() function.
 *
 *  Formats a string and writes it to the console output.
 *
 *  @param fmt A pointer to the NUL terminated format string.
 *
 *  @return The number of characters actually written or a negative
 *          number if an error occurred.
 */

void printf(const char *fmt, ...);

/** The well known vprintf() function.
 *
 *  Formats a string and writes it to the console output.
 *
 *  @param fmt A pointer to the NUL terminated format string.
 *  @param args The variable long argument list as va_list.
 *  @return The number of characters actually written or a negative
 *          number if an error occurred.
 */

void vprintf(const char *fmt, va_list args);

#ifndef LOG_PREFIX
#define LOG_PREFIX
#endif

/* LOG_* is the single BEAU system log API. Console output adds color by severity. */
#define LOG_ERR(...)						\
	do {							\
		do_logmsg(LOG_ERROR, LOG_PREFIX __VA_ARGS__);	\
	} while (0)

#define LOG_WRN(...)						\
	do {							\
		do_logmsg(LOG_WARNING, LOG_PREFIX __VA_ARGS__);	\
	} while (0)

#define LOG_INF(...)						\
	do {							\
		do_logmsg(LOG_INFO, LOG_PREFIX __VA_ARGS__);	\
	} while (0)

#define LOG_DBG(...)						\
	do {							\
		do_logmsg(LOG_DEBUG, LOG_PREFIX __VA_ARGS__);	\
	} while (0)

#define dev_dbg(lvl, ...)					\
	do {							\
		if ((lvl) > 0) {                                \
			do_logmsg((lvl), LOG_PREFIX __VA_ARGS__);\
		}                                               \
	} while (0)

#define panic(...) 							\
	do { LOG_ERR("panic: %s @ %s:%d\n", __func__, __FILE__, __LINE__);	\
		LOG_ERR(__VA_ARGS__); 					\
		panic_dump_context();					\
		while (1) { asm_pause(); }; } while (0)

#endif /* LOGMSG_H */
