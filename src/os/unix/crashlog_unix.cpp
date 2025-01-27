/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <http://www.gnu.org/licenses/>.
 */

/** @file crashlog_unix.cpp Unix crash log handler */

#include "../../stdafx.h"
#include "../../crashlog.h"
#include "../../string_func.h"
#include "../../gamelog.h"
#include "../../saveload/saveload.h"

#include <errno.h>
#include <signal.h>
#include <sys/utsname.h>

#if defined(__GLIBC__)
/* Execinfo (and thus making stacktraces) is a GNU extension */
#	include <execinfo.h>
#elif defined(SUNOS)
#	include <ucontext.h>
#	include <dlfcn.h>
#endif

#if defined(__NetBSD__)
#include <unistd.h>
#endif

#include "../../safeguards.h"

/**
 * Unix implementation for the crash logger.
 */
class CrashLogUnix : public CrashLog {
	/** Signal that has been thrown. */
	int signum;

	char *LogOSVersion(char *buffer, const char *last) const override
	{
		struct utsname name;
		if (uname(&name) < 0) {
			return buffer + seprintf(buffer, last, "Could not get OS version: %s\n", strerror(errno));
		}

		return buffer + seprintf(buffer, last,
				"Operating system:\n"
				" Name:     %s\n"
				" Release:  %s\n"
				" Version:  %s\n"
				" Machine:  %s\n",
				name.sysname,
				name.release,
				name.version,
				name.machine
		);
	}

	char *LogError(char *buffer, const char *last, const char *message) const override
	{
		return buffer + seprintf(buffer, last,
				"Crash reason:\n"
				" Signal:  %s (%d)\n"
				" Message: %s\n\n",
				strsignal(this->signum),
				this->signum,
				message
		);
	}

#if defined(SUNOS)
	/** Data needed while walking up the stack */
	struct StackWalkerParams {
		char **bufptr;    ///< Buffer
		const char *last; ///< End of buffer
		int counter;      ///< We are at counter-th stack level
	};

	/**
	 * Callback used while walking up the stack.
	 * @param pc program counter
	 * @param sig 'active' signal (unused)
	 * @param params parameters
	 * @return always 0, continue walking up the stack
	 */
	static int SunOSStackWalker(uintptr_t pc, int sig, void *params)
	{
		StackWalkerParams *wp = (StackWalkerParams *)params;

		/* Resolve program counter to file and nearest symbol (if possible) */
		Dl_info dli;
		if (dladdr((void *)pc, &dli) != 0) {
			*wp->bufptr += seprintf(*wp->bufptr, wp->last, " [%02i] %s(%s+0x%x) [0x%x]\n",
					wp->counter, dli.dli_fname, dli.dli_sname, (int)((byte *)pc - (byte *)dli.dli_saddr), (uint)pc);
		} else {
			*wp->bufptr += seprintf(*wp->bufptr, wp->last, " [%02i] [0x%x]\n", wp->counter, (uint)pc);
		}
		wp->counter++;

		return 0;
	}
#endif

	char *LogStacktrace(char *buffer, const char *last) const override
	{
		buffer += seprintf(buffer, last, "Stacktrace:\n");
#if defined(__GLIBC__)
		void *trace[64];
		int trace_size = backtrace(trace, lengthof(trace));

		char **messages = backtrace_symbols(trace, trace_size);
		for (int i = 0; i < trace_size; i++) {
			buffer += seprintf(buffer, last, " [%02i] %s\n", i, messages[i]);
		}
		free(messages);
#elif defined(SUNOS)
		ucontext_t uc;
		if (getcontext(&uc) != 0) {
			buffer += seprintf(buffer, last, " getcontext() failed\n\n");
			return buffer;
		}

		StackWalkerParams wp = { &buffer, last, 0 };
		walkcontext(&uc, &CrashLogUnix::SunOSStackWalker, &wp);
#else
		buffer += seprintf(buffer, last, " Not supported.\n");
#endif
		return buffer + seprintf(buffer, last, "\n");
	}
public:
	/**
	 * A crash log is always generated by signal.
	 * @param signum the signal that was caused by the crash.
	 */
	CrashLogUnix(int signum) :
		signum(signum)
	{
	}
};

/** The signals we want our crash handler to handle. */
static const int _signals_to_handle[] = { SIGSEGV, SIGABRT, SIGFPE, SIGBUS, SIGILL };

/**
 * Entry point for the crash handler.
 * @note Not static so it shows up in the backtrace.
 * @param signum the signal that caused us to crash.
 */
static void CDECL HandleCrash(int signum)
{
	/* Disable all handling of signals by us, so we don't go into infinite loops. */
	for (const int *i = _signals_to_handle; i != endof(_signals_to_handle); i++) {
		signal(*i, SIG_DFL);
	}

	if (GamelogTestEmergency()) {
		printf("A serious fault condition occurred in the game. The game will shut down.\n");
		printf("As you loaded an emergency savegame no crash information will be generated.\n");
		abort();
	}

	if (SaveloadCrashWithMissingNewGRFs()) {
		printf("A serious fault condition occurred in the game. The game will shut down.\n");
		printf("As you loaded an savegame for which you do not have the required NewGRFs\n");
		printf("no crash information will be generated.\n");
		abort();
	}

	CrashLogUnix log(signum);
	log.MakeCrashLog();

	CrashLog::AfterCrashLogCleanup();
	abort();
}

/* static */ void CrashLog::InitialiseCrashLog()
{
	for (const int *i = _signals_to_handle; i != endof(_signals_to_handle); i++) {
		signal(*i, HandleCrash);
	}
}

/* static */ void CrashLog::InitThread()
{
}
