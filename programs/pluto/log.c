/* error logging functions, for libreswan
 *
 * Copyright (C) 1997 Angelos D. Keromytis.
 * Copyright (C) 1998-2001,2013 D. Hugh Redelmeier <hugh@mimosa.com>
 * Copyright (C) 2005-2007 Michael Richardson
 * Copyright (C) 2006-2010 Bart Trojanowski
 * Copyright (C) 2008-2012 Paul Wouters
 * Copyright (C) 2008-2010 David McCullough.
 * Copyright (C) 2012 Paul Wouters <paul@libreswan.org>
 * Copyright (C) 2013,2015 Paul Wouters <pwouters@redhat.com>
 * Copyright (C) 2013 Tuomo Soini <tis@foobar.fi>
 * Copyright (C) 2017 Andrew Cagney <cagney@gnu.org>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.  See <http://www.fsf.org/copyleft/gpl.txt>.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * for more details.
 *
 */

#include <pthread.h>    /* Must be the first include file; XXX: why? */

#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <syslog.h>

#include "defs.h"
#include "lswlog.h"
#include "log.h"
#include "peerlog.h"
#include "state_db.h"
#include "connections.h"
#include "state.h"
#include "kernel.h"	/* for kernel_ops */
#include "timer.h"

bool
	log_to_stderr = TRUE,		/* should log go to stderr? */
	log_to_syslog = TRUE,		/* should log go to syslog? */
	log_with_timestamp = TRUE,	/* testsuite requires no timestamps */
	log_append = TRUE,
	log_ip = TRUE;

char *pluto_log_file = NULL;	/* pathname */
static FILE *pluto_log_fp = NULL;

char *pluto_stats_binary = NULL;

const char *sensitive_ipstr(const ip_address *src, ipstr_buf *b)
{
	return log_ip ? ipstr(src, b) : "<ip address>";
}

/*
 * If valid, wack and log_whack streams write to this.
 *
 * (apparently) If the context provides a whack file descriptor,
 * messages should be copied to it -- see whack_log()
 */
int whack_log_fd = NULL_FD;                     /* only set during whack_handle() */

/*
 * Context for logging.
 *
 * CUR_FROM, CUR_CONNECTION and CUR_STATE work something like a stack.
 * lswlog_log_prefix() will use the first of CUR_STATE, CUR_CONNECTION
 * and CUR_FROM when looking for the context to use with a prefix.
 * Operations then "push" and "pop" (or clear all) contexts.
 *
 * For instance, setting CUR_STATE will hide CUR_CONNECTION, and
 * resetting CUR_STATE will re-expose CUR_CONNECTION.
 *
 * Surely it would be easier to explicitly specify the context with
 * something like LSWLOG_LOG_WHACK_STATE()?
 *
 * Global variables: must be carefully adjusted at transaction
 * boundaries!
 */
static struct state *cur_state = NULL;                 /* current state, for diagnostics */
static struct connection *cur_connection = NULL;       /* current connection, for diagnostics */
const ip_address *cur_from = NULL;              /* source of current current message */
u_int16_t cur_from_port;                        /* host order */

void reset_globals(void)
{
	whack_log_fd = NULL_FD;
	cur_state = NULL;
	cur_from = NULL;
	reset_cur_connection();
}


bool globals_are_reset(void)
{
	return  (whack_log_fd == NULL_FD
		 && cur_state == NULL
		 && cur_connection == NULL
		 && cur_from == NULL
		 && cur_debugging == base_debugging);
}

static void update_extra(const char *what, enum_names *names,
			 lmod_t extra, lset_t mask)
{
	if (!lmod_empty(extra)) {
		lset_t old = base_debugging & mask;
		lset_t new = lmod(old, extra);
		if (new != old) {
			LSWLOG(buf) {
				lswlogf(buf, "extra %s enabled for connection: ", what);
				lswlog_enum_lset_short(buf, names, "+", new & ~old);
				/* XXX: doesn't log cleared */
			}
			set_debugging(new | (base_debugging & ~mask));
		}
	}
}

static void update_debugging(void)
{
	struct connection *c = cur_state != NULL ? cur_state->st_connection : cur_connection;
	if (c == NULL) {
		set_debugging(base_debugging);
	} else {
		update_extra("debugging", &debug_names,
			     c->extra_debugging, DBG_MASK);
		update_extra("impairing", &impair_names,
			     c->extra_impairing, IMPAIR_MASK);
	}
}

enum processing {
	START = 1,
	SUSPEND,
	RESUME,
	STILL,
	STOP,
};

/*
 * if any debugging is on, make sure that we log the connection we are
 * processing, because it may not be clear in later debugging.
 */
static void log_processing(enum processing processing, bool current,
			   struct state *st, struct connection *c,
			   const char *func, const char *file, long line)
{
	passert((st != NULL) != (c != NULL));
	LSWDBGP(DBG_MASK, buf) {
		if (!current) {
			lswlogs(buf, "(");
		}
		switch (processing) {
		case START: lswlogs(buf, "start"); break;
		case STILL: lswlogs(buf, "still"); break;
		case SUSPEND: lswlogs(buf, "suspend"); break;
		case RESUME: lswlogs(buf, "resume"); break;
		case STOP: lswlogs(buf, "stop"); break;
		}
		lswlogs(buf, " processing");
		if (st != NULL) {
			lswlogf(buf, " state #%lu", st->st_serialno);
			c = st->st_connection;
		}
		if (c != NULL) {
			char b1[CONN_INST_BUF];
			lswlogf(buf, " connection \"%s\"%s",
				c->name, fmt_conn_instance(c, b1));
		}
		if (st != NULL && (c == NULL || !(c->policy & POLICY_OPPORTUNISTIC))) {
			/* fmt_conn_instance() include the same if POLICY_OPPORTUNISTIC */
			lswlogf(buf, " ");
			lswlog_ip(buf, &st->st_remoteaddr);
		}
		lswlog_source_line(buf, func, file, line);
		if (!current) {
			lswlogs(buf, ")");
		}
	}
}

struct connection *log_push_connection(struct connection *c, const char *func,
				       const char *file, long line)
{
	bool current = (cur_state != NULL); /* not hidden? */
	if (cur_connection != NULL) {
		log_processing(SUSPEND, current,
			       NULL, cur_connection,
			       func, file, line);
	}
	struct connection *old_connection = cur_connection;
	cur_connection = c;
	update_debugging();
	if (cur_connection == NULL) {
		LSWDBGP(DBG_MASK, buf) {
			lswlogf(buf, "trying to processing NULL connection");
			lswlog_source_line(buf, func, file, line);
		}
	} else if (cur_connection == old_connection) {
		log_processing(STILL, current,
			       NULL, cur_connection,
			       func, file, line);
	} else {
		log_processing(START, current,
			       NULL, cur_connection,
			       func, file, line);
	}
	if (cur_state != NULL) {
		log_processing(STILL, true /* must be current */,
			       cur_state, NULL,
			       func, file, line);
	}
	return old_connection;
}

void log_pop_connection(struct connection *c, const char *func,
			const char *file, long line)
{
	if (cur_connection != NULL) {
		log_processing(STOP, cur_state == NULL /* current? */,
			       NULL, cur_connection,
			       func, file, line);
	}
	cur_connection = c;
	update_debugging();
	if (cur_connection != NULL) {
		log_processing(RESUME, cur_state == NULL /* current? */,
			       NULL, cur_connection,
			       func, file, line);
	}
	if (cur_state != NULL) {
		log_processing(STILL, true /* must be current */,
			       cur_state, NULL,
			       func, file, line);
	}
}

so_serial_t log_push_state(struct state *st, const char *func,
			   const char *file, long line)
{
	if (cur_state != NULL) {
		log_processing(SUSPEND, true /* must be current */,
			       cur_state, NULL,
			       func, file, line);
	} else if (cur_connection != NULL) {
		log_processing(SUSPEND, true /* must be current */,
			       NULL, cur_connection,
			       func, file, line);
	}
	so_serial_t old_serialno = (cur_state != NULL ? cur_state->st_serialno : SOS_NOBODY);
	cur_state = st;
	update_debugging();
	if (cur_state == NULL) {
		LSWDBGP(DBG_MASK, buf) {
			lswlogf(buf, "trying to processing NULL state #0");
			lswlog_source_line(buf, func, file, line);
		}
	} else if (cur_state->st_serialno == old_serialno) {
		log_processing(STILL, true /* must be current */,
			       cur_state, NULL,
			       func, file, line);
	} else {
		log_processing(START, true /* must be current */,
			       cur_state, NULL,
			       func, file, line);
	}
	return old_serialno;
}

void log_pop_state(so_serial_t serialno, const char *func,
		   const char *file, long line)
{
	if (cur_state != NULL) {
		log_processing(STOP, true, /* must be current */
			       cur_state, NULL,
			       func, file, line);
	} else {
		LSWDBGP(DBG_MASK, buf) {
			lswlogf(buf, "stop processing NULL state #0");
			lswlog_source_line(buf, func, file, line);
		}
	}
	cur_state = state_by_serialno(serialno);
	update_debugging();
	if (cur_state != NULL) {
		log_processing(RESUME, true, /* must be current */
			       cur_state, NULL,
			       func, file, line);
	} else if (cur_connection != NULL) {
		log_processing(RESUME, true, /* now current */
			       NULL, cur_connection,
			       func, file, line);
	}
}

/*
 * Initialization.
 */

void pluto_init_log(void)
{
	set_alloc_exit_log_func(exit_log);
	if (log_to_stderr)
		setbuf(stderr, NULL);

	if (pluto_log_file != NULL) {
		pluto_log_fp = fopen(pluto_log_file,
			log_append ? "a" : "w");
		if (pluto_log_fp == NULL) {
			fprintf(stderr,
				"Cannot open logfile '%s': %s\n",
				pluto_log_file, strerror(errno));
		} else {
			/*
			 * buffer by line:
			 * should be faster that no buffering
			 * and yet safe since each message is probably a line.
			 */
			setvbuf(pluto_log_fp, NULL, _IOLBF, 0);
		}
	}

	if (log_to_syslog)
		openlog("pluto", LOG_CONS | LOG_NDELAY | LOG_PID,
			LOG_AUTHPRIV);

	peerlog_init();
}

/*
 * Add just the WHACK or STATE (or connection) prefix.
 *
 * Callers need to pick and choose.  For instance, WHACK output some
 * times suppress the whack prefix; and there is no point adding the
 * STATE prefix when it was added earlier.
 */

static void add_whack_rc_prefix(struct lswlog *buf, enum rc_type rc)
{
	lswlogf(buf, "%03d ", rc);
}

/*
 * Wrap up the logic to decide if a particular output should occure.
 * The compiler will likely inline these.
 */

static void stdlog_raw(char *b)
{
	if (log_to_stderr || pluto_log_fp != NULL) {
		FILE *out = log_to_stderr ? stderr : pluto_log_fp;

		if (log_with_timestamp) {
			char now[34] = "";
			struct realtm t = local_realtime(realnow());
			strftime(now, sizeof(now), "%b %e %T", &t.tm);
			fprintf(out, "%s.%06ld: %s\n", now, t.microsec, b);
		} else {
			fprintf(out, "%s\n", b);
		}
	}
}

static void syslog_raw(int severity, char *b)
{
	if (log_to_syslog)
		syslog(severity, "%s", b);
}

static void peerlog_raw(char *b)
{
	if (log_to_perpeer) {
		peerlog(cur_connection, b);
	}
}

static void whack_raw(struct lswlog *b, enum rc_type rc)
{
	/*
	 * Only whack-log when the main thread.
	 *
	 * Helper threads, which are asynchronous, shouldn't be trying
	 * to directly emit whack output.
	 */
	if (pthread_equal(pthread_self(), main_thread)) {
		if (whack_log_p()) {
			/*
			 * On the assumption that logging to whack is
			 * rare and slow anyway, don't try to tune
			 * this code path.
			 */
			LSWBUF(buf) {
				add_whack_rc_prefix(buf, rc);
				/* add_state_prefix() - done by caller */
				lswlogl(buf, b);
				lswlog_to_whack_stream(buf);
			}
		}
	}
}

static void lswlog_cur_prefix(struct lswlog *buf,
			      struct state *cur_state,
			      struct connection *cur_connection,
			      const ip_address *cur_from, u_int16_t cur_from_port)
{
	if (!pthread_equal(pthread_self(), main_thread)) {
		return;
	}

	struct connection *c = cur_state != NULL ? cur_state->st_connection :
		cur_connection;

	if (c != NULL) {
		lswlogf(buf, "\"%s\"", c->name);
		/* if it fits, put in any connection instance information */
		char inst[CONN_INST_BUF];
		fmt_conn_instance(c, inst);
		lswlogs(buf, inst);
		if (cur_state != NULL) {
			/* state number */
			lswlogf(buf, " #%lu", cur_state->st_serialno);
			/* state name */
			if (DBGP(DBG_ADD_PREFIX)) {
				lswlogf(buf, " ");
				lswlog_enum_short(buf, &state_names,
						  cur_state->st_state);
			}
		}
		lswlogs(buf, ": ");
	} else if (cur_from != NULL) {
		/* peer's IP address */
		ipstr_buf b;
		lswlogf(buf, "packet from %s:%u: ",
			sensitive_ipstr(cur_from, &b),
			(unsigned)cur_from_port);
	}
}

void lswlog_log_prefix(struct lswlog *buf)
{
	lswlog_cur_prefix(buf, cur_state, cur_connection,
			  cur_from, cur_from_port);
}

/*
 * This needs to mimic both lswlog_log_prefix() and
 * lswlog_dbg_prefix().
 */

void log_prefix(struct lswlog *buf, bool debug,
		struct state *st, struct connection *c)
{
	if (debug) {
		lswlogs(buf, DEBUG_PREFIX);
	}
	if (!debug || DBGP(DBG_ADD_PREFIX)) {
		lswlog_cur_prefix(buf, st, c, cur_from, cur_from_port);
	}
}

bool log_debugging(struct state *st, struct connection *c,
		   lset_t debug)
{
	if (st != NULL) {
		c = st->st_connection;
	}
	if (c == NULL) {
		return base_debugging & debug;
	} else {
		lset_t debugging = lmod(base_debugging,
					st->st_connection->extra_debugging);
		return debugging & debug;
	}
}

static void log_raw(struct lswlog *buf, int severity)
{
	stdlog_raw(buf->array);
	syslog_raw(severity, buf->array);
	peerlog_raw(buf->array);
	/* not whack */
}

void lswlog_to_debug_stream(struct lswlog *buf)
{
	sanitize_string(buf->array, buf->roof); /* needed? */
	log_raw(buf, LOG_DEBUG);
	/* not whack */
}

void lswlog_to_error_stream(struct lswlog *buf)
{
	log_raw(buf, LOG_ERR);
	whack_raw(buf, RC_LOG_SERIOUS);
}

void lswlog_to_log_stream(struct lswlog *buf)
{
	log_raw(buf, LOG_WARNING);
	/* not whack */
}

void lswlog_to_log_whack_stream(struct lswlog *buf, enum rc_type rc)
{
	log_raw(buf, LOG_WARNING);
	whack_raw(buf, rc);
}

void close_log(void)
{
	if (log_to_syslog)
		closelog();

	if (pluto_log_fp != NULL) {
		(void)fclose(pluto_log_fp);
		pluto_log_fp = NULL;
	}

	peerlog_close();
}

void libreswan_log_errno(int e, const char *prefix, const char *message, ...)
{
	LSWBUF(buf) {
		/* <prefix><state#N...><message>.Errno %d: <strerror> */
		lswlogs(buf, prefix);
		lswlog_log_prefix(buf);
		va_list args;
		va_start(args, message);
		lswlogvf(buf, message, args);
		va_end(args);
		lswlogs(buf, ".");
		lswlog_errno(buf, e);
		lswlog_to_error_stream(buf);
	}
}

void exit_log(const char *message, ...)
{
	LSWBUF(buf) {
		/* FATAL ERROR: <state...><message> */
		lswlogs(buf, "FATAL ERROR: ");
		lswlog_log_prefix(buf);
		va_list args;
		va_start(args, message);
		lswlogvf(buf, message, args);
		va_end(args);
		lswlog_to_error_stream(buf);
	}
	exit_pluto(PLUTO_EXIT_FAIL);
}

void libreswan_exit(enum rc_type rc)
{
	exit_pluto(rc);
}

void whack_log_pre(enum rc_type rc, struct lswlog *buf)
{
	passert(pthread_equal(pthread_self(), main_thread));
	add_whack_rc_prefix(buf, rc);
	lswlog_log_prefix(buf);
}

void lswlog_to_whack_stream(struct lswlog *buf)
{
	passert(pthread_equal(pthread_self(), main_thread));

	int wfd = whack_log_fd != NULL_FD ? whack_log_fd :
	      cur_state != NULL ? cur_state->st_whack_sock :
	      NULL_FD;

	passert(wfd != NULL_FD);

	char *m = buf->array;
	size_t len = buf->len;

	/* write to whack socket, but suppress possible SIGPIPE */
#ifdef MSG_NOSIGNAL                     /* depends on version of glibc??? */
	m[len] = '\n';  /* don't need NUL, do need NL */
	(void) send(wfd, m, len + 1, MSG_NOSIGNAL);
#else /* !MSG_NOSIGNAL */
	int r;
	struct sigaction act, oldact;

	m[len] = '\n'; /* don't need NUL, do need NL */
	act.sa_handler = SIG_IGN;
	sigemptyset(&act.sa_mask);
	act.sa_flags = 0; /* no nothing */
	r = sigaction(SIGPIPE, &act, &oldact);
	passert(r == 0);

	(void) write(wfd, m, len + 1);

	r = sigaction(SIGPIPE, &oldact, NULL);
	passert(r == 0);
#endif /* !MSG_NOSIGNAL */
}

bool whack_log_p(void)
{
	if (!pthread_equal(pthread_self(), main_thread)) {
		PEXPECT_LOG("%s", "whack_log*() must be called from the main thread");
		return false;
	}

	int wfd = whack_log_fd != NULL_FD ? whack_log_fd :
	      cur_state != NULL ? cur_state->st_whack_sock :
	      NULL_FD;

	return wfd != NULL_FD;
}

/* emit message to whack.
 * form is "ddd statename text" where
 * - ddd is a decimal status code (RC_*) as described in whack.h
 * - text is a human-readable annotation
 */

void whack_log(enum rc_type rc, const char *message, ...)
{
	if (whack_log_p()) {
		LSWBUF(buf) {
			add_whack_rc_prefix(buf, rc);
			lswlog_log_prefix(buf);
			va_list args;
			va_start(args, message);
			lswlogvf(buf, message, args);
			va_end(args);
			lswlog_to_whack_stream(buf);
		}
	}
}

void whack_log_comment(const char *message, ...)
{
	if (whack_log_p()) {
		LSWBUF(buf) {
			/* add_whack_rc_prefix() - skipped */
			lswlog_log_prefix(buf);
			va_list args;
			va_start(args, message);
			lswlogvf(buf, message, args);
			va_end(args);
			lswlog_to_whack_stream(buf);
		}
	}
}

lset_t base_debugging = DBG_NONE; /* default to reporting nothing */

void set_debugging(lset_t deb)
{
	cur_debugging = deb;

	if (kernel_ops != NULL && kernel_ops->set_debug != NULL)
		(*kernel_ops->set_debug)(cur_debugging, DBG_log,
					 libreswan_log);
}

void reset_debugging(void)
{
	set_debugging(base_debugging);
}
