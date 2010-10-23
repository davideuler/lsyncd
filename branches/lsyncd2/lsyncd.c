/** 
 * lsyncd.c   Live (Mirror) Syncing Demon
 *
 * License: GPLv2 (see COPYING) or any later version
 *
 * Authors: Axel Kittenberger <axkibe@gmail.com>
 *
 * This is the core. It contains as minimal as possible glues 
 * to the operating system needed for lsyncd operation. All high-level
 * logic is coded (when feasable) into lsyncd.lua
 */
#include "config.h"
#define LUA_USE_APICHECK 1

#ifdef HAVE_SYS_INOTIFY_H
#  include <sys/inotify.h>
#else
#  error Missing <sys/inotify.h> please supply kernel-headers and rerun configure
#endif

#include <sys/stat.h>
#include <sys/times.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#include <lua.h>
#include <lualib.h>
#include <lauxlib.h>

/**
 * Macros to compare times() values
 * (borrowed from linux/jiffies.h)
 *
 * time_after(a,b) returns true if the time a is after time b.
 */
#define time_after(a,b)         ((long)(b) - (long)(a) < 0)
#define time_before(a,b)        time_after(b,a)
#define time_after_eq(a,b)      ((long)(a) - (long)(b) >= 0)
#define time_before_eq(a,b)     time_after_eq(b,a)


/**
 * Event types core sends to runner.
 */
enum event_type {
	NONE     = 0,
	ATTRIB   = 1,
	MODIFY   = 2,
	CREATE   = 3,
	DELETE   = 4,
	MOVE     = 5,
	/* MOVEFROM/TO never get passed to the runner, but it uses these
	 * enums to split events again. The core will only send complete
	 * move events to the runner. Moves into or out of the watch tree
	 * are replaced with CREATE/DELETE events. */
	MOVEFROM = 6, 
	MOVETO   = 7,
};

/**
 * The Lua part of lsyncd.
 */
#define LSYNCD_DEFAULT_RUNNER_FILE "lsyncd.lua"
static char * lsyncd_runner_file = NULL;
static char * lsyncd_config_file = NULL;

/**
 * The inotify file descriptor.
 */
static int inotify_fd;

/**
 * TODO allow configure.
 */
static const uint32_t standard_event_mask =
		IN_ATTRIB   | IN_CLOSE_WRITE | IN_CREATE     |
		IN_DELETE   | IN_DELETE_SELF | IN_MOVED_FROM |
		IN_MOVED_TO | IN_DONT_FOLLOW | IN_ONLYDIR;


/** 
 * If not null lsyncd logs in this file.
 */
static char * logfile = NULL;

/**
 * If true lsyncd sends log messages to syslog
 */
bool logsyslog = false;

/**
 * lsyncd log level
 */
static enum loglevel {
	DEBUG   = 1,    /* Log debug messages       */
	VERBOSE = 2,    /* Log more                 */
	NORMAL  = 3,    /* Log short summeries      */
	ERROR   = 4,    /* Log severe errors only   */
	CORE    = 0x80  /* Indicates a core message */
} loglevel = DEBUG;  // TODO

/**
 * True when lsyncd daemonized itself.
 */
static bool is_daemon;

/**
 * Set to TERM or HUP in signal handler, when lsyncd should end or reset ASAP.
 */
static volatile sig_atomic_t reset = 0;

/**
 * The kernels clock ticks per second.
 */
static long clocks_per_sec; 

/**
 * predeclerations -- see belorw.
 */
static void 
logstring0(enum loglevel level,
	  const char *message);
/* core logs with CORE flag */
#define logstring(loglevel, message) logstring0(loglevel | CORE, message)

static void
printlogf(lua_State *L, 
          enum loglevel level, 
		  const char *fmt, ...)
	__attribute__((format(printf, 3, 4)));


/**
 * "secured" calloc.
 */
void *
s_calloc(size_t nmemb, size_t size)
{
	void *r = calloc(nmemb, size);
	if (r == NULL) {
		logstring(ERROR, "Out of memory!");
		exit(-1); // ERRNO 
	}
	return r;
}

/**
 * "secured" malloc. the deamon shall kill itself
 * in case of out of memory.
 */
void *
s_malloc(size_t size)
{
	void *r = malloc(size);
	if (r == NULL) {
		logstring(ERROR, "Out of memory!");
		exit(-1);  // ERRNO
	}
	return r;
}

/**
 * "secured" realloc.
 */
void *
s_realloc(void *ptr, size_t size)
{
	void *r = realloc(ptr, size);
	if (r == NULL) {
		logstring(ERROR, "Out of memory!");
		exit(-1);
	}
	return r;
}

/**
 * "secured" strdup.
 */
char *
s_strdup(const char *src)
{
	char *s = strdup(src);
	if (s == NULL) {
		logstring(ERROR, "Out of memory!");
		exit(-1); // ERRNO
	}
	return s;
}

/**
 * Logs a string
 *
 * @param level   the level of the log message
 * @param message the log message
 */
static void 
logstring0(enum loglevel level,
	   const char *message)
{
	const char *prefix;
	/* true if logmessages comes from core */
	bool coremsg = (level & CORE) != 0;
	/* strip flags from level */
	level &= 0x0F;

	/* skips filtered messagaes */
	if (level < loglevel) {
		return;
	}

	prefix = level == ERROR ? 
	                  (coremsg ? "CORE ERROR: " : "ERROR: ") :
	                  (coremsg ? "core: " : "");

	/* writes on console */
	if (!is_daemon) {
		char ct[255];
		/* gets current timestamp hour:minute:second */
		time_t mtime;
		time(&mtime);
		strftime(ct, sizeof(ct), "%T", localtime(&mtime));
		FILE * flog = level == ERROR ? stderr : stdout;
		fprintf(flog, "%s %s%s\n", ct, prefix, message);
	}

	/* writes on file */
	if (logfile) {
		FILE * flog = fopen(logfile, "a");
		/* gets current timestamp day-time-year */
		char * ct;
		time_t mtime;
		time(&mtime);
		ct = ctime(&mtime);
	 	/* cuts trailing linefeed */
 		ct[strlen(ct) - 1] = 0;

		if (flog == NULL) {
			fprintf(stderr, "core: cannot open logfile [%s]!\n", logfile);
			exit(-1);  // ERRNO
		}
		fprintf(flog, "%s: %s%s", ct, prefix, message);
		fclose(flog);
	}

	/* sends to syslog */
	if (logsyslog) {
		int sysp;
		switch (level) {
		case DEBUG  :
			sysp = LOG_DEBUG;
			break;
		case VERBOSE :
		case NORMAL  :
			sysp = LOG_NOTICE;
			break;
		case ERROR  :
			sysp = LOG_ERR;
			break;
		default :
			/* should never happen */
			sysp = 0;
			break;
		}
		syslog(sysp, "%s%s", prefix, message);
	}
	return;
}


/*****************************************************************************
 * Library calls for lsyncd.lua
 * 
 * These are as minimal as possible glues to the operating system needed for 
 * lsyncd operation.
 *
 ****************************************************************************/

/**
 * Adds an inotify watch
 * 
 * @param dir (Lua stack) path to directory
 * @return    (Lua stack) numeric watch descriptor
 */
static int
l_add_watch(lua_State *L)
{
	const char *path = luaL_checkstring(L, 1);
	lua_Integer wd = inotify_add_watch(inotify_fd, path, standard_event_mask);
	lua_pushinteger(L, wd);
	return 1;
}

/**
 * Logs a message.
 *
 * @param loglevel (Lua stack) loglevel of massage
 * @param string   (Lua stack) the string to log
 */
static int 
l_log(lua_State *L)
{
	/* log message */
	const char * message;
	/* log level */
	int level = luaL_checkinteger(L, 1);
	/* skips filtered messages early */
	if ((level & 0x0F) < loglevel) {
		return 0;
	}
	message = luaL_checkstring(L, 2);
	logstring0(level, message);
	return 0;
}

/**
 * Returns (on Lua stack) the current kernels clock state (jiffies, times() call)
 */
static int
l_now(lua_State *L) 
{
	clock_t c = times(NULL);
	lua_pushinteger(L, c);
	return 1;
}

/**
 * Returns (on Lua stack) the addition of two clock_t times.
 *
 * TODO
 */
static int
l_addup_clocks(lua_State *L) 
{
	clock_t c1 = luaL_checkinteger(L, 1);
	clock_t c2 = luaL_checkinteger(L, 2);
	lua_pop(L, 2);
	lua_pushinteger(L, c1 + c2);
	return 1;
}
/**
 * Executes a subprocess. Does not wait for it to return.
 * 
 * @param  (Lua stack) Path to binary to call
 * @params (Lua stack) list of string as arguments
 * @return (Lua stack) the pid on success, 0 on failure.
 */
static int
l_exec(lua_State *L)
{
	const char *binary = luaL_checkstring(L, 1);
	int argc = lua_gettop(L) - 1;
	pid_t pid;
	int i;
	char const **argv = s_calloc(argc + 2, sizeof(char *));

	argv[0] = binary;
	for(i = 1; i <= argc; i++) {
		argv[i] = luaL_checkstring(L, i + 1);
	}
	argv[i] = NULL;

	pid = fork();

	if (pid == 0) {
		//if (!log->flag_nodaemon && log->logfile) {
		//	if (!freopen(log->logfile, "a", stdout)) {
		//		printlogf(log, ERROR, "cannot redirect stdout to [%s].", log->logfile);
		//	}
		//	if (!freopen(log->logfile, "a", stderr)) {
		//		printlogf(log, ERROR, "cannot redirect stderr to [%s].", log->logfile);
		//	}
		//}
		execv(binary, (char **)argv);
		// in a sane world execv does not return!
		printlogf(L, ERROR, "Failed executing [%s]!", binary);
		exit(-1); // ERRNO
	}

	free(argv);
	lua_pushnumber(L, pid);
	return 1;
}


/**
 * Converts a relative directory path to an absolute.
 * 
 * @param dir a relative path to directory
 * @return    absolute path of directory
 */
static int
l_real_dir(lua_State *L)
{
	luaL_Buffer b;
	char *cbuf;
	const char *rdir = luaL_checkstring(L, 1);
	
	/* use c-library to get absolute path */
	cbuf = realpath(rdir, NULL);
	if (cbuf == NULL) {
		printlogf(L, ERROR, "failure getting absolute path of [%s]", rdir);
		return 0;
	}
	{
		/* makes sure its a directory */
	    struct stat st;
	    stat(cbuf, &st);
	    if (!S_ISDIR(st.st_mode)) {
			printlogf(L, ERROR, "failure in real_dir [%s] is not a directory", rdir);
			free(cbuf);
			return 0;
	    }
	}

	/* returns absolute path with a concated '/' */
	luaL_buffinit(L, &b);
	luaL_addstring(&b, cbuf);
	luaL_addchar(&b, '/');
	luaL_pushresult(&b);
	free(cbuf);
	return 1;
}

/**
 * Dumps the LUA stack. For debugging purposes.
 */
static int
l_stackdump(lua_State* L)
{
	int i;
	int top = lua_gettop(L);
	printlogf(L, DEBUG, "total in stack %d\n",top);
	for (i = 1; i <= top; i++) { 
		int t = lua_type(L, i);
		switch (t) {
		case LUA_TSTRING:
			printlogf(L, DEBUG, "%d string: '%s'\n", i, lua_tostring(L, i));
			break;
		case LUA_TBOOLEAN:
			printlogf(L, DEBUG, "%d boolean %s\n", i, lua_toboolean(L, i) ? "true" : "false");
			break;
		case LUA_TNUMBER: 
			printlogf(L, DEBUG, "%d number: %g\n", i, lua_tonumber(L, i));
			break;
		default: 
			printlogf(L, DEBUG, "%d %s\n", i, lua_typename(L, t));
			break;
		}
	}
	return 0;
}

/**
 * Reads the directories sub directories.
 * 
 * @param  (Lua stack) absolute path to directory.
 * @return (Lua stack) a table of directory names.
 */
static int
l_sub_dirs (lua_State *L)
{
	const char * dirname = luaL_checkstring(L, 1);
	DIR *d;
	int idx = 1;

	d = opendir(dirname);
	if (d == NULL) {
		printlogf(L, ERROR, "cannot open dir [%s].", dirname);
		return 0;
	}
	
	lua_newtable(L);
	while (!reset) {
		struct dirent *de = readdir(d);
		bool isdir;
		if (de == NULL) {
			/* finished */
			break;
		}
		if (de->d_type == DT_UNKNOWN) {
			/* must call stat on some systems :-/ */
			char *subdir = s_malloc(strlen(dirname) + strlen(de->d_name) + 2);
			struct stat st;
			strcpy(subdir, dirname);
			strcat(subdir, "/");
			strcat(subdir, de->d_name);
			stat(subdir, &st);
			isdir = S_ISDIR(st.st_mode);
			free(subdir);
		} else {
			/* readdir can trusted */
			isdir = de->d_type == DT_DIR;
		}
		if (!isdir || !strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) {
			/* ignore non directories and . and .. */
			continue;
		}

		/* add this to the Lua table */
		lua_pushnumber(L, idx++);
		lua_pushstring(L, de->d_name);
		lua_settable(L, -3);
	}
	return 1;
}

/**
 * Terminates lsyncd daemon.
 * 
 * @param (Lua stack) exitcode for lsyncd.
 *
 * Does not return.
 */
int 
l_terminate(lua_State *L) 
{
	int exitcode = luaL_checkinteger(L, 1);
	exit(exitcode);
	return 0;
}

/**
 * Suspends execution until a table of child processes returned.
 * 
 * @param (Lua stack) a table of the children pids.
 * @param (Lua stack) a function of a collector to be called 
 *                    when a child finishes.
 */
int 
l_wait_pids(lua_State *L) 
{
	/* the number of pids in table */
	int pidn; 
	/* the pid table */
	int *pids; 
	/* the number of children to be waited for */
	int remaining = 0;
	int i;
	/* global function to call on finished processes */
	const char * collector;
	/* checks if Lua script returned a table */
	luaL_checktype(L, 1, LUA_TTABLE);
	if (lua_type(L, 2) == LUA_TNIL) {
		collector = NULL;
	} else {
		collector = luaL_checkstring(L, 2);
	}

	/* determines size of the pid-table */
	pidn = lua_objlen (L, 1);
	if (pidn == 0) {
		/* nothing to do on zero pids */
		return 0;
	}
	/* reads the pid table from Lua stack */
	pids = s_calloc(pidn, sizeof(int));
	for(i = 0; i < pidn; i++) {
		lua_rawgeti(L, 1, i + 1);
		pids[i] = luaL_checkinteger(L, -1);
		lua_pop(L, 1);
		/* ignores zero pids */
		if (pids[i]) {
			remaining++;
		}
	}
	/* starts waiting for the children */
	while(remaining) {
		/* argument for waitpid, and exitcode of child */
		int status, exitcode;
		/* new process id in case of retry */
		int newp;
		/* process id of terminated child process */
		int wp = waitpid(0, &status, 0);

		/* if nothing really finished ignore */
		if (wp == 0 || !WIFEXITED(status)) {
			continue;
		}

		exitcode = WEXITSTATUS(status);
		/* checks if the pid is one waited for */
		for(i = 0; i < pidn; i++) {
			if (pids[i] == wp) {
				break;
			}
		}
		if (i >= pidn) {
			/* not a pid waited for */
			continue;
		}
		/* calls the lua collector to determine further actions */
		if (collector) {
			lua_getglobal(L, collector);
			lua_pushinteger(L, wp);
			lua_pushinteger(L, exitcode);
			lua_call(L, 2, 1);
			newp = luaL_checkinteger(L, -1);
			lua_pop(L, 1);
		} else {
			newp = 0;
		}

		/* replace the new pid in the pidtable,
		   or zero it on no new pid */
		for(i = 0; i < pidn; i++) {
			if (pids[i] == wp) {
				pids[i] = newp;
				if (newp == 0) {
					remaining--;
				}
				/* does not break, in case there are duplicate pids (whyever) */
			}
		}
	}
	free(pids);
	return 0;
}


static const luaL_reg lsyncdlib[] = {
		{"addup_clocks", l_addup_clocks },
		{"add_watch",    l_add_watch    },
		{"log",          l_log          },
		{"now",          l_now          },
		{"exec",         l_exec         },
		{"real_dir",     l_real_dir     },
		{"stackdump",    l_stackdump    },
		{"sub_dirs",     l_sub_dirs     },
		{"terminate",    l_terminate    },
		{"wait_pids",    l_wait_pids    },
		{NULL, NULL}
};


/*****************************************************************************
 * Lsyncd Core 
 ****************************************************************************/

/**
 * Let the core print logmessage comfortably.
 */
static void
printlogf(lua_State *L, 
          enum loglevel level, 
	  const char *fmt, ...)
{
	va_list ap;
	/* skips filtered messages early */
	if (level < loglevel) {
		return;
	}
	lua_pushcfunction(L, l_log);
	lua_pushinteger(L, level | CORE);
	va_start(ap, fmt);
	lua_pushvfstring(L, fmt, ap);
	va_end(ap);
	lua_call(L, 2, 0);
	return;
}

/**
 * Transfers the core relevant settings from lua's global "settings" into core.
 * This saves time in normal operation instead of bothering lua all the time.
 */
 /*
void
get_settings(lua_State *L)
{
	if (settings.logfile) {
		free(settings.logfile);
		settings.logfile = NULL;
	}
	lua_getglobal(L, "settings");
	if (!lua_istable(L, -1)) {
		return;
	}
	
	lua_pushstring(L, "logfile");
	lua_gettable(L, -2);
	if (settings.logfile) {
		free(settings.logfile);
		settings.logfile = NULL;
	}
	if (lua_isstring(L, -1)) {
		settings.logfile = s_strdup(luaL_checkstring(L, -1));
	}
	lua_pop(L, 1);
	
	lua_pop(L, 1);
}*/


/**
 * Buffer for MOVE_FROM events.
 * Lsyncd buffers MOVE_FROM events to check if 
 */
struct inotify_event * move_event_buf = NULL;
size_t move_event_buf_size = 0;
/* true if the buffer is used.*/
bool move_event = false;

/**
 * Handles an inotify event.
 */
void handle_event(lua_State *L, struct inotify_event *event) {
	/* TODO */
	int event_type = NONE;

	/* used to execute two events in case of unmatched MOVE_FROM buffer */
	struct inotify_event *after_buf = NULL;

	if (reset) {
		return;
	}
	if (event && (IN_Q_OVERFLOW & event->mask)) {
		/* and overflow happened, lets runner/user decide what to do. */
		lua_getglobal(L, "overflow");
		lua_call(L, 0, 0);
		return;
	}
	/* cancel on ignored or resetting */
	if (event && (IN_IGNORED & event->mask)) {
		return;
	}

	if (event == NULL) {
		/* a buffered MOVE_FROM is not followed by anything, 
		   thus it is unary */
		event = move_event_buf;
		event_type = DELETE;
		move_event = false;
	} else if (move_event && 
	            ( !(IN_MOVED_TO & event->mask) || 
			      event->cookie != move_event_buf->cookie) ) {
		/* there is a MOVE_FROM event in the buffer and this is not the match
		 * continue in this function iteration to handler the buffer instead */
		after_buf = event;
		event = move_event_buf;
		event_type = DELETE;
		move_event = false;
	} else if ( move_event && 
	            (IN_MOVED_TO & event->mask) && 
			    event->cookie == move_event_buf->cookie ) {
		/* this is indeed a matched move */
		event_type = MOVE;
		move_event = false;
	} else if (IN_MOVED_FROM & event->mask) {
		/* just the MOVE_FROM, buffers this event, and wait if next event is a matching 
		 * MOVED_TO of this was an unary move out of the watched tree. */
		size_t el = sizeof(struct inotify_event) + event->len;
		if (move_event_buf_size < el) {
			move_event_buf_size = el;
			move_event_buf = s_realloc(move_event_buf, el);
		}
		memcpy(move_event_buf, event, el);
		move_event = true;
		return;
	} else if (IN_MOVED_TO & event->mask) {
		/* must be an unary move-to */
		event_type = CREATE;
	} else if (IN_MOVED_FROM & event->mask) {
		/* must be an unary move-from */
		event_type = DELETE;
	} else if (IN_ATTRIB & event->mask) {
		/* just attrib change */
		event_type = ATTRIB;
	} else if (IN_CLOSE_WRITE & event->mask) {
		/* closed after written something */
		event_type = MODIFY;
	} else if (IN_CREATE & event->mask) {
		/* a new file */
		event_type = CREATE;
	} else if (IN_DELETE & event->mask) {
		/* rm'ed */
		event_type = DELETE;
	} else {
		logstring(DEBUG, "skipped some inotify event.");
		return;
	}

	/* and hands over to runner */
	lua_getglobal(L, "lsyncd_event");
	lua_pushnumber(L, event_type);
	lua_pushnumber(L, event->wd);
	lua_pushboolean(L, (event->mask & IN_ISDIR) != 0);
	if (event_type == MOVE) {
		lua_pushstring(L, move_event_buf->name);
		lua_pushstring(L, event->name);
	} else {
		lua_pushstring(L, event->name);
		lua_pushnil(L);
	}
	lua_call(L, 5, 0);

	/* if there is a buffered event executes it */
	if (after_buf) {
		handle_event(L, after_buf);
	}
}

/**
 * Normal operation happens in here.
 */
void
masterloop(lua_State *L)
{
	size_t readbuf_size = 2048;
	char *readbuf = s_malloc(readbuf_size);
	while(!reset) {
		int alarm_state;
		clock_t now = times(NULL);
		clock_t alarm_time;
		bool do_read = false;
		ssize_t len; 

		/* query runner about soonest alarm  */
		lua_getglobal(L, "lsyncd_get_alarm");
		lua_pushnumber(L, now);
		lua_call(L, 1, 2);
		alarm_state = luaL_checkinteger(L, -2);
		alarm_time = (clock_t) luaL_checknumber(L, -1);
		lua_pop(L, 2);

		if (alarm_state < 0) {
			/* there is a delay that wants to be handled already
			 * thus do not read from inotify_fd and jump directly to its handling */
			logstring(DEBUG, "immediately handling delayed entries.");
			do_read = 0;
		} else if (alarm_state > 0) {
			/* use select() to determine what happens next
			 * + a new event on inotify
			 * + an alarm on timeout  
			 * + the return of a child process */
			fd_set readfds;
			struct timeval tv;

			if (time_after(now, alarm_time)) {
				/* should never happen */
				logstring(ERROR, "critical failure, alarm_time is in past!\n");
				exit(-1); //ERRNO
			}

			tv.tv_sec  = (alarm_time - now) / clocks_per_sec;
			tv.tv_usec = (alarm_time - now) * 1000000 / clocks_per_sec % 1000000;
			/* if select returns a positive number there is data on inotify *
			 * on zero the timemout occured.                                */
			FD_ZERO(&readfds);
			FD_SET(inotify_fd, &readfds);
			do_read = select(inotify_fd + 1, &readfds, NULL, NULL, &tv);

			if (do_read) {
				logstring(DEBUG, "theres data on inotify.");
			} else {
				logstring(DEBUG, "core: select() timeout or signal.");
			}
		} else {
			// if nothing to wait for, enter a blocking read
			logstring(DEBUG, "gone blocking.");
			do_read = 1;
		}
		
		/* reads possible events from inotify stream */
		while(do_read) {
			int i = 0;
			do {
				len = read (inotify_fd, readbuf, readbuf_size);
				if (len < 0 && errno == EINVAL) {
					/* kernel > 2.6.21 indicates that way that way that
					 * the buffer was too small to fit a filename.
					 * double its size and try again. When using a lower
					 * kernel and a filename > 2KB       appears lsyncd
					 * will fail. (but does a 2KB filename really happen?)*/
					readbuf_size *= 2;
					readbuf = s_realloc(readbuf, readbuf_size);
					continue;
				}
			} while(0);
			while (i < len && !reset) {
				struct inotify_event *event = (struct inotify_event *) &readbuf[i];
				handle_event(L, event);
				i += sizeof(struct inotify_event) + event->len;
			}
			/* check if there is more data */
			{
				struct timeval tv = {.tv_sec = 0, .tv_usec = 0};
				fd_set readfds;
				FD_ZERO(&readfds);
				FD_SET(inotify_fd, &readfds);
				do_read = select(inotify_fd + 1, &readfds, NULL, NULL, &tv);
				if (do_read) {
					logstring(DEBUG, "there is more data on inotify.");
				}
			}
		} 

		/* checks if there is an unary MOVE_FROM left in the buffer */
		if (move_event) {
			handle_event(L, NULL);	
		}
	}
}


/**
 * Main
 */
int
main(int argc, char *argv[])
{
	/* position at cores (minimal) argument parsing *
	 * most arguments are parsed in the lua runner  */
	int argp = 1; 

	if (argc < 2) {
		fprintf(stderr, "Missing config file\n");
		fprintf(stderr, "Minimal Usage: %s CONFIG_FILE\n", argv[0]);
		fprintf(stderr, "  Specify --help for more help.\n");
		return -1; // ERRNO
	}

	/* kernel parameters */
	clocks_per_sec = sysconf(_SC_CLK_TCK);

	/* the Lua interpreter */
	lua_State* L;

	/* TODO check lua version */

	/* load Lua */
	L = lua_open();
	luaL_openlibs(L);
	luaL_register(L, "lsyncd", lsyncdlib);
	lua_setglobal(L, "lysncd");

	/* register event types */
	lua_pushinteger(L, ATTRIB);   lua_setglobal(L, "ATTRIB");
	lua_pushinteger(L, MODIFY);   lua_setglobal(L, "MODIFY");
	lua_pushinteger(L, CREATE);   lua_setglobal(L, "CREATE");
	lua_pushinteger(L, DELETE);   lua_setglobal(L, "DELETE");
	lua_pushinteger(L, MOVE);     lua_setglobal(L, "MOVE");
	lua_pushinteger(L, MOVEFROM); lua_setglobal(L, "MOVEFROM");
	lua_pushinteger(L, MOVETO);   lua_setglobal(L, "MOVETO");
	
	/* register log levels */
	lua_pushinteger(L, DEBUG);   lua_setglobal(L, "DEBUG");
	lua_pushinteger(L, VERBOSE); lua_setglobal(L, "VERBOSE");
	lua_pushinteger(L, NORMAL);  lua_setglobal(L, "NORMAL");
	lua_pushinteger(L, ERROR);   lua_setglobal(L, "ERROR");

	/* TODO parse runner */
	if (!strcmp(argv[argp], "--runner")) {
		if (argc < 3) {
			fprintf(stderr, "Lsyncd Lua-runner file missing after --runner.\n");
			return -1; //ERRNO
		}
		if (argc < 4) {
			fprintf(stderr, "Missing config file\n");
			fprintf(stderr, "  Usage: %s --runner %s CONFIG_FILE\n", argv[0], argv[2]);
			fprintf(stderr, "  Specify --help for more help.\n");
			return -1; // ERRNO
		}
		lsyncd_runner_file = argv[argp + 1];
		argp += 2;
	} else {
		lsyncd_runner_file = LSYNCD_DEFAULT_RUNNER_FILE;
	}
	lsyncd_config_file = argv[argp];
	{
		struct stat st;
		if (stat(lsyncd_runner_file, &st)) {
			fprintf(stderr, "Cannot find Lsyncd Lua-runner at %s.\n", lsyncd_runner_file);
			fprintf(stderr, "Maybe specify another place? %s --runner RUNNER_FILE CONFIG_FILE\n", argv[0]);
			return -1; // ERRNO
		}
		if (stat(lsyncd_config_file, &st)) {
			fprintf(stderr, "Cannot find config file at %s.\n", lsyncd_config_file);
			return -1; // ERRNO
		}
	}

	if (luaL_loadfile(L, lsyncd_runner_file)) {
		fprintf(stderr, "error loading '%s': %s\n", 
		       lsyncd_runner_file, lua_tostring(L, -1));
		return -1; // ERRNO
	}
	if (lua_pcall(L, 0, LUA_MULTRET, 0)) {
		fprintf(stderr, "error preparing '%s': %s\n", 
		       lsyncd_runner_file, lua_tostring(L, -1));
		return -1; // ERRNO
	}

	{
		/* checks version match between runner/core */
		const char *lversion;
		lua_getglobal(L, "lsyncd_version");
		lversion = luaL_checkstring(L, -1);
		lua_pop(L, 1);
		if (strcmp(lversion, PACKAGE_VERSION)) {
			fprintf(stderr, "Version mismatch '%s' is '%s', but core is '%s'\n",
 			        lsyncd_runner_file, lversion, PACKAGE_VERSION);
			return -1; // ERRNO
		}
	}

	if (luaL_loadfile(L, lsyncd_config_file)) {
		fprintf(stderr, "error loading %s: %s\n", lsyncd_config_file, lua_tostring(L, -1));
		return -1; // ERRNO
	}
	if (lua_pcall(L, 0, LUA_MULTRET, 0)) {
		fprintf(stderr, "error preparing %s: %s\n", lsyncd_config_file, lua_tostring(L, -1));
		return -1; // ERRNO
	}

	/* open inotify */
	inotify_fd = inotify_init();
	if (inotify_fd == -1) {
		fprintf(stderr, "Cannot create inotify instance! (%d:%s)\n", errno, strerror(errno));
		return -1; // ERRNO
	}

	/* initialize */
	/* lua code will set configuration and add watches */
	lua_getglobal(L, "lsyncd_initialize");
	lua_call(L, 0, 0);

	masterloop(L);

	/* cleanup */
	close(inotify_fd);
	lua_close(L);
	return 0;
}