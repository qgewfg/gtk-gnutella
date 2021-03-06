/*
 * Copyright (c) 2006 Christian Biere <christianbiere@gmx.de>
 * All rights reserved.
 *
 * Copyright (c) 2009-2011 Raphael Manfredi <Raphael_Manfredi@pobox.com>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the authors nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHORS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

/**
 * @ingroup lib
 * @file
 *
 * A ridiculously over-complicated crash handler.
 *
 * But incredibly useful...  The aim is to be able to capture as much
 * information as possible regarding the crash conditions, especially
 * in the field where core dumps are usually not allowed and where people
 * do not necessarily know how to launch a debugger anyway.
 *
 * There are three aspects to crash handling:
 *
 * - Logging of the crash condition (cause, call stack if possible).
 * - Capturing a debugging stack with local variable in case there is
 *   no persistent logging.
 * - Providing contextual side information that can assist forensics.
 *
 * Note that when crashing with an assertion failure, we usually already
 * have the stack trace, but crash handling is triggered anyway to collect
 * the debugging stack when no core dump is generated.
 *
 * To use the crash handler, the application must at least call crash_init(),
 * followed by some of the crash_setxxx() routines, closing the initialization
 * with crash_post_init().  All this initial configuration is saved in a
 * read-only memory region to prevent accidental corruption.
 *
 * Upon reception of a fatal signal, the crash_handler() routine is invoked.
 *
 * When using "fast assertions", there is also a hook to record the
 * fatal failing assertion through crash_assert_failure().
 *
 * Side information can be provided through crash hooks: these routines are
 * invoked when an assertion failure happens in a specific file.  The purpose
 * of the crash hook is to dump all the information it can to assist tracking
 * of the assertion failure.
 *
 * @author Christian Biere
 * @date 2006
 * @author Raphael Manfredi
 * @date 2009-2011
 */

#include "common.h"

#include "crash.h"
#include "atomic.h"
#include "ckalloc.h"
#include "compat_pause.h"
#include "compat_sleep_ms.h"
#include "fast_assert.h"
#include "fd.h"
#include "file.h"
#include "getcpucount.h"
#include "halloc.h"
#include "hashing.h"
#include "hashtable.h"
#include "iovec.h"
#include "log.h"
#include "mempcpy.h"
#include "offtime.h"
#include "omalloc.h"
#include "path.h"
#include "signal.h"
#include "stacktrace.h"
#include "str.h"
#include "stringify.h"
#include "timestamp.h"
#include "tm.h"
#include "unsigned.h"			/* For size_is_positive() */
#include "vmm.h"				/* For vmm_crash_mode() */
#include "xmalloc.h"

#include "override.h"			/* Must be the last header included */

#define PARENT_STDOUT_FILENO	3
#define PARENT_STDERR_FILENO	4
#define CRASH_MSG_MAXLEN		3072	/**< Pre-allocated max length */
#define CRASH_MSG_SAFELEN		512		/**< Failsafe static string */
#define CRASH_MIN_ALIVE			600		/**< secs, minimum uptime for exec() */

#ifdef HAS_FORK
#define has_fork() 1
#else
#define fork()     0
#define has_fork() 0
#endif

struct crash_vars {
	void *stack[STACKTRACE_DEPTH_MAX];	/**< Stack frame on assert failure */
	ckhunk_t *mem;			/**< Reserved memory, read-only */
	ckhunk_t *logck;		/**< Reserved memory, read-only */
	ckhunk_t *fmtck;		/**< Reserved memory, read-only */
	ckhunk_t *hookmem;		/**< Reserved memory, read-only */
	const char *argv0;		/**< The original argv[0]. */
	const char *progname;	/**< The program name */
	const char *exec_path;	/**< Path of program (optional, may be NULL) */
	const char *crashfile;	/**< Environment variable "Crashfile=..." */
	const char *cwd;		/**< Current working directory (NULL if unknown) */
	const char *crashdir;	/**< Directory where crash logs are written */
	const char *version;	/**< Program version string (NULL if unknown) */
	const assertion_data *failure;	/**< Failed assertion, NULL if none */
	const char *message;	/**< Additional error messsage, NULL if none */
	const char *filename;	/**< Filename where error occurred, NULL if node */
	pid_t pid;				/**< Initial process ID */
	time_delta_t gmtoff;	/**< Offset to GMT, supposed to be fixed */
	time_t start_time;		/**< Launch time (at crash_init() call) */
	size_t stackcnt;		/**< Valid stack items in stack[] */
	str_t *logstr;			/**< String to build and hold error message */
	str_t *fmtstr;			/**< String to allow log formatting during crash */
	hash_table_t *hooks;	/**< Records crash hooks by file name */
	const char * const *argv;	/**< Saved argv[] array */
	const char * const *envp;	/**< Saved environment array */
	int argc;				/**< Saved argv[] count */
	unsigned build;			/**< Build number, unique version number */
	uint8 major;			/**< Major version */
	uint8 minor;			/**< Minor version */
	uint8 patchlevel;		/**< Patchlevel version */
	uint8 crash_mode;		/**< True when we enter crash mode */
	uint8 recursive;		/**< True when we are in a recursive crash */
	uint8 closed;			/**< True when crash_close() was called */
	uint8 invoke_inspector;
	uint8 has_numbers;		/**< True if major/minor/patchlevel were inited */
	/* Not boolean fields because we need to update them individually */
	uint8 pause_process;
	uint8 dumps_core;
	uint8 may_restart;
	uint8 hooks_run;		/**< True when hooks have been run */
};

#define crash_set_var(name, src) \
G_STMT_START { \
	STATIC_ASSERT(sizeof(src) == sizeof(vars->name)); \
	ck_memcpy(vars->mem, (void *) &(vars->name), &(src), sizeof(vars->name)); \
	atomic_mb(); \
} G_STMT_END

static const struct crash_vars *vars; /**< read-only after crash_init()! */
static bool crash_closed;

/**
 * Signals that usually lead to a crash.
 */
static const int signals[] = {
#ifdef SIGBUS
	SIGBUS,
#endif
#ifdef SIGTRAP
	SIGTRAP,
#endif
	SIGABRT,
	SIGFPE,
	SIGILL,
	SIGSEGV,
};

/**
 * Determines whether coredumps are disabled.
 *
 * @return TRUE if enabled, FALSE if disabled, -1 if unknown or on error.
 */
int
crash_coredumps_disabled(void)
#if defined(HAS_GETRLIMIT) && defined(RLIMIT_CORE)
{
	struct rlimit lim;

	if (-1 != getrlimit(RLIMIT_CORE, &lim)) {
		/* RLIM_INFINITY could be negative, thus not > 0 */
		return 0 == lim.rlim_cur;
	}
	return -1;
}
#else
{
	errno = ENOTSUP;
	return -1;
}
#endif	/* HAS_GETRLIMIT && RLIMIT_CORE */

typedef struct cursor {
	char *buf;
	size_t size;
} cursor_t;

/**
 * Append positive value to buffer, formatted as "%02u".
 */
static G_GNUC_COLD void
crash_append_fmt_02u(cursor_t *cursor, long v)
{
	if (cursor->size < 2 || v < 0)
		return;

	if (v >= 100)
		v %= 100;

	if (v < 10) {
		*cursor->buf++ = '0';
		*cursor->buf++ = dec_digit(v);
	} else {
		int d = v % 10;
		int c = v /= 10;
		*cursor->buf++ = dec_digit(c);
		*cursor->buf++ = dec_digit(d);
	}
	cursor->size -= 2;
}

/**
 * Append positive value to buffer, formatted as "%u".
 */
static G_GNUC_COLD void
crash_append_fmt_u(cursor_t *cursor, unsigned long v)
{
	char buf[ULONG_DEC_BUFLEN];
	const char *s;
	size_t len;

	s = print_number(buf, sizeof buf, v);
	len = strlen(s);

	if (cursor->size < len)
		return;

	cursor->buf = mempcpy(cursor->buf, s, len);
	cursor->size -= len;
}

/**
 * Append a character to supplied buffer.
 */
static G_GNUC_COLD void
crash_append_fmt_c(cursor_t *cursor, unsigned char c)
{
	if (cursor->size < 1)
		return;

	*cursor->buf++ = c;
	cursor->size--;
}

/**
 * Fill supplied buffer with the current time formatted as yy-mm-dd HH:MM:SS
 * and should be at least 18-byte long or the string will be truncated.
 *
 * This routine can safely be used in a signal handler as it does not rely
 * on unsafe calls.
 */
G_GNUC_COLD void
crash_time(char *buf, size_t size)
{
	const size_t num_reserved = 1;
	struct tm tm;
	cursor_t cursor;
	time_delta_t gmtoff;

	/* We need at least space for a NUL */
	if (size < num_reserved)
		return;

	cursor.buf = buf;
	cursor.size = size - num_reserved;	/* Reserve one byte for NUL */

	/*
	 * If called very early from the logging layer, crash_init() may not have
	 * been invoked yet, so vars would still be NULL.
	 */

	gmtoff = (vars != NULL) ? vars->gmtoff : 0;

	if (!off_time(time(NULL) + gmtoff, 0, &tm)) {
		buf[0] = '\0';
		return;
	}

	crash_append_fmt_02u(&cursor, (TM_YEAR_ORIGIN + tm.tm_year) % 100);
	crash_append_fmt_c(&cursor, '-');
	crash_append_fmt_02u(&cursor, tm.tm_mon + 1);
	crash_append_fmt_c(&cursor, '-');
	crash_append_fmt_02u(&cursor, tm.tm_mday);
	crash_append_fmt_c(&cursor, ' ');
	crash_append_fmt_02u(&cursor, tm.tm_hour);
	crash_append_fmt_c(&cursor, ':');
	crash_append_fmt_02u(&cursor, tm.tm_min);
	crash_append_fmt_c(&cursor, ':');
	crash_append_fmt_02u(&cursor, tm.tm_sec);

	cursor.size += num_reserved;	/* We reserved one byte for NUL above */
	crash_append_fmt_c(&cursor, '\0');
}

/**
 * Fill supplied buffer with the current time formatted using the ISO format
 * yyyy-mm-dd HH:MM:SSZ and should be at least 21-byte long or the string
 * will be truncated.
 *
 * This routine can safely be used in a signal handler as it does not rely
 * on unsafe calls.
 */
G_GNUC_COLD void
crash_time_iso(char *buf, size_t size)
{
	const size_t num_reserved = 1;
	struct tm tm;
	cursor_t cursor;

	/* We need at least space for a NUL */
	if (size < num_reserved)
		return;

	cursor.buf = buf;
	cursor.size = size - num_reserved;	/* Reserve one byte for NUL */

	if (!off_time(time(NULL) + vars->gmtoff, 0, &tm)) {
		buf[0] = '\0';
		return;
	}

	crash_append_fmt_u(&cursor, TM_YEAR_ORIGIN + tm.tm_year);
	crash_append_fmt_c(&cursor, '-');
	crash_append_fmt_02u(&cursor, tm.tm_mon + 1);
	crash_append_fmt_c(&cursor, '-');
	crash_append_fmt_02u(&cursor, tm.tm_mday);
	crash_append_fmt_c(&cursor, ' ');
	crash_append_fmt_02u(&cursor, tm.tm_hour);
	crash_append_fmt_c(&cursor, ':');
	crash_append_fmt_02u(&cursor, tm.tm_min);
	crash_append_fmt_c(&cursor, ':');
	crash_append_fmt_02u(&cursor, tm.tm_sec);

	cursor.size += num_reserved;	/* We reserved one byte for NUL above */
	crash_append_fmt_c(&cursor, '\0');
}

/**
 * Fill supplied buffer with the current running time.
 *
 * This routine can safely be used in a signal handler as it does not rely
 * on unsafe calls.
 */
static G_GNUC_COLD void
crash_run_time(char *buf, size_t size)
{
	const size_t num_reserved = 1;
	time_delta_t t;
	cursor_t cursor;
	uint s;

	/* We need at least space for a NUL */
	if (size < num_reserved)
		return;

	t = delta_time(time(NULL), vars->start_time);
	s = MAX(t, 0);		/* seconds */

	cursor.buf = buf;
	cursor.size = size - num_reserved;	/* Reserve one byte for NUL */

	if (s > 86400) {
		crash_append_fmt_u(&cursor, s / 86400);
		crash_append_fmt_c(&cursor, 'd');
		crash_append_fmt_c(&cursor, ' ');
		crash_append_fmt_u(&cursor, (s % 86400) / 3600);
		crash_append_fmt_c(&cursor, 'h');
	} else if (s > 3600) {
		crash_append_fmt_u(&cursor, s / 3600);
		crash_append_fmt_c(&cursor, 'h');
		crash_append_fmt_c(&cursor, ' ');
		crash_append_fmt_u(&cursor, (s % 3600) / 60);
		crash_append_fmt_c(&cursor, 'm');
	} else if (s > 60) {
		crash_append_fmt_u(&cursor, s / 60);
		crash_append_fmt_c(&cursor, 'm');
		crash_append_fmt_c(&cursor, ' ');
		crash_append_fmt_u(&cursor, s % 60);
		crash_append_fmt_c(&cursor, 's');
	} else {
		crash_append_fmt_u(&cursor, s);
		crash_append_fmt_c(&cursor, 's');
	}

	cursor.size += num_reserved;	/* We reserved one byte for NUL above */
	crash_append_fmt_c(&cursor, '\0');
}

/**
 * Get the hook function that we have to run in order to log more context.
 *
 * @return the hook function to run, NULL if nothing.
 */
static G_GNUC_COLD crash_hook_t
crash_get_hook(void)
{
	const char *file;

	if (NULL == vars)
		return NULL;		/* No crash_init() yet */

	if (vars->recursive)
		return NULL;		/* Already recursed, maybe through hook? */

	/*
	 * File name can come from an assertion failure or from an explict
	 * call to crash_set_filename().
	 */

	if (vars->failure != NULL)
		file = vars->failure->file;
	else if (vars->filename != NULL)
		file = vars->filename;
	else
		file = NULL;

	if (NULL == file)
		return NULL;		/* Nothing to lookup against */

	return cast_pointer_to_func(hash_table_lookup(vars->hooks, file));
}

/**
 * Run crash hooks if we have an identified assertion failure.
 *
 * @param logfile		if non-NULL, redirect messages there as well.
 * @param logfd			if not -1, the opened file where we should log to
 */
static G_GNUC_COLD void
crash_run_hooks(const char *logfile, int logfd)
{
	crash_hook_t hook;
	const char *routine;
	char pid_buf[22];
	char time_buf[18];
	DECLARE_STR(7);
	int fd = logfd;

	hook = crash_get_hook();
	if (NULL == hook)
		return;

	if (vars != NULL && vars->hooks_run)
		return;		/* Prevent duplicate run */

	/*
	 * Let them know we're going to run a hook.
	 *
	 * Because we can be called from the child prorcess, we do not
	 * hardwire the stderr file descriptor but get it from the log layer.
	 */

	routine = stacktrace_function_name(hook);

	crash_time(time_buf, sizeof time_buf);
	print_str(time_buf);					/* 0 */
	print_str(" CRASH (pid=");				/* 1 */
	print_str(print_number(pid_buf, sizeof pid_buf, getpid()));	/* 2 */
	print_str(") ");						/* 3 */
	print_str(" invoking crash hook \"");	/* 4 */
	print_str(routine);						/* 5 */
	print_str("\"...\n");					/* 6 */
	flush_str(log_get_fd(LOG_STDERR));
	rewind_str(0);

	/*
	 * If there is a crash filename given, open it for appending and
	 * configure the stderr logfile with a duplicate logging to that file.
	 */

	if (logfile != NULL && -1 == logfd) {
		fd = open(logfile, O_WRONLY | O_APPEND, 0);
		if (-1 == fd) {
			crash_time(time_buf, sizeof time_buf);
			print_str(time_buf);					/* 0 */
			print_str(" WARNING: cannot reopen ");	/* 1 */
			print_str(logfile);						/* 2 */
			print_str(" for appending: ");			/* 3 */
			print_str(symbolic_errno(errno));		/* 4 */
			print_str("\n");						/* 5 */
			flush_str(log_get_fd(LOG_STDERR));
			rewind_str(0);
		}
	}

	/*
	 * Invoke hook, then log a message indicating we're done.
	 */

	if (-1 != fd) {
		log_set_duplicate(LOG_STDERR, fd);
		print_str("invoking crash hook \"");	/* 0 */
		print_str(routine);						/* 1 */
		print_str("\"...\n");					/* 2 */
		flush_str(fd);
		rewind_str(0);
	}

	(*hook)();

	crash_time(time_buf, sizeof time_buf);
	print_str(time_buf);					/* 0 */
	print_str(" CRASH (pid=");				/* 1 */
	print_str(print_number(pid_buf, sizeof pid_buf, getpid()));	/* 2 */
	print_str(") ");						/* 3 */
	print_str("done with hook \"");			/* 4 */
	print_str(routine);						/* 5 */
	print_str("\"\n");						/* 6 */
	flush_str(log_get_fd(LOG_STDERR));

	if (fd != -1) {
		rewind_str(0);
		print_str("done with hook \"");			/* 0 */
		print_str(routine);						/* 1 */
		print_str("\".\n");						/* 2 */
		flush_str(fd);
	}

	if (vars != NULL) {
		uint8 t = TRUE;
		crash_set_var(hooks_run, t);
	}

	/*
	 * We do not close the file if opened so as to continue logging
	 * duplicate information until the end should anyone call g_logv()
	 * or s_logv().
	 */
}

/**
 * Emit leading crash information: who crashed and why.
 */
static G_GNUC_COLD void
crash_message(const char *signame, bool trace, bool recursive)
{
	DECLARE_STR(11);
	char pid_buf[22];
	char time_buf[18];
	char runtime_buf[22];
	char build_buf[22];
	unsigned iov_prolog;

	crash_time(time_buf, sizeof time_buf);
	crash_run_time(runtime_buf, sizeof runtime_buf);

	/* The following precedes each line */
	print_str(time_buf);				/* 0 */
	print_str(" CRASH (pid=");			/* 1 */
	print_str(print_number(pid_buf, sizeof pid_buf, getpid()));	/* 2 */
	print_str(") ");					/* 3 */
	iov_prolog = getpos_str();

	print_str("for ");					/* 4 */
	if (vars->version != NULL) {
		print_str(vars->version);		/* 5 */
	} else {
		print_str(vars->progname);		/* 5 */
		if (0 != vars->build) {
			print_str(" build #");		/* 6 */
			print_str(print_number(build_buf, sizeof build_buf, vars->build));
		}
	}
	print_str("\n");					/* 8, at most */
	flush_err_str();
	if (log_stdout_is_distinct())
		flush_str(STDOUT_FILENO);

	rewind_str(iov_prolog);
	print_str("by ");					/* 4 */
	if (recursive)
		print_str("recursive ");		/* 5 */
	print_str(signame);					/* 6 */
	print_str(" after ");				/* 7 */
	print_str(runtime_buf);				/* 8 */
	if (vars->closed) {
		print_str(" during final exit()");	/* 9 */
	} else if (trace) {
		print_str(" -- stack was:");	/* 9 */
	}
	print_str("\n");					/* 10, at most */
	flush_err_str();
	if (log_stdout_is_distinct())
		flush_str(STDOUT_FILENO);
}

/**
 * Signal that we are attempting to print a decorated stack trace.
 */
static G_GNUC_COLD void
crash_decorating_stack(void)
{
	DECLARE_STR(5);
	char pid_buf[22];
	char time_buf[18];

	if (!vars->invoke_inspector && !vars->closed)
		crash_run_hooks(NULL, -1);

	crash_time(time_buf, sizeof time_buf);
	print_str(time_buf);			/* 0 */
	print_str(" CRASH (pid=");		/* 1 */
	print_str(print_number(pid_buf, sizeof pid_buf, getpid()));	/* 2 */
	print_str(") ");				/* 3 */
	print_str("attempting to dump a decorated stack trace:\n");	/* 4 */
	flush_err_str();
	if (log_stdout_is_distinct())
		flush_str(STDOUT_FILENO);
}

/**
 * Marks end of crash logging and potential pausing or debugger hook calling.
 */
static G_GNUC_COLD void
crash_end_of_line(bool forced)
{
	DECLARE_STR(7);
	char pid_buf[22];
	char time_buf[18];

	if (!forced && !vars->invoke_inspector && !vars->closed)
		crash_run_hooks(NULL, -1);

	crash_time(time_buf, sizeof time_buf);

	print_str(time_buf);			/* 0 */
	print_str(" CRASH (pid=");		/* 1 */
	print_str(print_number(pid_buf, sizeof pid_buf, getpid()));	/* 2 */
	print_str(") ");				/* 3 */
	if (forced) {
		print_str("recursively crashing -- end of line.");	/* 4 */
	} else if (vars->closed) {
		print_str("end of line.");	/* 4 */
	} else if (vars->invoke_inspector) {
		if (NULL != vars->exec_path) {
			print_str("calling ");				/* 4 */
			print_str(vars->exec_path);			/* 5 */
		} else {
			print_str("calling gdb...");		/* 4 */
		}
	} else if (vars->pause_process) {
		print_str("pausing -- end of line.");	/* 4 */
	} else {
		print_str("end of line.");	/* 4 */
	}
	print_str("\n");				/* 6, at most */
	flush_err_str();
	if (log_stdout_is_distinct())
		flush_str(STDOUT_FILENO);
}

/**
 * Construct name of GTKG crash log.
 */
static G_GNUC_COLD void
crash_logname(char *buf, size_t len, const char *pidstr)
{
	clamp_strcpy(buf, len, EMPTY_STRING(vars->progname));

	if (0 != vars->has_numbers) {
		char num_buf[ULONG_DEC_BUFLEN + 2];
		const char *num_str;

		num_str = print_number(num_buf, sizeof num_buf, vars->major);
		clamp_strcat(buf, len, "-");
		clamp_strcat(buf, len, num_str);
		num_str = print_number(num_buf, sizeof num_buf, vars->minor);
		clamp_strcat(buf, len, ".");
		clamp_strcat(buf, len, num_str);
		num_str = print_number(num_buf, sizeof num_buf, vars->patchlevel);
		clamp_strcat(buf, len, ".");
		clamp_strcat(buf, len, num_str);
	}

	/*
	 * File is opened with O_EXCL so we need to make the filename as unique
	 * as possible.  Therefore, include the build number if available.
	 */

	if (0 != vars->build) {
		char build_buf[ULONG_DEC_BUFLEN + 2];
		const char *build_str;

		build_str = print_number(build_buf, sizeof build_buf, vars->build);
		clamp_strcat(buf, len, "-r");
		clamp_strcat(buf, len, build_str);
	}

	clamp_strcat(buf, len, "-crash.");

	/*
	 * Because we can re-execute ourselves (at user's request after an upgrade
	 * or after a crash), we need to include our starting time as well.
	 *
	 * Having the time right after the version allows natural sorting of
	 * files for the same version, with the latest one at the end.
	 */

	{
		char time_buf[ULONG_HEX_BUFLEN];
		const char *time_str;

		time_str = print_hex(time_buf, sizeof time_buf, vars->start_time);
		clamp_strcat(buf, len, time_str);
	}

	/*
	 * Finish with the PID, to really ensure we get a unique filename.
	 */

	clamp_strcat(buf, len, ".");
	clamp_strcat(buf, len, pidstr);
	clamp_strcat(buf, len, ".log");
}

/**
 * Emit the current stack frame to specified file, or the assertion stack
 * if we have one.
 */
static G_GNUC_COLD NO_INLINE void
crash_stack_print(int fd, size_t offset)
{
	if (vars != NULL && vars->stackcnt != 0) {
		/* Saved assertion stack preferred over current stack trace */
		stacktrace_stack_safe_print(fd, vars->stack, vars->stackcnt);
	} else {
		stacktrace_where_cautious_print_offset(fd, offset + 1);
	}
}

/**
 * Emit a decorated stack frame to specified file, using a gdb-like format.
 */
static G_GNUC_COLD NO_INLINE void
crash_stack_print_decorated(int fd, size_t offset, bool in_child)
{
	uint flags = STACKTRACE_F_ORIGIN | STACKTRACE_F_SOURCE | STACKTRACE_F_GDB |
			STACKTRACE_F_ADDRESS | STACKTRACE_F_NO_INDENT |
			STACKTRACE_F_NUMBER | STACKTRACE_F_PATH;

	if (!in_child && vars != NULL && vars->stackcnt != 0) {
		/* Saved assertion stack preferred over current stack trace */
		stacktrace_stack_print_decorated(fd,
			vars->stack, vars->stackcnt, flags);
	} else {
		void *stack[STACKTRACE_DEPTH_MAX];
		size_t count;

		count = stacktrace_safe_unwind(stack, G_N_ELEMENTS(stack), offset + 1);
		stacktrace_stack_print_decorated(fd, stack, count, flags);
	}
}

/**
 * Emit a decorated stack.
 */
static G_GNUC_COLD NO_INLINE void
crash_emit_decorated_stack(size_t offset, bool in_child)
{
	crash_decorating_stack();
	crash_stack_print_decorated(STDERR_FILENO, offset + 1, in_child);
	if (log_stdout_is_distinct())
		crash_stack_print_decorated(STDOUT_FILENO, offset + 1, in_child);
}

/**
 * Reset the handler of all the signals we trap, and unblock them.
 */
static G_GNUC_COLD void
crash_reset_signals(void)
{
	unsigned i;

	/*
	 * The signal mask is preserved across execve(), therefore it is
	 * important to also unblock all the signals we trap in case we
	 * are about to re-exec() ourselves from a signal handler!
	 */

	for (i = 0; i < G_N_ELEMENTS(signals); i++) {
		signal_set(signals[i], SIG_DFL);
		signal_unblock(signals[i]);
	}
}

#ifdef HAS_FORK
static Sigjmp_buf crash_fork_env;

/**
 * Handle fork() timeouts.
 */
static G_GNUC_COLD void
crash_fork_timeout(int signo)
{
	DECLARE_STR(2);
	char time_buf[18];

	crash_time(time_buf, sizeof time_buf);
	print_str(time_buf);
	print_str(" (WARNING): fork() timed out, found a libc bug?\n");
	flush_err_str();

	Siglongjmp(crash_fork_env, signo);
}
#endif	/* HAS_FORK */

/**
 * A fork() wrapper to work around libc6 bugs causing hangs within fork().
 */
static G_GNUC_COLD pid_t
crash_fork(void)
{
#ifdef HAS_FORK
	pid_t pid;
	signal_handler_t old_sigalrm;
#ifdef HAS_ALARM
	unsigned remain;
#endif

#ifdef SIGPROF
	/*
	 * We're forking following a crash, we're going to abort() or exit()
	 * abnormally, we could not care less about profiling at this stage.
	 *
	 * SIGPROF could also be the cause of the libc6 hangs I've been witnessing
	 * on Linux, since I'm often running with profiling enabled.
	 *		--RAM, 2011-11-02
	 */

	signal_set(SIGPROF, SIG_IGN);
#endif

#ifdef HAS_ALARM
	old_sigalrm = signal_set(SIGALRM, crash_fork_timeout);
	remain = alarm(15);		/* Guess, large enough to withstand system load */
#endif

	if (Sigsetjmp(crash_fork_env, TRUE)) {
		errno = EDEADLK;	/* Probable deadlock in the libc */
		pid = -1;
		goto restore;
	}

	pid = fork();
	/* FALL THROUGH */
restore:
#ifdef HAS_ALARM
	alarm(remain);
	signal_set(SIGALRM, old_sigalrm);
#endif

	return pid;
#else
	return 0;			/* Act as if we were in a child upon return */
#endif	/* HAS_FORK */
}

/**
 * Write crash log header.
 *
 * @param clf		crash log file descriptor
 * @param signo		crashing signal number
 * @param filename	name of the crash log file
 */
static void
crash_log_write_header(int clf, int signo, const char *filename)
{
	char tbuf[22];
	char rbuf[22];
	char sbuf[ULONG_DEC_BUFLEN];
	char nbuf[ULONG_DEC_BUFLEN];
	char lbuf[22];
	time_delta_t t;
	struct utsname u;
	long cpucount = getcpucount();
	DECLARE_STR(15);

	crash_time_iso(tbuf, sizeof tbuf);
	crash_run_time(rbuf, sizeof rbuf);
	t = delta_time(time(NULL), vars->start_time);

	print_str("Operating-System: ");	/* 0 */
	if (-1 != uname(&u)) {
		print_str(u.sysname);			/* 1 */
		print_str(" ");					/* 2 */
		print_str(u.release);			/* 3 */
		print_str(" ");					/* 4 */
		print_str(u.version);			/* 5 */
		print_str("\n");				/* 6 */
	} else {
		print_str("Unknown\n");
	}
	print_str("CPU-Architecture: ");	/* 7 */
	if (-1 != uname(&u)) {
		print_str(u.machine);			/* 8 */
	} else {
		print_str("unknown");			/* 8 */
	}
	if (cpucount > 1) {
		print_str(" * ");				/* 9 */
		print_str(print_number(sbuf, sizeof sbuf, cpucount)); /* 10 */
	}
	print_str(", ");					/* 11 */
	print_str(print_number(nbuf, sizeof nbuf, PTRSIZE * 8)); /* 12 */
	print_str(" bits\n");				/* 13 */
	flush_str(clf);
	rewind_str(0);

	print_str("Executable-Path: ");		/* 0 */
	print_str(vars->argv0);				/* 1 */
	print_str("\n");					/* 2 */
	if (NULL != vars->version) {
		print_str("Version: ");			/* 3 */
		print_str(vars->version);		/* 4 */
		print_str("\n");				/* 5 */
	}
	print_str("Run-Elapsed: ");			/* 6 */
	print_str(rbuf);					/* 7 */
	print_str("\n");					/* 8 */
	print_str("Run-Seconds: ");			/* 9 */
	print_str(print_number(sbuf, sizeof sbuf, MAX(t, 0)));	/* 10 */
	print_str("\n");					/* 11 */
	print_str("Crash-Signal: ");		/* 12 */
	print_str(signal_name(signo));		/* 13 */
	print_str("\n");					/* 14 */
	flush_str(clf);
	rewind_str(0);
	print_str("Crash-Time: ");			/* 0 */
	print_str(tbuf);					/* 1 */
	print_str("\n");					/* 2 */
	print_str("Core-Dump: ");			/* 3 */
	print_str(vars->dumps_core ? "enabled" : "disabled");	/* 4 */
	print_str("\n");					/* 5 */
	if (NULL != vars->cwd) {
		print_str("Working-Directory: ");	/* 6 */
		print_str(vars->cwd);				/* 7 */
		print_str("\n");					/* 8 */
	}
	if (NULL != vars->exec_path) {
		print_str("Exec-Path: ");		/* 9 */
		print_str(vars->exec_path);		/* 10 */
		print_str("\n");				/* 11 */
	}
	if (NULL != vars->crashdir) {
		print_str("Crash-Directory: ");	/* 12 */
		print_str(vars->crashdir);		/* 13 */
		print_str("\n");				/* 14 */
	}
	flush_str(clf);
	rewind_str(0);
	print_str("Crash-File: ");			/* 0 */
	print_str(filename);				/* 1 */
	print_str("\n");					/* 2 */
	if (vars->failure != NULL) {
		const assertion_data *failure = vars->failure;
		if (failure->expr != NULL) {
			print_str("Assertion-At: ");	/* 3 */
		} else {
			print_str("Reached-Code-At: ");	/* 3 */
		}
		print_str(failure->file);			/* 4 */
		print_str(":");						/* 5 */
		print_str(print_number(lbuf, sizeof lbuf, failure->line));
		print_str("\n");					/* 6 */
		if (failure->expr != NULL) {
			print_str("Assertion-Expr: ");	/* 7 */
			print_str(failure->expr);		/* 8 */
			print_str("\n");				/* 9 */
		}
		if (vars->message != NULL) {
			print_str("Assertion-Info: ");	/* 10 */
			print_str(vars->message);		/* 11 */
			print_str("\n");				/* 12 */
		}
	} else if (vars->message != NULL) {
		print_str("Error-Message: ");		/* 3 */
		print_str(vars->message);			/* 4 */
		print_str("\n");					/* 5 */
	}
	flush_str(clf);

	rewind_str(0);
	print_str("Atomic-Operations: ");					/* 0 */
	print_str(atomic_ops_available() ? "yes" : "no");	/* 1 */
	print_str("\n");									/* 2 */
	flush_str(clf);

	rewind_str(0);
	print_str("Auto-Restart: ");		/* 0 */
	print_str(vars->may_restart ? "enabled" : "disabled"); /* 1 */
	if (t <= CRASH_MIN_ALIVE) {
		char rtbuf[ULONG_DEC_BUFLEN];
		print_str("; run time threshold of ");	/* 2 */
		print_str(print_number(rtbuf, sizeof rtbuf, CRASH_MIN_ALIVE));
		print_str("s not reached");				/* 4 */
	} else {
		print_str("; ");				/* 2 */
		print_str(vars->may_restart ? "will" : "would"); /* 3 */
		print_str(" be attempted");		/* 4 */
	}
	print_str("\n");					/* 5 */
	{
		enum stacktrace_sym_quality sq = stacktrace_quality();
		if (STACKTRACE_SYM_GOOD != sq) {
			const char *quality = stacktrace_quality_string(sq);
			print_str("Stacktrace-Symbols: ");		/* 6 */
			print_str(quality);						/* 7 */
			print_str("\n");						/* 8 */
		}
	}

	print_str("Stacktrace:\n");			/* 9 */
	flush_str(clf);
	crash_stack_print(clf, 3);

	rewind_str(0);
	print_str("\n");					/* 0 -- End of Header */
	flush_str(clf);
}

/*
 * Carefully close opened file descriptor.
 *
 * We must be careful for OS X: we cannot close random UNIX file descriptors
 * or we get sanctionned with:
 * BUG IN CLIENT OF LIBDISPATCH: Do not close random Unix descriptors
 *		--RAM, 2011-11-17
 *
 * @param fd		file descriptor to close
 *
 * @return -1 if an error occured, 0 if OK.
 */
static inline int
crash_fd_close(int fd)
{
	if (is_open_fd(fd))
		return close(fd);

	return 0;	/* Nothing done */
}

/**
 * Generate a crash logfile.
 *
 * This is used when there is no inspector run, to leave a trace of the crash.
 */
static G_GNUC_COLD void
crash_generate_crashlog(int signo)
{
	static char crashlog[MAX_PATH_LEN];
   	const char *pid_str;
	char pid_buf[22];
	char filename[80];
	int clf;
	const mode_t mode = S_IRUSR | S_IWUSR;
	int flags = O_CREAT | O_TRUNC | O_EXCL | O_WRONLY;

	pid_str = print_number(pid_buf, sizeof pid_buf, getpid());
	crash_logname(filename, sizeof filename, pid_str);
	if (vars != NULL && vars->crashdir != NULL) {
		str_bprintf(crashlog, sizeof crashlog,
			"%s%c%s", vars->crashdir, G_DIR_SEPARATOR, filename);
	} else {
		str_bprintf(crashlog, sizeof crashlog, "%s", filename);
	}
	clf = open(crashlog, flags, mode);
	if (-1 == clf) {
		char buf[256];
		str_bprintf(buf, sizeof buf, "cannot create %s: %m", crashlog);
		s_miniwarn("%s", buf);
		return;
	}
	crash_log_write_header(clf, signo, filename);
	crash_stack_print_decorated(clf, 2, FALSE);
	crash_run_hooks(NULL, clf);
	close(clf);
	s_minimsg("trace left in %s", crashlog);
	if (vars != NULL && vars->dumps_core) {
		bool gotcwd = NULL != getcwd(crashlog, sizeof crashlog);
		s_minimsg("core dumped in %s", gotcwd ? crashlog : "current directory");
	}
}

/**
 * Invoke the inspector process (gdb, or any other program specified at
 * initialization time).
 *
 * @return TRUE if we were able to invoke the crash hooks.
 */
static G_GNUC_COLD bool
crash_invoke_inspector(int signo, const char *cwd)
{
   	const char *pid_str;
	char pid_buf[22];
	pid_t pid;
	int fd[2];
	const char *stage = NULL;
	bool retried_child = FALSE;
	bool could_fork = has_fork();
	int fork_errno = 0;
	int parent_stdout = STDOUT_FILENO;

	pid_str = print_number(pid_buf, sizeof pid_buf, getpid());

#ifdef HAS_WAITPID
retry_child:
#endif

	/* Make sure we don't exceed the system-wide file descriptor limit */
	close_file_descriptors(3);

	if (has_fork()) {
		/* In case fork() fails, make sure we leave stdout open */
		if (PARENT_STDOUT_FILENO != dup(STDOUT_FILENO)) {
			stage = "parent's stdout duplication";
			goto parent_failure;
		}
		parent_stdout = PARENT_STDOUT_FILENO;

		/* Make sure child will get access to the stderr of its parent */
		if (PARENT_STDERR_FILENO != dup(STDERR_FILENO)) {
			stage = "parent's stderr duplication";
			goto parent_failure;
		}

		if (
			crash_fd_close(STDIN_FILENO) ||
			crash_fd_close(STDOUT_FILENO) ||
			pipe(fd) ||
			STDIN_FILENO != fd[0] ||
			STDOUT_FILENO != fd[1]
		) {
			stage = "pipe setup";
			goto parent_failure;
		}
	} else {
		DECLARE_STR(2);
		char time_buf[18];

		crash_time(time_buf, sizeof time_buf);
		print_str(time_buf);
		print_str(" (WARNING): cannot fork() on this platform\n");
		flush_err_str();
		if (log_stdout_is_distinct())
			flush_str(STDOUT_FILENO);
	}

#ifdef SIGCHLD
	signal_set(SIGCHLD, SIG_DFL);
#endif

	pid = crash_fork();
	switch (pid) {
	case -1:
		fork_errno = errno;
		could_fork = FALSE;
		{
			DECLARE_STR(6);
			char time_buf[18];

			crash_time(time_buf, sizeof time_buf);
			print_str(time_buf);
			print_str(" (WARNING): fork() failed: ");
			print_str(symbolic_errno(errno));
			print_str(" (");
			print_str(g_strerror(errno));
			print_str(")\n");
			flush_err_str();
			if (log_stdout_is_distinct())
				flush_str(parent_stdout);
		}
		/*
		 * Even though we could not fork() for some reason, we're going
		 * to continue as if we were in the "child process" to create
		 * the crash log file and save important information.
		 */
		/* FALL THROUGH */
	case 0:	/* executed by child */
		{
			int flags;
			const mode_t mode = S_IRUSR | S_IWUSR;
			char const *argv[8];
			char filename[80];
			char tbuf[22];
			char cmd[MAX_PATH_LEN];
			int clf = STDOUT_FILENO;	/* crash log file fd */
			DECLARE_STR(10);

			/*
			 * Immediately unplug the crash handler in case we do something
			 * bad in the child: we want it to crash immediately and have
			 * the parent see the failure.
			 */

			if (could_fork)
				crash_reset_signals();

			/*
			 * If we are retrying the child, don't discard what we can
			 * have already from a previous run.
			 */

			if (retried_child) {
				flags = O_WRONLY | O_APPEND;
			} else {
				flags = O_CREAT | O_TRUNC | O_EXCL | O_WRONLY;
			}

			if (vars->exec_path) {
				argv[0] = vars->exec_path;
				argv[1] = vars->argv0;
				argv[2] = pid_str;
				argv[3] = NULL;
			} else {
				const char quote_ch = '\'';
				size_t max_argv0;

				clamp_strcpy(cmd, sizeof cmd, "gdb -q -n -p ");
				clamp_strcat(cmd, sizeof cmd, pid_str);

				/**
				 * The parameter -e argv0 is required on some platforms but
				 * we only provide it if there is sufficient space and can be
				 * safely quoted as shell command.
				 */

				max_argv0 = (sizeof cmd) - (strlen(cmd) + strlen(" -se ''"));
				if (
					NULL == strchr(vars->argv0, quote_ch) &&
					strlen(vars->argv0) < max_argv0
				) {
					clamp_strcat(cmd, sizeof cmd, " -se '");
					clamp_strcat(cmd, sizeof cmd, vars->argv0);
					clamp_strcat(cmd, sizeof cmd, "'");
				}

				/*
				 * We use "/bin/sh -c" to launch gdb so that we don't have
				 * to locate the executable in the PATH, letting the shell
				 * do it for us.
				 */

				argv[0] = "/bin/sh";
				argv[1] = "-c";
				argv[2] = cmd;
				argv[3] = NULL;
			}
			crash_logname(filename, sizeof filename, pid_str);

			if (could_fork) {
				/* STDIN must be kept open when piping to gdb */
				if (vars->exec_path != NULL) {
					if (
						crash_fd_close(STDIN_FILENO) ||
						STDIN_FILENO != open("/dev/null", O_RDONLY, 0)
					)
						goto child_failure;
				}

				set_close_on_exec(PARENT_STDERR_FILENO);
				set_close_on_exec(PARENT_STDOUT_FILENO);
			}

			if (could_fork) {
				if (
					crash_fd_close(STDOUT_FILENO) ||
					crash_fd_close(STDERR_FILENO) ||
					STDOUT_FILENO != open(filename, flags, mode) ||
					STDERR_FILENO != dup(STDOUT_FILENO)
				)
					goto child_failure;
			} else {
				clf = open(filename, flags, mode);
			}

			/*
			 * When retrying, issue a blank line, and mark retrying attempt.
			 */

			if (retried_child) {
				print_str("\n---- Retrying:\n\n");	/* 0 */
				flush_str(clf);
				rewind_str(0);
			}

			/*
			 * Emit crash header.
			 */

			crash_log_write_header(clf, signo, filename);

			/*
			 * If we don't have fork() on this platform (or could not fork)
			 * we've now logged the essential stuff: we can execute what is
			 * normally done by the parent process.
			 */

			if (!could_fork) {
				rewind_str(0);
				if (!has_fork()) {
					print_str("No fork() on this platform.\n");
				} else {
					print_str("fork() failed: ");
					print_str(symbolic_errno(fork_errno));
					print_str(" (");
					print_str(g_strerror(fork_errno));
					print_str(")\n");
				}
				flush_str(clf);
				crash_stack_print_decorated(clf, 2, FALSE);
				crash_fd_close(clf);
				goto parent_process;
			}

			/*
			 * Since we have fork(), and we're in the crash inspector,
			 * run the crash hooks, directing output into the crash file.
			 * But first, we have to force stderr to use a new file descriptor
			 * since we're in the child process and stdout has been remapped
			 * to the crash file.
			 *
			 * If we have already been trough the child, don't attempt to
			 * run hooks again in case they were responsible for the crash
			 * of the process already.
			 */

			if (!retried_child) {
				log_force_fd(LOG_STDERR, PARENT_STDERR_FILENO);
				log_set_disabled(LOG_STDOUT, TRUE);
				crash_run_hooks(NULL, STDOUT_FILENO);
				log_set_disabled(LOG_STDOUT, FALSE);
			}

#ifdef HAS_SETSID
			if (-1 == setsid())
				goto child_failure;
#endif

			/*
			 * They may have specified a relative path for the program (argv0)
			 * so go back to the initial working directory to allow the
			 * inspector to find it since we're passing the name in the
			 * argument list.
			 */

			if (
				NULL != vars->cwd &&
				(
					!is_absolute_path(EMPTY_STRING(vars->exec_path)) ||
					!is_absolute_path(vars->argv0)
				)
			) {
				/* Ignore error, it may still work */
				IGNORE_RESULT(chdir(vars->cwd));
			}

			/*
			 * Pass the Crashfile variable to the custom program.
			 */

			if (NULL != vars->exec_path) {
				const char *envp[2];

				envp[0] = vars->crashfile;
				envp[1] = NULL;
				execve(argv[0], (const void *) argv, (const void *) envp);
			} else {
				execve(argv[0], (const void *) argv, NULL);
			}

			/* Log exec failure */
			crash_time(tbuf, sizeof tbuf);
			rewind_str(0);
			print_str(tbuf);					/* 0 */
			print_str(" CRASH (pid=");			/* 1 */
			print_str(pid_str);					/* 2 (parent's PID) */
			print_str(") ");					/* 3 */
			print_str("exec() error: ");		/* 4 */
			print_str(symbolic_errno(errno));	/* 5 */
			print_str(" (");					/* 6 */
			print_str(g_strerror(errno));		/* 7 */
			print_str(")\n");					/* 8 */
			flush_str(PARENT_STDERR_FILENO);
			flush_str(STDOUT_FILENO);			/* into crash file as well */
			if (log_stdout_is_distinct())
				flush_str(parent_stdout);

			/*
			 * Emit a decorated stack since we could not exec the script.
			 *
			 * Even though we're in the child process, say FALSE because
			 * we want the original stack frame from the parent if it was
			 * saved, not the current one.
			 */

			crash_stack_print_decorated(STDOUT_FILENO, 2, FALSE);

		child_failure:
			_exit(EXIT_FAILURE);
		}	
		break;

	default:	/* executed by parent */
		break;
	}

	/*
	 * The following is only executed by the parent process.
	 */

parent_process:
	{
		DECLARE_STR(10);
		unsigned iov_prolog;
		char time_buf[18];
		int status;
		bool child_ok = FALSE;

		if (has_fork()) {
			crash_fd_close(PARENT_STDERR_FILENO);
		}

		/*
		 * Now that the child has started, we can write commands to
		 * the pipe without fearing any blocking: we'll either
		 * succeed or get EPIPE if the child dies and closes its end.
		 */

		if (could_fork) {
			static const char commands[] =
				"thread\nbt\nbt full\nthread apply bt\nquit\n";
			const size_t n = CONST_STRLEN(commands);
			ssize_t ret;

			ret = write(STDOUT_FILENO, commands, n);
			if (n != UNSIGNED(ret)) {
				/*
				 * EPIPE is acceptable if the child's immediate action
				 * is to close stdin... The child could get scheduled
				 * before the parent, so this must be handled.
				 */

				if ((ssize_t) -1 != ret || EPIPE != errno) {
					stage = "sending commands to pipe";
					goto parent_failure;
				}
				/* FALL THROUGH */
			}
		}

		/*
		 * We need to maintain the pipe opened even though we
		 * sent commands because otherwise gdb complains about
		 * "Hangup detected on fd 0".
		 */

		crash_time(time_buf, sizeof time_buf);

		/* The following precedes each line */
		print_str(time_buf);				/* 0 */
		print_str(" CRASH (pid=");			/* 1 */
		print_str(pid_str);					/* 2 */
		print_str(") ");					/* 3 */
		iov_prolog = getpos_str();

		if (!could_fork) {
			child_ok = TRUE;
			goto no_fork;
		}

#ifdef HAS_WAITPID
		if ((pid_t) -1 == waitpid(pid, &status, 0)) {
			char buf[ULONG_DEC_BUFLEN];
			print_str("could not wait for child (errno = ");	/* 4 */
			print_str(print_number(buf, sizeof buf, errno));	/* 5 */
			print_str(")\n");									/* 6 */
			flush_err_str();
		} else if (WIFEXITED(status)) {
			if (vars->invoke_inspector && 0 == WEXITSTATUS(status)) {
				child_ok = TRUE;
			} else {
				char buf[ULONG_DEC_BUFLEN];

				print_str("child exited with status ");	/* 4 */
				print_str(print_number(buf, sizeof buf,
					WEXITSTATUS(status)));				/* 5 */
				print_str("\n");						/* 6 */
				flush_err_str();
				if (log_stdout_is_distinct())
					flush_str(parent_stdout);
			}
		} else {
			bool may_retry = FALSE;

			if (WIFSIGNALED(status)) {
				int sig = WTERMSIG(status);
				print_str("child got a ");			/* 4 */
				print_str(signal_name(sig));		/* 5 */
				if (!retried_child && NULL != crash_get_hook()) {
					may_retry = TRUE;
				}
			} else {
				print_str("child exited abnormally");	/* 4 */
			}
			print_str("\n");						/* 6, at most */
			flush_err_str();
			if (log_stdout_is_distinct())
				flush_str(parent_stdout);

			/*
			 * If we have hooks to run and the child crashed with a signal,
			 * attempt to run the child again without the hooks to make sure
			 * we get the full gdb stack.
			 */

			if (may_retry) {
				rewind_str(iov_prolog);
				print_str("retrying child fork without hooks\n");
				flush_err_str();
				if (log_stdout_is_distinct())
					flush_str(parent_stdout);
				retried_child = TRUE;
				goto retry_child;
			}
		}
#else
		(void) status;
#endif	/* HAS_WAITPID */

		/*
		 * Let them know where the trace is.
		 *
		 * Even if the child exited abnormally, there may be some
		 * partial information there so we mention the filename to
		 * have them look at it.
		 */

no_fork:
		{
			char buf[64];

			/*
			 * If there are crashing hooks recorded that we can invoke, run
			 * them and redirect a copy of the messages to the crash log.
			 */

			crash_logname(buf, sizeof buf, pid_str);
			if (!could_fork)
				crash_run_hooks(buf, -1);

			rewind_str(iov_prolog);
			if (!child_ok)
				print_str("possibly incomplete ");		/* 4 */
			print_str("trace left in ");				/* 5 */
			if (*cwd != '\0') {
				print_str(cwd);					/* 6 */
				print_str(G_DIR_SEPARATOR_S);	/* 7 */
				print_str(buf);					/* 8 */
			} else {
				print_str(buf);					/* 6 */
			}
			print_str("\n");					/* 9, at most */
			flush_err_str();
			if (log_stdout_is_distinct())
				flush_str(parent_stdout);
		}

		/*
		 * Items 0, 1, 2, 3 of the vector were already built above,
		 * and contain the crash time, and the "CRASH (pid=xxx)" string.
		 * No need to regenerate them, so start at index 4.
		 */

		if (vars->dumps_core) {
			rewind_str(iov_prolog);
			print_str("core dumped in ");	/* 4 */
			print_str(cwd);					/* 5 */
			print_str("\n");				/* 6 */
			flush_err_str();
			if (log_stdout_is_distinct())
				flush_str(parent_stdout);
		}

		/*
		 * Closing needs to happen after we gave feedback about the
		 * fate of our child.
		 */

		if (has_fork()) {
			if (
				crash_fd_close(STDOUT_FILENO) ||
				-1 == dup2(PARENT_STDOUT_FILENO, STDOUT_FILENO) ||
				crash_fd_close(PARENT_STDOUT_FILENO)
			) {
				stage = "stdout restore";
				goto parent_failure;
			}

			if (
				crash_fd_close(STDIN_FILENO) ||
				STDIN_FILENO != open("/dev/null", O_RDONLY, 0)
			) {
				stage = "final stdin closing";
				goto parent_failure;
			}
		}

		/*
		 * This is our "OK" marker.  If it's not present in the logs,
		 * it means something went wrong.
		 */

		rewind_str(iov_prolog);
		print_str("end of line.\n");	/* 4 */
		flush_err_str();
		if (log_stdout_is_distinct())
			flush_str(STDOUT_FILENO);
	}

	return TRUE;

parent_failure:
	{
		DECLARE_STR(6);
		char time_buf[18];

		crash_time(time_buf, sizeof time_buf);
		print_str(time_buf);					/* 0 */
		print_str(" CRASH (pid=");				/* 1 */
		print_str(pid_str);						/* 2 */
		print_str(") error in parent during ");	/* 3 */
		print_str(EMPTY_STRING(stage));			/* 4 */
		print_str("\n");						/* 5 */
		flush_err_str();
		if (log_stdout_is_distinct())
			flush_str(parent_stdout);
	}

	return FALSE;
}

/**
 * Entering crash mode.
 */
static G_GNUC_COLD void
crash_mode(void)
{
	/*
	 * Put our main allocators in crash mode, which will limit risks if we
	 * are crashing due to a data structure corruption or an assertion failure.
	 */

	vmm_crash_mode();
	xmalloc_crash_mode();

	if (vars != NULL) {
		if (!vars->crash_mode) {
			uint8 t = TRUE;

			crash_set_var(crash_mode, t);

			/*
			 * Configuring crash mode logging requires a formatting string.
			 *
			 * In crashing mode, logging will avoid fprintf() and will use
			 * the pre-allocated string to format message, calling write()
			 * to emit the message.
			 */

			ck_writable(vars->fmtck);		/* Chunk holding vars->fmtstr */
			log_crashing(vars->fmtstr);
		}
		if (ck_is_readonly(vars->fmtck)) {
			char time_buf[18];
			DECLARE_STR(2);

			crash_time(time_buf, sizeof time_buf);
			print_str(time_buf);
			print_str(" WARNING: formatting string held in read-only chunk\n");
			flush_err_str();
		}
	} else {
		static bool warned;

		if (!warned) {
			char time_buf[18];
			DECLARE_STR(2);

			warned = TRUE;
			crash_time(time_buf, sizeof time_buf);
			print_str(time_buf);
			print_str(" WARNING: crashing before any crash_init() call\n");
			flush_err_str();
		}
	}
}

/**
 * Re-execute the same process with the same arguments.
 *
 * This function only returns when exec()ing fails.
 */
static G_GNUC_COLD void
crash_try_reexec(void)
{
	char dir[MAX_PATH_LEN];

	if (NULL == vars) {
		s_carp("%s(): no crash_init() yet!", G_STRFUNC);
		_exit(EXIT_FAILURE);
	}

	/*
	 * They may have specified a relative path for the program (argv0)
	 * or for some of the arguments (--log-stderr file) so go back to the
	 * initial working directory before launching the new process.
	 */

	if (NULL != vars->cwd) {
		bool gotcwd = NULL != getcwd(dir, sizeof dir);

		if (-1 == chdir(vars->cwd)) {
			s_miniwarn("%s(): cannot chdir() to \"%s\": %m",
				G_STRFUNC, vars->cwd);
		} else if (gotcwd && 0 != strcmp(dir, vars->cwd)) {
			s_minimsg("switched back to directory %s", vars->cwd);
		}
	}

	if (vars->logstr != NULL) {
		int i;

		/*
		 * The string we use for formatting is held in a read-only chunk.
		 * Before formatting inside, we must therfore make the chunk writable.
		 * Since we're about to exec(), we don't bother turning it back to
		 * the read-only status.
		 */

		ck_writable(vars->logck);
		str_reset(vars->logstr);

		for (i = 0; i < vars->argc; i++) {
			if (i != 0)
				str_putc(vars->logstr, ' ');
			str_cat(vars->logstr, vars->argv[i]);
		}

		s_minimsg("launching %s", str_2c(vars->logstr));
	} else {
		s_minimsg("launching %s with %d argument%s", vars->argv0,
			vars->argc, 1 == vars->argc ? "" : "s");
	}

	/*
	 * Off we go...
	 */

#ifdef SIGPROF
	signal_set(SIGPROF, SIG_IGN);	/* In case we're running under profiler */
#endif

	close_file_descriptors(3);
	crash_reset_signals();
	execve(vars->argv0, (const void *) vars->argv, (const void *) vars->envp);

	/* Log exec() failure */

	{
		char tbuf[22];
		DECLARE_STR(6);

		crash_time(tbuf, sizeof tbuf);
		print_str(tbuf);							/* 0 */
		print_str(" (CRITICAL) exec() error: ");	/* 1 */
		print_str(symbolic_errno(errno));			/* 2 */
		print_str(" (");							/* 3 */
		print_str(g_strerror(errno));				/* 4 */
		print_str(")\n");							/* 5 */
		flush_err_str();
		if (log_stdout_is_distinct())
			flush_str(STDOUT_FILENO);

		rewind_str(1);
		print_str(" (CRITICAL) executable file was: ");	/* 1 */
		print_str(vars->argv0);							/* 2 */
		print_str("\n");								/* 3 */
		flush_err_str();
		if (log_stdout_is_distinct())
			flush_str(STDOUT_FILENO);

		if (NULL != getcwd(dir, sizeof dir)) {
			rewind_str(1);
			print_str(" (CRITICAL) current directory was: ");	/* 1 */
			print_str(dir);										/* 2 */
			print_str("\n");									/* 3 */
			flush_err_str();
			if (log_stdout_is_distinct())
				flush_str(STDOUT_FILENO);
		}
	}
}

/**
 * Handle possible auto-restart, if configured.
 * This function does not return when auto-restart succeeds
 */
static G_GNUC_COLD void
crash_auto_restart(void)
{
	/*
	 * When the process has been alive for some time (CRASH_MIN_ALIVE secs,
	 * to avoid repetitive frequent failures), we can consider auto-restarts
	 * if CRASH_F_RESTART was given.
	 */

	if (delta_time(time(NULL), vars->start_time) <= CRASH_MIN_ALIVE) {
		if (vars->may_restart) {
			char time_buf[18];
			char runtime_buf[22];
			DECLARE_STR(5);

			crash_time(time_buf, sizeof time_buf);
			crash_run_time(runtime_buf, sizeof runtime_buf);
			print_str(time_buf);							/* 0 */
			print_str(" (WARNING) not auto-restarting ");	/* 1 */
			print_str("since process was only up for ");	/* 2 */
			print_str(runtime_buf);							/* 3 */
			print_str("\n");								/* 4 */
			flush_err_str();
			if (log_stdout_is_distinct())
				flush_str(STDOUT_FILENO);
		}
		return;
	}

	if (vars->may_restart) {
		char time_buf[18];
		char runtime_buf[22];
		DECLARE_STR(6);

		crash_time(time_buf, sizeof time_buf);
		crash_run_time(runtime_buf, sizeof runtime_buf);
		print_str(time_buf);					/* 0 */
		print_str(" (INFO) ");					/* 1 */
		if (vars->dumps_core) {
			print_str("auto-restart was requested");	/* 2 */
		} else {
			print_str("core dumps are disabled");		/* 2 */
		}
		print_str(" and process was up for ");	/* 3 */
		print_str(runtime_buf);					/* 4 */
		print_str("\n");						/* 5 */
		flush_err_str();
		if (log_stdout_is_distinct())
			flush_str(STDOUT_FILENO);

		rewind_str(1);
		print_str(" (MESSAGE) ");					/* 1 */
		print_str("attempting auto-restart...");	/* 2 */
		print_str("\n");							/* 3 */
		flush_err_str();
		if (log_stdout_is_distinct())
			flush_str(STDOUT_FILENO);

		/*
		 * We want to preserve our ability to dump a core, so fork() a child
		 * to perform the exec() and keep the parent going for core dumping.
		 */

		if (vars->dumps_core) {
			pid_t pid = crash_fork();
			switch (pid) {
			case -1:	/* fork() error */
				crash_time(time_buf, sizeof time_buf);
				print_str(time_buf);						/* 0 */
				print_str(" (CRITICAL) fork() error: ");	/* 1 */
				print_str(symbolic_errno(errno));			/* 2 */
				print_str(" (");							/* 3 */
				print_str(g_strerror(errno));				/* 4 */
				print_str(")\n");							/* 5 */
				flush_err_str();
				if (log_stdout_is_distinct())
					flush_str(STDOUT_FILENO);

				rewind_str(1);
				print_str(" (CRITICAL) ");			/* 1 */
				print_str("core dump suppressed");	/* 2 */
				flush_err_str();
				if (log_stdout_is_distinct())
					flush_str(STDOUT_FILENO);
				/* FALL THROUGH */
			case 0:		/* Child process */
				break;
			default:	/* Parent process */
				return;
			}
			/* FALL THROUGH */
		}

		crash_try_reexec();

		/* The exec() failed, we may dump a core then */

		if (vars->dumps_core) {
			crash_time(time_buf, sizeof time_buf);
			print_str(time_buf);					/* 0 */
			print_str(" (CRITICAL) ");				/* 1 */
			print_str("core dump re-enabled");		/* 2 */
			flush_err_str();
			if (log_stdout_is_distinct())
				flush_str(STDOUT_FILENO);
		}
	}
}

/**
 * The signal handler used to trap harmful signals.
 */
G_GNUC_COLD void
crash_handler(int signo)
{
	static volatile sig_atomic_t crashed;
	const char *name;
	const char *cwd = "";
	unsigned i;
	bool trace;
	bool recursive = crashed > 0;
	bool in_child = FALSE;

	/*
	 * SIGBUS and SIGSEGV are configured by signal_set() to be reset to the
	 * default behaviour on delivery, and are not masked during signal delivery.
	 *
	 * This allows us to usefully trap them again to detect recursive faults
	 * that would otherwise remain invisible (on the path between the initial
	 * signal handler and the dispatching of this crash handler routine) since
	 * the default handler normally leads to fatal error triggering a core dump.
	 */

	if (crashed++ > 1) {
		if (2 == crashed) {
			DECLARE_STR(1);

			print_str("\nERROR: too many recursive crashes\n");
			flush_err_str();
			signal_set(signo, SIG_DFL);
			raise(signo);
		} else if (3 == crashed) {
			raise(signo);
		}
		_exit(EXIT_FAILURE);	/* Die, die, die! */
	}

	/*
	 * If we are in the child process, prevent any exec() or pausing.
	 */

	if (vars != NULL && vars->pid != getpid()) {
		uint8 f = FALSE;
		in_child = TRUE;
		crash_set_var(invoke_inspector, f);
		crash_set_var(may_restart, f);
		crash_set_var(pause_process, f);
	}

	for (i = 0; i < G_N_ELEMENTS(signals); i++) {
		int sig = signals[i];
		switch (sig) {
#ifdef SIGBUS
		case SIGBUS:
#endif
		case SIGSEGV:
			signal_set(sig, crash_handler);
			break;
		default:
			signal_set(sig, SIG_DFL);
			break;
		}
	}

	/*
	 * Unblock SIGBUS or SIGSEGV if it is the signal we're handling, so
	 * that we can have them delivered again.
	 *
	 * In case the code we're calling also encounters an assertion failure,
	 * we need to unblock SIGBABRT as well.
	 */

	switch (signo) {
#ifdef SIGBUS
	case SIGBUS:
#endif
	case SIGSEGV:
	case SIGABRT:
		signal_unblock(signo);
	}

	/*
	 * Crashing early means we can't be called from a signal handler: rather
	 * we were called manually, from crash_abort().
	 */

	if (NULL == vars)
		return;

	/*
	 * Enter crash mode and configure safe logging parameters.
	 */

	crash_mode();

	if (recursive) {
		if (!vars->recursive) {
			uint8 t = TRUE;
			crash_set_var(recursive, t);
		}
	}

	/*
	 * When crash_close() was called, print minimal error message and exit.
	 */

	name = signal_name(signo);

	if (vars->closed) {
		crash_message(name, FALSE, recursive);
		if (!recursive)
			crash_emit_decorated_stack(1, in_child);
		crash_end_of_line(FALSE);
		_exit(EXIT_FAILURE);
	}

	/*
	 * Try to go back to the crashing directory, if configured, when we're
	 * about to exec() a process, so that the core dump happens there,
	 * even if we're daemonized.
	 */

	if (!recursive && NULL != vars->crashdir && vars->invoke_inspector) {
		if (-1 == chdir(vars->crashdir)) {
			if (NULL != vars->cwd) {
				s_miniwarn("cannot chdir() back to \"%s\", "
					"staying in \"%s\" (errno = %d)",
					vars->crashdir, vars->cwd, errno);
				cwd = vars->cwd;
			} else {
				s_miniwarn("cannot chdir() back to \"%s\" (errno = %d)",
					vars->crashdir, errno);
			}
		} else {
			cwd = vars->crashdir;
		}
	}

	if (recursive && NULL != vars->crashdir && vars->invoke_inspector) {
		/*
		 * We've likely chdir-ed back there when recursing.  It's a better
		 * default value than "" anyway.
		 */
		cwd = vars->crashdir;
	}

	trace = recursive ? FALSE : !stacktrace_cautious_was_logged();

	crash_message(name, trace, recursive);
	if (trace) {
		crash_stack_print(STDERR_FILENO, 1);
		if (log_stdout_is_distinct())
			crash_stack_print(STDOUT_FILENO, 1);

		/*
		 * If we are in a signal handler and are not going to invoke an
		 * inspector, dump a decorated stack.
		 */

		if (signal_in_handler() && !vars->invoke_inspector)
			crash_emit_decorated_stack(1, in_child);
	}
	if ((recursive && 1 == crashed) || in_child) {
		crash_emit_decorated_stack(1, in_child);
		crash_end_of_line(TRUE);
		goto the_end;
	}
	if (!vars->invoke_inspector)
		crash_generate_crashlog(signo);
	crash_end_of_line(FALSE);
	if (vars->invoke_inspector) {
		bool hooks;

		/*
		 * If we have no stackframe, then we're probably not on an assertion
		 * failure path.  Capture the stack including the crash handler so
		 * that we know were the capture was made from.
		 */

		if (0 == vars->stackcnt)
			crash_save_current_stackframe(0);

		hooks = crash_invoke_inspector(signo, cwd);
		if (!hooks) {
			uint8 f = FALSE;
			crash_run_hooks(NULL, -1);
			crash_set_var(invoke_inspector, f);
			crash_end_of_line(FALSE);
		}
	}
	if (vars->pause_process && vars->invoke_inspector) {
		uint8 f = FALSE;
		crash_set_var(invoke_inspector, f);
		crash_end_of_line(FALSE);
	}
	if (vars->pause_process) {
		compat_pause();
	}

the_end:
	if (!in_child)
		crash_auto_restart();
	raise(SIGABRT);			/* This is the end of our road */
}

static void *
crash_ck_allocator(void *allocator, size_t len)
{
	return ck_alloc(allocator, len);
}

/**
 * Alter crash flags.
 */
G_GNUC_COLD void
crash_ctl(enum crash_alter_mode mode, int flags)
{
	uint8 value;

	g_assert(CRASH_FLAG_SET == mode || CRASH_FLAG_CLEAR == mode);

	value = booleanize(CRASH_FLAG_SET == mode);

	if (CRASH_F_PAUSE & flags)
		crash_set_var(pause_process, value);

	if (CRASH_F_GDB & flags)
		crash_set_var(invoke_inspector, value);

	if (CRASH_F_RESTART & flags)
		crash_set_var(may_restart, value);
}

/**
 * Installs a simple crash handler.
 * 
 * @param argv0		the original argv[0] from main().
 * @param progname	the program name, to generate the proper crash file
 * @param flags		combination of CRASH_F_GDB, CRASH_F_PAUSE, CRASH_F_RESTART
 * @parah exec_path	pathname of custom program to execute on crash
 */
G_GNUC_COLD void
crash_init(const char *argv0, const char *progname,
	int flags, const char *exec_path)
{
	struct crash_vars iv;
	unsigned i;
	char dir[MAX_PATH_LEN];
	char *executable = NULL;

	ZERO(&iv);

	/*
	 * Must set this early in case we have to call crash_time(), since
	 * vars->gtmoff must be set.
	 */

	iv.gmtoff = timestamp_gmt_offset(time(NULL), NULL);
	vars = &iv;

	if (NULL == getcwd(dir, sizeof dir)) {
		char time_buf[18];
		DECLARE_STR(4);

		crash_time(time_buf, sizeof time_buf);

		dir[0] = '\0';
		print_str(time_buf);
		print_str(" (WARNING): cannot get current working directory: ");
		print_str(g_strerror(errno));
		print_str("\n");
		flush_err_str();
	}

	if (NULL != exec_path) {
		filestat_t buf;

		if (
			-1 == stat(exec_path, &buf) ||
			!S_ISREG(buf.st_mode) || 
			-1 == access(exec_path, X_OK)
		) {
			char time_buf[18];
			DECLARE_STR(4);

			crash_time(time_buf, sizeof time_buf);

			print_str(time_buf);
			print_str(" (ERROR): unusable program \"");
			print_str(exec_path);
			print_str("\"\n");
			flush_err_str();
			exit(EXIT_FAILURE);
		}
	}

	/*
	 * We hand out the executable path in case we have to invoke gdb, since
	 * this is required on some platform.  Make sure this is a full path, or
	 * a valid relative path from our initial working directory (which will
	 * be restored on crash if the executable path ends up being relative).
	 */

	if (argv0 != NULL && !file_exists(argv0))
		executable = file_locate_from_path(argv0);

	if (NULL == executable)
		executable = deconstify_char(argv0);

	iv.mem = ck_init_not_leaking(sizeof iv, 0);

	if ('\0' != dir[0]) {
		iv.cwd = ostrdup_readonly(dir);
		g_assert(NULL != iv.cwd);
	}

	iv.argv0 = ostrdup_readonly(executable);
	g_assert(NULL == executable || NULL != iv.argv0);

	iv.progname = ostrdup_readonly(progname);
	g_assert(NULL == progname || NULL != iv.progname);

	iv.exec_path = ostrdup_readonly(exec_path);
	g_assert(NULL == exec_path || NULL != iv.exec_path);

	iv.pause_process = booleanize(CRASH_F_PAUSE & flags);
	iv.invoke_inspector = booleanize(CRASH_F_GDB & flags) || NULL != exec_path;
	iv.may_restart = booleanize(CRASH_F_RESTART & flags);
	iv.dumps_core = booleanize(!crash_coredumps_disabled());
	iv.start_time = time(NULL);
	iv.pid = getpid();

	for (i = 0; i < G_N_ELEMENTS(signals); i++) {
		signal_set(signals[i], crash_handler);
	}

	vars = ck_copy(iv.mem, &iv, sizeof iv);
	ck_readonly(vars->mem);

	if (executable != argv0)
		HFREE_NULL(executable);

	/*
	 * This chunk is used to save error messages and to hold a string object
	 * that can be used to format an error message.
	 *
	 * After initialization, the chunk is turned read-only to avoid accidental
	 * corruption until the time we need to use the string object.
	 */

	{
		ckhunk_t *logck;
		str_t *logstr;

		logck = ck_init_not_leaking(compat_pagesize(), 0);
		crash_set_var(logck, logck);

		logstr = str_new_in_chunk(logck, CRASH_MSG_MAXLEN);
		crash_set_var(logstr, logstr);

		ck_readonly(vars->logck);
	}

	/*
	 * This chunk is used to hold a string object that can be used to format
	 * logs during crashes to bypass fprintf().
	 *
	 * After initialization, the chunk is turned read-only to avoid accidental
	 * corruption until the time we need to use the string object during a
	 * crash.
	 */

	{
		ckhunk_t *fmtck;
		str_t *str;

		fmtck = ck_init_not_leaking(compat_pagesize(), 0);
		crash_set_var(fmtck, fmtck);

		str = str_new_in_chunk(fmtck, CRASH_MSG_MAXLEN);
		crash_set_var(fmtstr, str);

		ck_readonly(vars->fmtck);
	}

	/*
	 * This chunk is used to record "on-crash" handlers.
	 */

	{
		ckhunk_t *hookmem;
		hash_table_t *ht;

		hookmem = ck_init_not_leaking(compat_pagesize(), 0);
		crash_set_var(hookmem, hookmem);

		ht = hash_table_new_special_full(crash_ck_allocator, hookmem,
			string_mix_hash, string_eq);
		crash_set_var(hooks, ht);

		hash_table_readonly(ht);
		ck_readonly(vars->hookmem);
	}
}

/**
 * Generate crashfile environment variable into destination buffer.
 *
 * @param dst			the destination buffer, may be NULL for dry run.
 * @param dst_size		the size of the destination buffer in bytes.
 * @param pathname		the directory where crash file is to be held
 *
 * @return Required buffer size.
 */
static G_GNUC_COLD size_t
crashfile_name(char *dst, size_t dst_size, const char *pathname)
{
	const char *pid_str, *item;
	char pid_buf[ULONG_DEC_BUFLEN];
	char filename[80];
	size_t size = 1;	/* Minimum is one byte for NUL */

	/* @BUG: The ADNS helper process has a different PID.  */
	pid_str = print_number(pid_buf, sizeof pid_buf, getpid());
	crash_logname(filename, sizeof filename, pid_str);

	if (NULL == dst) {
		dst = deconstify_char("");
		dst_size = 0;
	}

	item = "Crashfile=";
	clamp_strcpy(dst, dst_size, item);
	size = size_saturate_add(size, strlen(item));

	item = pathname;
	clamp_strcat(dst, dst_size, item);
	size = size_saturate_add(size, strlen(item));

	item = G_DIR_SEPARATOR_S;
	clamp_strcat(dst, dst_size, item);
	size = size_saturate_add(size, strlen(item));

	item = filename;
	clamp_strcat(dst, dst_size, item);
	size = size_saturate_add(size, strlen(item));

	return size;
}

/**
 * Record current working directory and configured crash directory.
 */
G_GNUC_COLD void
crash_setdir(const char *pathname)
{
	const char *curdir = NULL;
	size_t crashfile_size = 0;
	char dir[MAX_PATH_LEN];

	if (
		NULL != getcwd(dir, sizeof dir) &&
		(NULL == vars->cwd || 0 != strcmp(dir, vars->cwd))
	) {
		curdir = dir;
	}

	/*
	 * When they specified an exec_path, we generate the environment
	 * string "Crashfile=pathname" which will be used to pass the name
	 * of the crashfile to the program.
	 */

	if (has_fork() && NULL != vars->exec_path) {
		crashfile_size = crashfile_name(NULL, 0, pathname);
	}

	if (crashfile_size > 0) {
		char *crashfile = xmalloc(crashfile_size);
		const char *ro;

		crashfile_name(crashfile, crashfile_size, pathname);
		ro = ostrdup_readonly(crashfile);
		crash_set_var(crashfile, ro);
		xfree(crashfile);
	}

	curdir = ostrdup_readonly(curdir);
	pathname = ostrdup_readonly(pathname);

	crash_set_var(crashdir, pathname);
	if (curdir != NULL) {
		crash_set_var(cwd, curdir);
	}
}

/**
 * Record program's version string.
 */
G_GNUC_COLD void
crash_setver(const char *version)
{
	const char *value;

	g_assert(NULL != vars->mem);
	g_assert(NULL != version);

	value = ostrdup_readonly(version);
	crash_set_var(version, value);

	g_assert(NULL != vars->version);
}

/**
 * Set program's numbers (major, minor and patchlevel).
 */
void
crash_setnumbers(uint8 major, uint8 minor, uint8 patchlevel)
{
	uint8 t = TRUE;

	crash_set_var(major, major);
	crash_set_var(minor, minor);
	crash_set_var(patchlevel, patchlevel);
	crash_set_var(has_numbers, t);
}

/**
 * Set program's build number.
 */
void
crash_setbuild(unsigned build)
{
	crash_set_var(build, build);
}

/**
 * Save original argc/argv and environment.
 *
 * These should not be the original argv[] and environ pointer but rather
 * copies that point to read-only memory to prevent tampering.
 *
 * The gm_dupmain() routine handles this duplication into a read-only memory
 * region and it should ideally be called before calling this routine.
 */
void
crash_setmain(int argc, const char *argv[], const char *env[])
{
	crash_set_var(argc, argc);
	crash_set_var(argv, argv);
	crash_set_var(envp, env);
}

/**
 * Record a crash hook for a file.
 */
void
crash_hook_add(const char *filename, const crash_hook_t hook)
{
	g_assert(filename != NULL);
	g_assert(hook != NULL);
	g_assert(vars != NULL);			/* Must have run crash_init() */

	/*
	 * Only one crash hook can be added per file.
	 */

	if (hash_table_contains(vars->hooks, filename)) {
		const void *oldhook = hash_table_lookup(vars->hooks, filename);
		s_carp("CRASH cannot add hook \"%s\" for \"%s\", already have \"%s\"",
			stacktrace_function_name(hook),
			filename, stacktrace_routine_name(oldhook, FALSE));
	} else {
		ck_writable(vars->hookmem);			/* Holds the hash table object */
		hash_table_writable(vars->hooks);
		hash_table_insert(vars->hooks, filename, func_to_pointer(hook));
		hash_table_readonly(vars->hooks);
		ck_readonly(vars->hookmem);
	}
}

/**
 * Final call to signal that crash initialization is done and we can now
 * shrink the pre-sized data structures to avoid wasting too much space.
 */
void
crash_post_init(void)
{
	/* Nothing to be done currently */
}

/**
 * Called at exit() time, when all the program data structures have been
 * released and when we give control back to possible atexit() handlers.
 *
 * When xmalloc() is malloc(), it is possible to get occasional SIGSEGV
 * in exit handlers from gdk_exit() in the XCloseDisplay() sequence.
 *
 * When that happens, we don't want to pause() or dump a core.
 */
void
crash_close(void)
{
	crash_closed = TRUE;

	if (vars != NULL) {
		uint8 t = TRUE;
		crash_set_var(closed, t);
	}
}

/**
 * Are we done?
 *
 * @return TRUE if crash_close() has been called.
 */
bool
crash_is_closed(void)
{
	if (vars != NULL)
		return vars->closed;

	return crash_closed;
}

/**
 * Abort execution, synchronously.
 */
void
crash_abort(void)
{
	crash_handler(SIGABRT);
	abort();
}

/**
 * Re-execute the same process with the same arguments.
 *
 * This function does not return: either it succeeds exec()ing or it exits.
 */
G_GNUC_COLD void
crash_reexec(void)
{
	crash_mode();		/* Not really, but prevents any memory allocation */

	crash_try_reexec();
	_exit(EXIT_FAILURE);
}

/***
 *** Calling any of the following routines means we're about to crash.
 ***/

/**
 * Record failed assertion data.
 */
G_GNUC_COLD void
crash_assert_failure(const struct assertion_data *a)
{
	crash_mode();

	if (vars != NULL)
		crash_set_var(failure, a);
}

/**
 * Record additional assertion message.
 *
 * @return formatted message string, NULL if it could not be built
 */
G_GNUC_COLD const char *
crash_assert_logv(const char * const fmt, va_list ap)
{
	crash_mode();

	if (vars != NULL && vars->logstr != NULL) {
		const char *msg;

		/*
		 * The string we use for formatting is held in a read-only chunk.
		 * Before formatting inside, we must therfore make the chunk writable,
		 * turning it back to read-only after formatting to prevent tampering.
		 */

		ck_writable(vars->logck);
		str_vprintf(vars->logstr, fmt, ap);
		msg = str_2c(vars->logstr);
		ck_readonly(vars->logck);
		crash_set_var(message, msg);
		return msg;
	} else {
		static char msg[CRASH_MSG_SAFELEN];

		str_vbprintf(msg, sizeof msg, fmt, ap);
		return msg;
	}
}

/**
 * Record the name of the file from which we're crashing.
 *
 * This allows triggering of crash hooks, if any defined for the file.
 */
G_GNUC_COLD void
crash_set_filename(const char * const filename)
{
	crash_mode();

	if (vars != NULL && vars->logck != NULL) {
		const char *f = ck_strdup_readonly(vars->logck, filename);
		crash_set_var(filename, f);
	}
}

/**
 * Record crash error message.
 */
G_GNUC_COLD void
crash_set_error(const char * const msg)
{
	crash_mode();

	if (vars != NULL && vars->logck != NULL) {
		const char *m;

		/*
		 * The string we use for formatting is held in a read-only chunk.
		 * Before formatting inside, we must therfore make the chunk writable,
		 * turning it back to read-only after formatting to prevent tampering.
		 */

		ck_writable(vars->logck);
		if (0 != str_len(vars->logstr))
			str_ncat_safe(vars->logstr, ", ", 2);
		str_ncat_safe(vars->logstr, msg, strlen(msg));
		m = str_2c(vars->logstr);
		ck_readonly(vars->logck);
		crash_set_var(message, m);
	}
}

/**
 * Append information to existing error message.
 */
G_GNUC_COLD void
crash_append_error(const char * const msg)
{
	crash_mode();

	if (vars != NULL && vars->logck != NULL) {
		const char *m;

		/*
		 * The string we use for formatting is held in a read-only chunk.
		 * Before formatting inside, we must therfore make the chunk writable,
		 * turning it back to read-only after formatting to prevent tampering.
		 */

		ck_writable(vars->logck);
		str_ncat_safe(vars->logstr, msg, strlen(msg));
		m = str_2c(vars->logstr);
		ck_readonly(vars->logck);
		crash_set_var(message, m);
	}
}

/**
 * Save given stack trace, which will be displayed during crashes instead
 * of the current stack frame.
 */
G_GNUC_COLD void
crash_save_stackframe(void *stack[], size_t count)
{
	crash_mode();

	if (count > G_N_ELEMENTS(vars->stack))
		count = G_N_ELEMENTS(vars->stack);

	if (vars != NULL && 0 == vars->stackcnt) {
		ck_memcpy(vars->mem,
			&vars->stack, (void *) stack, count * sizeof(void *));
		crash_set_var(stackcnt, count);
	}
}

/**
 * Capture current stack frame during assertion failures.
 *
 * The reason we capture a stack frame at the moment of the assertion failure
 * is to protect against SIGABRT signal delivery happening on a dedicated
 * signal stack.
 */
G_GNUC_COLD void
crash_save_current_stackframe(unsigned offset)
{
	crash_mode();

	if (vars != NULL && 0 == vars->stackcnt) {
		void *stack[STACKTRACE_DEPTH_MAX];
		size_t count;

		count = stacktrace_safe_unwind(stack, G_N_ELEMENTS(stack), offset + 1);
		crash_save_stackframe(stack, count);
	}
}

/* vi: set ts=4 sw=4 cindent: */
