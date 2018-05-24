/*
 * Copyright (C) 2011 Philippe Gerum <rpm@xenomai.org>.
 * Copyright (C) 2008, 2009 Jan Kiszka <jan.kiszka@siemens.com>.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.

 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA.
 *
 * --
 * Internal Cobalt services. No sanity check will be done with
 * respect to object validity, callers have to take care of this.
 */
#include <sys/types.h>
#include <stddef.h>
#include <stdlib.h>
#include <limits.h>
#include <stdio.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <stdarg.h>
#include <pthread.h>
#include <asm/xenomai/syscall.h>
#include <cobalt/sys/cobalt.h>
#include "internal.h"

int cobalt_extend(unsigned int magic)
{
	return XENOMAI_SYSCALL1(sc_cobalt_extend, magic);
}

int cobalt_corectl(int request, void *buf, size_t bufsz)
{
	return XENOMAI_SYSCALL3(sc_cobalt_corectl, request, buf, bufsz);
}

void cobalt_thread_harden(void)
{
	int status = cobalt_get_current_mode();

	/* non-RT shadows are NOT allowed to force primary mode. */
	if ((status & (XNRELAX|XNWEAK)) == XNRELAX)
		XENOMAI_SYSCALL1(sc_cobalt_migrate, COBALT_PRIMARY);
}

void cobalt_thread_relax(void)
{
	if (!cobalt_is_relaxed())
		XENOMAI_SYSCALL1(sc_cobalt_migrate, COBALT_SECONDARY);
}

int cobalt_thread_stat(pid_t pid, struct cobalt_threadstat *stat)
{
	return XENOMAI_SYSCALL2(sc_cobalt_thread_getstat, pid, stat);
}

pid_t cobalt_thread_pid(pthread_t thread)
{
	return XENOMAI_SYSCALL1(sc_cobalt_thread_getpid, thread);
}

int cobalt_thread_mode(void)
{
	return cobalt_get_current_mode();
}

int cobalt_thread_join(pthread_t thread)
{
	int ret, oldtype;

	/*
	 * Serialize with the regular task exit path, so that no call
	 * for the joined pthread may succeed after this routine
	 * returns. A successful call to sc_cobalt_thread_join
	 * receives -EIDRM, meaning that we eventually joined the
	 * exiting thread as seen by the Cobalt core.
	 *
	 * -ESRCH means that the joined thread has already exited
	 * linux-wise, while we were about to wait for it from the
	 * Cobalt side, in which case we are fine.
	 *
	 * -EBUSY denotes a multiple join for several threads in
	 * parallel to the same target.
	 *
	 * -EPERM may be received because the caller is not a
	 * Cobalt thread.
	 *
	 * -EINVAL is received in case the target is not a joinable
	 * thread (i.e. detached).
	 *
	 * Zero is unexpected.
	 *
	 * CAUTION: this service joins a thread Cobat-wise only, not
	 * glibc-wise.  For a complete join comprising the libc
	 * cleanups, __STD(pthread_join()) should be paired with this
	 * call.
	 */
	pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, &oldtype);

	do
		ret = XENOMAI_SYSCALL1(sc_cobalt_thread_join, thread);
	while (ret == -EINTR);

	pthread_setcanceltype(oldtype, NULL);

	return ret;
}

int cobalt_thread_probe(pid_t pid)
{
	return XENOMAI_SYSCALL2(sc_cobalt_kill, pid, 0);
}

void __cobalt_commit_memory(void *p, size_t len)
{
	volatile char *_p = (volatile char *)p, *end;
	long pagesz = sysconf(_SC_PAGESIZE);

	end = _p + len;
	do {
		*_p = *_p;
		_p += pagesz;
	} while (_p < end);
}

int cobalt_serial_debug(const char *fmt, ...)
{
	char msg[128];
	va_list ap;
	int n, ret;

	/*
	 * The serial debug output handler disables hw IRQs while
	 * writing to the UART console port, so the message ought to
	 * be reasonably short.
	 */
	va_start(ap, fmt);
	n = vsnprintf(msg, sizeof(msg), fmt, ap);
	ret = XENOMAI_SYSCALL2(sc_cobalt_serialdbg, msg, n);
	va_end(ap);

	return ret;
}

static inline
struct cobalt_monitor_state *get_monitor_state(cobalt_monitor_t *mon)
{
	return mon->flags & COBALT_MONITOR_SHARED ?
		cobalt_umm_shared + mon->state_offset :
		cobalt_umm_private + mon->state_offset;
}

int cobalt_monitor_init(cobalt_monitor_t *mon, clockid_t clk_id, int flags)
{
	struct cobalt_monitor_state *state;
	int ret;

	ret = XENOMAI_SYSCALL3(sc_cobalt_monitor_init,
			       mon, clk_id, flags);
	if (ret)
		return ret;

	state = get_monitor_state(mon);
	cobalt_commit_memory(state);

	return 0;
}

int cobalt_monitor_destroy(cobalt_monitor_t *mon)
{
	return XENOMAI_SYSCALL1(sc_cobalt_monitor_destroy, mon);
}

int cobalt_monitor_enter(cobalt_monitor_t *mon)
{
	struct cobalt_monitor_state *state;
	int status, ret, oldtype;
	xnhandle_t cur;

	/*
	 * Assumptions on entry:
	 *
	 * - this is a Cobalt thread (caller checked this).
	 * - no recursive entry/locking.
	 */

	status = cobalt_get_current_mode();
	if (status & (XNRELAX|XNWEAK|XNDEBUG))
		goto syscall;

	state = get_monitor_state(mon);
	cur = cobalt_get_current();
	ret = xnsynch_fast_acquire(&state->owner, cur);
	if (ret == 0) {
		state->flags &= ~(COBALT_MONITOR_SIGNALED|COBALT_MONITOR_BROADCAST);
		return 0;
	}
syscall:
	pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, &oldtype);

	/*
	 * Jump to kernel to wait for entry. We redo in case of
	 * interrupt.
	 */
	do
		ret = XENOMAI_SYSCALL1(sc_cobalt_monitor_enter,	mon);
	while (ret == -EINTR);

	pthread_setcanceltype(oldtype, NULL);

	return ret;
}

int cobalt_monitor_exit(cobalt_monitor_t *mon)
{
	struct cobalt_monitor_state *state;
	int status, ret;
	xnhandle_t cur;

	__sync_synchronize();

	state = get_monitor_state(mon);
	if ((state->flags & COBALT_MONITOR_PENDED) &&
	    (state->flags & COBALT_MONITOR_SIGNALED))
		goto syscall;

	status = cobalt_get_current_mode();
	if (status & (XNWEAK|XNDEBUG))
		goto syscall;

	cur = cobalt_get_current();
	if (xnsynch_fast_release(&state->owner, cur))
		return 0;
syscall:
	do
		ret = XENOMAI_SYSCALL1(sc_cobalt_monitor_exit, mon);
	while (ret == -EINTR);

	return ret;
}

int cobalt_monitor_wait(cobalt_monitor_t *mon, int event,
			const struct timespec *ts)
{
	int ret, opret, oldtype;

	pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, &oldtype);

	ret = XENOMAI_SYSCALL4(sc_cobalt_monitor_wait,
			       mon, event, ts, &opret);

	pthread_setcanceltype(oldtype, NULL);

	/*
	 * If we got interrupted while trying to re-enter the monitor,
	 * we need to redo. In the meantime, any pending linux signal
	 * has been processed.
	 */
	if (ret == -EINTR)
		ret = cobalt_monitor_enter(mon);

	return ret ?: opret;
}

void cobalt_monitor_grant(cobalt_monitor_t *mon,
			  struct xnthread_user_window *u_window)
{
	struct cobalt_monitor_state *state = get_monitor_state(mon);

	state->flags |= COBALT_MONITOR_GRANTED;
	u_window->grant_value = 1;
}

int cobalt_monitor_grant_sync(cobalt_monitor_t *mon,
			  struct xnthread_user_window *u_window)
{
	struct cobalt_monitor_state *state = get_monitor_state(mon);
	int ret, oldtype;

	cobalt_monitor_grant(mon, u_window);

	if ((state->flags & COBALT_MONITOR_PENDED) == 0)
		return 0;

	pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, &oldtype);

	ret = XENOMAI_SYSCALL1(sc_cobalt_monitor_sync, mon);

	pthread_setcanceltype(oldtype, NULL);

	if (ret == -EINTR)
		return cobalt_monitor_enter(mon);

	return ret;
}

void cobalt_monitor_grant_all(cobalt_monitor_t *mon)
{
	struct cobalt_monitor_state *state = get_monitor_state(mon);

	state->flags |= COBALT_MONITOR_GRANTED|COBALT_MONITOR_BROADCAST;
}

int cobalt_monitor_grant_all_sync(cobalt_monitor_t *mon)
{
	struct cobalt_monitor_state *state = get_monitor_state(mon);
	int ret, oldtype;

	cobalt_monitor_grant_all(mon);

	if ((state->flags & COBALT_MONITOR_PENDED) == 0)
		return 0;

	pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, &oldtype);

	ret = XENOMAI_SYSCALL1(sc_cobalt_monitor_sync, mon);

	pthread_setcanceltype(oldtype, NULL);

	if (ret == -EINTR)
		return cobalt_monitor_enter(mon);

	return ret;
}

void cobalt_monitor_drain(cobalt_monitor_t *mon)
{
	struct cobalt_monitor_state *state = get_monitor_state(mon);

	state->flags |= COBALT_MONITOR_DRAINED;
}

int cobalt_monitor_drain_sync(cobalt_monitor_t *mon)
{
	struct cobalt_monitor_state *state = get_monitor_state(mon);
	int ret, oldtype;

	cobalt_monitor_drain(mon);

	if ((state->flags & COBALT_MONITOR_PENDED) == 0)
		return 0;

	pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, &oldtype);

	ret = XENOMAI_SYSCALL1(sc_cobalt_monitor_sync, mon);

	pthread_setcanceltype(oldtype, NULL);

	if (ret == -EINTR)
		return cobalt_monitor_enter(mon);

	return ret;
}

void cobalt_monitor_drain_all(cobalt_monitor_t *mon)
{
	struct cobalt_monitor_state *state = get_monitor_state(mon);

	state->flags |= COBALT_MONITOR_DRAINED|COBALT_MONITOR_BROADCAST;
}

int cobalt_monitor_drain_all_sync(cobalt_monitor_t *mon)
{
	struct cobalt_monitor_state *state = get_monitor_state(mon);
	int ret, oldtype;

	cobalt_monitor_drain_all(mon);

	if ((state->flags & COBALT_MONITOR_PENDED) == 0)
		return 0;

	pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, &oldtype);

	ret = XENOMAI_SYSCALL1(sc_cobalt_monitor_sync, mon);

	pthread_setcanceltype(oldtype, NULL);

	if (ret == -EINTR)
		return cobalt_monitor_enter(mon);

	return ret;
}

#define __raw_write_out(__msg)					\
	do {							\
		int __ret;					\
		__ret = write(1, __msg , sizeof(__msg) - 1);	\
		(void)__ret;					\
	} while (0)

#define raw_write_out(__msg)	__raw_write_out("Xenomai/cobalt: " __msg "\n")

void cobalt_sigdebug_handler(int sig, siginfo_t *si, void *context)
{
	if (!sigdebug_marked(si))
		goto forward;

	switch (sigdebug_reason(si)) {
	case SIGDEBUG_NOMLOCK:
		raw_write_out("process memory not locked");
		_exit(4);
	case SIGDEBUG_RESCNT_IMBALANCE:
		raw_write_out("resource locking imbalance");
		break;
	case SIGDEBUG_MUTEX_SLEEP:
		raw_write_out("sleeping while holding mutex");
		break;
	case SIGDEBUG_WATCHDOG:
		raw_write_out("watchdog triggered");
		break;
	}

forward:
	sigaction(SIGDEBUG, &__cobalt_orig_sigdebug, NULL);
	pthread_kill(pthread_self(), SIGDEBUG);
}

static inline
struct cobalt_event_state *get_event_state(cobalt_event_t *event)
{
	return event->flags & COBALT_EVENT_SHARED ?
		cobalt_umm_shared + event->state_offset :
		cobalt_umm_private + event->state_offset;
}

int cobalt_event_init(cobalt_event_t *event, unsigned int value,
		      int flags)
{
	struct cobalt_event_state *state;
	int ret;

	ret = XENOMAI_SYSCALL3(sc_cobalt_event_init, event, value, flags);
	if (ret)
		return ret;

	state = get_event_state(event);
	cobalt_commit_memory(state);

	return 0;
}

int cobalt_event_destroy(cobalt_event_t *event)
{
	return XENOMAI_SYSCALL1(sc_cobalt_event_destroy, event);
}

int cobalt_event_post(cobalt_event_t *event, unsigned int bits)
{
	struct cobalt_event_state *state = get_event_state(event);

	if (bits == 0)
		return 0;

	__sync_or_and_fetch(&state->value, bits); /* full barrier. */

	if ((state->flags & COBALT_EVENT_PENDED) == 0)
		return 0;

	return XENOMAI_SYSCALL1(sc_cobalt_event_sync, event);
}

int cobalt_event_wait(cobalt_event_t *event,
		      unsigned int bits, unsigned int *bits_r,
		      int mode, const struct timespec *timeout)
{
	int ret, oldtype;

	pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, &oldtype);

	ret = XENOMAI_SYSCALL5(sc_cobalt_event_wait,
			       event, bits, bits_r, mode, timeout);

	pthread_setcanceltype(oldtype, NULL);

	return ret;
}

unsigned long cobalt_event_clear(cobalt_event_t *event,
				 unsigned int bits)
{
	struct cobalt_event_state *state = get_event_state(event);

	return __sync_fetch_and_and(&state->value, ~bits);
}

int cobalt_event_inquire(cobalt_event_t *event,
			 struct cobalt_event_info *info,
			 pid_t *waitlist, size_t waitsz)
{
	return XENOMAI_SYSCALL4(sc_cobalt_event_inquire, event,
				info, waitlist, waitsz);
}

int cobalt_sem_inquire(sem_t *sem, struct cobalt_sem_info *info,
		       pid_t *waitlist, size_t waitsz)
{
	struct cobalt_sem_shadow *_sem = &((union cobalt_sem_union *)sem)->shadow_sem;
	
	return XENOMAI_SYSCALL4(sc_cobalt_sem_inquire, _sem,
				info, waitlist, waitsz);
}

int cobalt_sched_weighted_prio(int policy,
			       const struct sched_param_ex *param_ex)
{
	return XENOMAI_SYSCALL2(sc_cobalt_sched_weightprio, policy, param_ex);
}


/*
 * Temporary compatibility aliases which should be phased out at next
 * API revision.
 */
void assert_nrt(void)
__attribute__((alias("cobalt_assert_nrt")));
void assert_nrt_fast(void)
__attribute__((alias("cobalt_assert_nrt")));

void cobalt_assert_nrt(void)
{
	if (cobalt_should_warn())
		pthread_kill(pthread_self(), SIGDEBUG);
}
