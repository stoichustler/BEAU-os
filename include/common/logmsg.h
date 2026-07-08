/*
 * Copyright (C) 2018-2022 Intel Corporation.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef LOGMSG_H
#define LOGMSG_H
#include <cpu.h>

/* Logging severity levels */
#define LOG_FATAL		1U
#define LOG_ERROR		3U
#define LOG_WARNING		4U
#define LOG_INFO		5U
#define LOG_DEBUG		6U

#define LOG_VT100_RESET		"\x1B[0m"
#define LOG_VT100_BOLD_RED	"\x1B[1;31m"
#define LOG_VT100_BOLD_YELLOW	"\x1B[1;33m"
#define LOG_VT100_BOLD_WHITE	"\x1B[1;37m"
#define LOG_VT100_BRIGHT_BLACK	"\x1B[90m"
#define LOG_VT100_BOLD_INDIGO	"\x1B[1;38;5;54m"

#define LOG_ENTRY_SIZE	80U
/* Size of buffer used to store a message being logged,
 * should align to LOG_ENTRY_SIZE.
 */
#define LOG_MESSAGE_MAX_SIZE	(4U * LOG_ENTRY_SIZE)
#define LOG_TIMESTAMP_MAX_SIZE	32U

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
const char *logmsg_severity_color(uint32_t severity);
/* Format uptime as HH:MM:SS.mmm,uuu to match SEAU/Zephyr log timestamps. */
void format_log_timestamp(char *buffer, size_t size, uint64_t timestamp_us);

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
#define LOG_FTL(...)						\
	do {							\
		do_logmsg(LOG_FATAL, LOG_PREFIX __VA_ARGS__);	\
	} while (0)

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
	do { LOG_FTL("panic: %s line: %d\n", __func__, __LINE__);	\
		LOG_FTL(__VA_ARGS__); 					\
		while (1) { asm_pause(); }; } while (0)

#endif /* LOGMSG_H */
