/*
 * Copyright (C) 2005 Philippe Gerum <rpm@xenomai.org>.
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
 */
#include <errno.h>
#include <string.h>
#include <limits.h>
#include <pthread.h>
#include <asm/xenomai/syscall.h>
#include "current.h"
#include "internal.h"

/**
 * @ingroup cobalt_api
 * @defgroup cobalt_api_mutex Mutual exclusion
 *
 * Cobalt/POSIX mutual exclusion services
 *
 * A mutex is a MUTual EXclusion device, and is useful for protecting
 * shared data structures from concurrent modifications, and implementing
 * critical sections and monitors.
 *
 * A mutex has two possible states: unlocked (not owned by any thread), and
 * locked (owned by one thread). A mutex can never be owned by two different
 * threads simultaneously. A thread attempting to lock a mutex that is already
 * locked by another thread is suspended until the owning thread unlocks the
 * mutex first.
 *
 * Before it can be used, a mutex has to be initialized with
 * pthread_mutex_init(). An attribute object, which reference may be passed to
 * this service, allows to select the features of the created mutex, namely its
 * @a type (see pthread_mutexattr_settype()), the priority @a protocol it
 * uses (see pthread_mutexattr_setprotocol()) and whether it may be shared
 * between several processes (see pthread_mutexattr_setpshared()).
 *
 * By default, Cobalt mutexes are of the normal type, use no
 * priority protocol and may not be shared between several processes.
 *
 * Note that only pthread_mutex_init() may be used to initialize a mutex, using
 * the static initializer @a PTHREAD_MUTEX_INITIALIZER is not supported.
 *
 *@{
 */

static pthread_mutexattr_t cobalt_default_mutexattr;
static union cobalt_mutex_union cobalt_autoinit_mutex_union;
static pthread_mutex_t *const cobalt_autoinit_mutex =
	&cobalt_autoinit_mutex_union.native_mutex;

void cobalt_mutex_init(void)
{
	struct cobalt_mutex_shadow *_mutex =
		&cobalt_autoinit_mutex_union.shadow_mutex;
	pthread_mutexattr_t rt_init_mattr;
	int err __attribute__((unused));

	pthread_mutexattr_init(&cobalt_default_mutexattr);

	pthread_mutexattr_init(&rt_init_mattr);
	pthread_mutexattr_setprotocol(&rt_init_mattr, PTHREAD_PRIO_INHERIT);
	_mutex->magic = ~COBALT_MUTEX_MAGIC;
	err = __COBALT(pthread_mutex_init(cobalt_autoinit_mutex,
						&rt_init_mattr));
	assert(err == 0);
	pthread_mutexattr_destroy(&rt_init_mattr);
}

/**
 * Initialize a mutex.
 *
 * This services initializes the mutex @a mx, using the mutex attributes object
 * @a attr. If @a attr is @a NULL, default attributes are used (see
 * pthread_mutexattr_init()).
 *
 * @param mutex the mutex to be initialized;
 *
 * @param attr the mutex attributes object.
 *
 * @return 0 on success,
 * @return an error number if:
 * - EINVAL, the mutex attributes object @a attr is invalid or uninitialized;
 * - EBUSY, the mutex @a mx was already initialized;
 * - ENOMEM, insufficient memory available from the system heap to initialize the
 *   mutex, increase CONFIG_XENO_OPT_SYS_HEAPSZ.
 * - EAGAIN, insufficient memory available to initialize the
 *   mutex, increase CONFIG_XENO_OPT_SHARED_HEAPSZ for a process-shared
 *   mutex, or CONFIG_XENO_OPT_PRIVATE_HEAPSZ for a process-private mutex.
 * - EAGAIN, no registry slot available, check/raise CONFIG_XENO_OPT_REGISTRY_NRSLOTS.
 *
 * @see
 * <a href="http://www.opengroup.org/onlinepubs/000095399/functions/pthread_mutex_init.html">
 * Specification.</a>
 *
 * @apitags{thread-unrestricted}
 */
COBALT_IMPL(int, pthread_mutex_init, (pthread_mutex_t *mutex,
				      const pthread_mutexattr_t *attr))
{
	struct cobalt_mutex_shadow *_mutex =
		&((union cobalt_mutex_union *)mutex)->shadow_mutex;
	struct cobalt_mutex_state *state;
	struct cobalt_mutexattr kmattr;
	int err, tmp;

	if (_mutex->magic == COBALT_MUTEX_MAGIC) {
		err = -XENOMAI_SYSCALL1(sc_cobalt_mutex_check_init, _mutex);

		if (err)
			return err;
	}

	if (attr == NULL)
		attr = &cobalt_default_mutexattr;

	err = pthread_mutexattr_getpshared(attr, &tmp);
	if (err)
		return err;
	kmattr.pshared = tmp;

	err = pthread_mutexattr_gettype(attr, &tmp);
	if (err)
		return err;
	kmattr.type = tmp;

	err = pthread_mutexattr_getprotocol(attr, &tmp);
	if (err)
		return err;
	if (tmp == PTHREAD_PRIO_PROTECT) { /* Prio ceiling unsupported */
		err = EINVAL;
		return err;
	}
	kmattr.protocol = tmp;

	err = -XENOMAI_SYSCALL2(sc_cobalt_mutex_init, _mutex, &kmattr);
	if (err)
		return err;

	state = mutex_get_state(_mutex);
	cobalt_commit_memory(state);

	return err;
}

/**
 * Destroy a mutex.
 *
 * This service destroys the mutex @a mx, if it is unlocked and not referenced
 * by any condition variable. The mutex becomes invalid for all mutex services
 * (they all return the EINVAL error) except pthread_mutex_init().
 *
 * @param mutex the mutex to be destroyed.
 *
 * @return 0 on success,
 * @return an error number if:
 * - EINVAL, the mutex @a mx is invalid;
 * - EPERM, the mutex is not process-shared and does not belong to the current
 *   process;
 * - EBUSY, the mutex is locked, or used by a condition variable.
 *
 * @see
 * <a href="http://www.opengroup.org/onlinepubs/000095399/functions/pthread_mutex_destroy.html">
 * Specification.</a>
 *
 * @apitags{thread-unrestricted}
 */
COBALT_IMPL(int, pthread_mutex_destroy, (pthread_mutex_t *mutex))
{
	struct cobalt_mutex_shadow *_mutex =
		&((union cobalt_mutex_union *)mutex)->shadow_mutex;
	int err;

	if (_mutex->magic != COBALT_MUTEX_MAGIC)
		return EINVAL;

	err = XENOMAI_SYSCALL1(sc_cobalt_mutex_destroy, _mutex);

	return -err;
}

static int __attribute__((cold)) cobalt_mutex_autoinit(pthread_mutex_t *mutex)
{
	static pthread_mutex_t uninit_normal_mutex =
		PTHREAD_MUTEX_INITIALIZER;
#if HAVE_DECL_PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP
	static pthread_mutex_t uninit_recursive_mutex =
		PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP;
#endif
#if HAVE_DECL_PTHREAD_ERRORCHECK_MUTEX_INITIALIZER_NP
	static pthread_mutex_t uninit_errorcheck_mutex =
		PTHREAD_ERRORCHECK_MUTEX_INITIALIZER_NP;
#endif
	struct cobalt_mutex_shadow *_mutex =
		&((union cobalt_mutex_union *)mutex)->shadow_mutex;
	int err __attribute__((unused));
	pthread_mutexattr_t mattr;
	int ret = 0, type;

	if (memcmp(mutex, &uninit_normal_mutex, sizeof(*mutex)) == 0)
		type = PTHREAD_MUTEX_DEFAULT;
#if HAVE_DECL_PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP
	else if (memcmp(mutex, &uninit_recursive_mutex, sizeof(*mutex)) == 0)
		type = PTHREAD_MUTEX_RECURSIVE_NP;
#endif
#if HAVE_DECL_PTHREAD_ERRORCHECK_MUTEX_INITIALIZER_NP
	else if (memcmp(mutex, &uninit_errorcheck_mutex, sizeof(*mutex)) == 0)
		type = PTHREAD_MUTEX_ERRORCHECK_NP;
#endif
	else
		return EINVAL;

	pthread_mutexattr_init(&mattr);
	pthread_mutexattr_settype(&mattr, type);
	err = __COBALT(pthread_mutex_lock(cobalt_autoinit_mutex));
	if (err) {
		ret = err;
		goto out;
	}
	if (_mutex->magic != COBALT_MUTEX_MAGIC)
		ret = __COBALT(pthread_mutex_init(mutex, &mattr));
	err = __COBALT(pthread_mutex_unlock(cobalt_autoinit_mutex));
	if (err) {
		if (ret == 0)
			ret = err;
	}

  out:
	pthread_mutexattr_destroy(&mattr);

	return ret;
}

/**
 * Lock a mutex.
 *
 * This service attempts to lock the mutex @a mx. If the mutex is free, it
 * becomes locked. If it was locked by another thread than the current one, the
 * current thread is suspended until the mutex is unlocked. If it was already
 * locked by the current mutex, the behaviour of this service depends on the
 * mutex type :
 * - for mutexes of the @a PTHREAD_MUTEX_NORMAL type, this service deadlocks;
 * - for mutexes of the @a PTHREAD_MUTEX_ERRORCHECK type, this service returns
 *   the EDEADLK error number;
 * - for mutexes of the @a PTHREAD_MUTEX_RECURSIVE type, this service increments
 *   the lock recursion count and returns 0.
 *
 * @param mutex the mutex to be locked.
 *
 * @return 0 on success
 * @return an error number if:
 * - EPERM, the caller context is invalid;
 * - EINVAL, the mutex @a mx is invalid;
 * - EPERM, the mutex is not process-shared and does not belong to the current
 *   process;
 * - EDEADLK, the mutex is of the @a PTHREAD_MUTEX_ERRORCHECK type and was
 *   already locked by the current thread;
 * - EAGAIN, the mutex is of the @a PTHREAD_MUTEX_RECURSIVE type and the maximum
 *   number of recursive locks has been exceeded.
 *
 * @see
 * <a href="http://www.opengroup.org/onlinepubs/000095399/functions/pthread_mutex_lock.html">
 * Specification.</a>
 *
 * @apitags{xthread-only, switch-primary}
 */
COBALT_IMPL(int, pthread_mutex_lock, (pthread_mutex_t *mutex))
{
	struct cobalt_mutex_shadow *_mutex =
		&((union cobalt_mutex_union *)mutex)->shadow_mutex;
	int status, ret;
	xnhandle_t cur;

	cur = cobalt_get_current();
	if (cur == XN_NO_HANDLE)
		return EPERM;

	if (_mutex->magic != COBALT_MUTEX_MAGIC)
		goto autoinit;

	/*
	 * We track resource ownership for auto-relax of non real-time
	 * shadows and some debug features, so we must always obtain
	 * them via a syscall.
	 */
  cont:
	status = cobalt_get_current_mode();
	if ((status & (XNRELAX|XNWEAK|XNDEBUG)) == 0) {
		ret = xnsynch_fast_acquire(mutex_get_ownerp(_mutex), cur);
		if (ret == 0) {
			_mutex->lockcnt = 1;
			return 0;
		}
	} else {
		ret = xnsynch_fast_owner_check(mutex_get_ownerp(_mutex), cur);
		if (ret == 0)
			ret = -EBUSY;
	}

	if (ret == -EBUSY)
		switch(_mutex->attr.type) {
		case PTHREAD_MUTEX_NORMAL:
			break;

		case PTHREAD_MUTEX_ERRORCHECK:
			return EDEADLK;

		case PTHREAD_MUTEX_RECURSIVE:
			if (_mutex->lockcnt == UINT32_MAX)
				return EAGAIN;
			++_mutex->lockcnt;
			return 0;
		}

	do
		ret = XENOMAI_SYSCALL1(sc_cobalt_mutex_lock, _mutex);
	while (ret == -EINTR);

	if (ret == 0)
		_mutex->lockcnt = 1;

	return -ret;

  autoinit:
	ret = cobalt_mutex_autoinit(mutex);
	if (ret)
		return ret;
	goto cont;
}

/**
 * Attempt, during a bounded time, to lock a mutex.
 *
 * This service is equivalent to pthread_mutex_lock(), except that if the mutex
 * @a mx is locked by another thread than the current one, this service only
 * suspends the current thread until the timeout specified by @a to expires.
 *
 * @param mutex the mutex to be locked;
 *
 * @param to the timeout, expressed as an absolute value of the CLOCK_REALTIME
 * clock.
 *
 * @return 0 on success;
 * @return an error number if:
 * - EPERM, the caller context is invalid;
 * - EINVAL, the mutex @a mx is invalid;
 * - EPERM, the mutex is not process-shared and does not belong to the current
 *   process;
 * - ETIMEDOUT, the mutex could not be locked and the specified timeout
 *   expired;
 * - EDEADLK, the mutex is of the @a PTHREAD_MUTEX_ERRORCHECK type and the mutex
 *   was already locked by the current thread;
 * - EAGAIN, the mutex is of the @a PTHREAD_MUTEX_RECURSIVE type and the maximum
 *   number of recursive locks has been exceeded.
 *
 * @see
 * <a href="http://www.opengroup.org/onlinepubs/000095399/functions/pthread_mutex_timedlock.html">
 * Specification.</a>
 *
 * @apitags{xthread-only, switch-primary}
 */
COBALT_IMPL(int, pthread_mutex_timedlock, (pthread_mutex_t *mutex,
					   const struct timespec *to))
{
	struct cobalt_mutex_shadow *_mutex =
		&((union cobalt_mutex_union *)mutex)->shadow_mutex;
	int status, ret;
	xnhandle_t cur;

	cur = cobalt_get_current();
	if (cur == XN_NO_HANDLE)
		return EPERM;

	if (_mutex->magic != COBALT_MUTEX_MAGIC)
		goto autoinit;

	/* See __cobalt_pthread_mutex_lock() */
  cont:
	status = cobalt_get_current_mode();
	if ((status & (XNRELAX|XNWEAK|XNDEBUG)) == 0) {
		ret = xnsynch_fast_acquire(mutex_get_ownerp(_mutex), cur);
		if (ret == 0) {
			_mutex->lockcnt = 1;
			return 0;
		}
	} else {
		ret = xnsynch_fast_owner_check(mutex_get_ownerp(_mutex), cur);
		if (ret == 0)
			ret = -EBUSY;
	}

	if (ret == -EBUSY)
		switch(_mutex->attr.type) {
		case PTHREAD_MUTEX_NORMAL:
			break;

		case PTHREAD_MUTEX_ERRORCHECK:
			return EDEADLK;

		case PTHREAD_MUTEX_RECURSIVE:
			if (_mutex->lockcnt == UINT32_MAX)
				return EAGAIN;

			++_mutex->lockcnt;
			return 0;
		}

	do {
		ret = XENOMAI_SYSCALL2(sc_cobalt_mutex_timedlock, _mutex, to);
	} while (ret == -EINTR);

	if (ret == 0)
		_mutex->lockcnt = 1;
	return -ret;

  autoinit:
	ret = cobalt_mutex_autoinit(mutex);
	if (ret)
		return ret;
	goto cont;
}

/**
 * Attempt to lock a mutex.
 *
 * This service is equivalent to pthread_mutex_lock(), except that if the mutex
 * @a mx is locked by another thread than the current one, this service returns
 * immediately.
 *
 * @param mutex the mutex to be locked.
 *
 * @return 0 on success;
 * @return an error number if:
 * - EPERM, the caller context is invalid;
 * - EINVAL, the mutex is invalid;
 * - EPERM, the mutex is not process-shared and does not belong to the current
 *   process;
 * - EBUSY, the mutex was locked by another thread than the current one;
 * - EAGAIN, the mutex is recursive, and the maximum number of recursive locks
 *   has been exceeded.
 *
 * @see
 * <a href="http://www.opengroup.org/onlinepubs/000095399/functions/pthread_mutex_trylock.html">
 * Specification.</a>
 *
 * @apitags{xthread-only, switch-primary}
 */
COBALT_IMPL(int, pthread_mutex_trylock, (pthread_mutex_t *mutex))
{
	struct cobalt_mutex_shadow *_mutex =
		&((union cobalt_mutex_union *)mutex)->shadow_mutex;
	int status, err;
	xnhandle_t cur;

	cur = cobalt_get_current();
	if (cur == XN_NO_HANDLE)
		return EPERM;

	if (_mutex->magic != COBALT_MUTEX_MAGIC)
		goto autoinit;

  cont:
	status = cobalt_get_current_mode();
	if ((status & (XNRELAX|XNWEAK|XNDEBUG)) == 0) {
		err = xnsynch_fast_acquire(mutex_get_ownerp(_mutex), cur);
		if (err == 0) {
			_mutex->lockcnt = 1;
			return 0;
		}
	} else {
		err = xnsynch_fast_owner_check(mutex_get_ownerp(_mutex), cur);
		if (err < 0)
			goto do_syscall;

		err = -EBUSY;
	}

	if (err == -EBUSY) {
		if (_mutex->attr.type == PTHREAD_MUTEX_RECURSIVE) {
			if (_mutex->lockcnt == UINT32_MAX)
				return EAGAIN;

			++_mutex->lockcnt;
			return 0;
		}
		return EBUSY;
	}

do_syscall:

	do {
		err = XENOMAI_SYSCALL1(sc_cobalt_mutex_trylock, _mutex);
	} while (err == -EINTR);

	if (!err)
		_mutex->lockcnt = 1;

	return -err;

  autoinit:
	err = cobalt_mutex_autoinit(mutex);
	if (err)
		return err;
	goto cont;
}

/**
 * Unlock a mutex.
 *
 * This service unlocks the mutex @a mx. If the mutex is of the @a
 * PTHREAD_MUTEX_RECURSIVE @a type and the locking recursion count is greater
 * than one, the lock recursion count is decremented and the mutex remains
 * locked.
 *
 * Attempting to unlock a mutex which is not locked or which is locked by
 * another thread than the current one yields the EPERM error, whatever the
 * mutex @a type attribute.
 *
 * @param mutex the mutex to be released.
 *
 * @return 0 on success;
 * @return an error number if:
 * - EPERM, the caller context is invalid;
 * - EINVAL, the mutex @a mx is invalid;
 * - EPERM, the mutex was not locked by the current thread.
 *
 * @see
 * <a href="http://www.opengroup.org/onlinepubs/000095399/functions/pthread_mutex_unlock.html">
 * Specification.</a>
 *
 * @apitags{xthread-only, switch-primary}
 */
COBALT_IMPL(int, pthread_mutex_unlock, (pthread_mutex_t *mutex))
{
	struct cobalt_mutex_shadow *_mutex =
		&((union cobalt_mutex_union *)mutex)->shadow_mutex;
	struct cobalt_mutex_state *state;
	xnhandle_t cur;
	int err;

	if (_mutex->magic != COBALT_MUTEX_MAGIC)
		goto autoinit;

  cont:
	cur = cobalt_get_current();
	if (cur == XN_NO_HANDLE)
		return EPERM;

	state = mutex_get_state(_mutex);
	if (xnsynch_fast_owner_check(&state->owner, cur) != 0)
		return EPERM;

	if (_mutex->lockcnt > 1) {
		--_mutex->lockcnt;
		return 0;
	}

	if ((state->flags & COBALT_MUTEX_COND_SIGNAL))
		goto do_syscall;

	if (cobalt_get_current_mode() & (XNWEAK|XNDEBUG))
		goto do_syscall;

	if (xnsynch_fast_release(&state->owner, cur))
		return 0;
do_syscall:

	do {
		err = XENOMAI_SYSCALL1(sc_cobalt_mutex_unlock, _mutex);
	} while (err == -EINTR);

	return -err;

  autoinit:
	err = cobalt_mutex_autoinit(mutex);
	if (err)
		return err;
	goto cont;
}

/**
 * Initialize a mutex attributes object.
 *
 * This services initializes the mutex attributes object @a attr with default
 * values for all attributes. Default value are :
 * - for the @a type attribute, @a PTHREAD_MUTEX_NORMAL;
 * - for the @a protocol attribute, @a PTHREAD_PRIO_NONE;
 * - for the @a pshared attribute, @a PTHREAD_PROCESS_PRIVATE.
 *
 * If this service is called specifying a mutex attributes object that was
 * already initialized, the attributes object is reinitialized.
 *
 * @param attr the mutex attributes object to be initialized.
 *
 * @return 0 on success;
 * @return an error number if:
 * - ENOMEM, the mutex attributes object pointer @a attr is @a NULL.
 *
 * @see
 * <a href="http://www.opengroup.org/onlinepubs/000095399/functions/pthread_mutexattr_init.html">
 * Specification.</a>
 *
 * @apitags{thread-unrestricted}
 */
int pthread_mutexattr_init(pthread_mutexattr_t * attr);

/**
 * Destroy a mutex attributes object.
 *
 * This service destroys the mutex attributes object @a attr. The object becomes
 * invalid for all mutex services (they all return EINVAL) except
 * pthread_mutexattr_init().
 *
 * @param attr the initialized mutex attributes object to be destroyed.
 *
 * @return 0 on success;
 * @return an error number if:
 * - EINVAL, the mutex attributes object @a attr is invalid.
 *
 * @see
 * <a href="http://www.opengroup.org/onlinepubs/000095399/functions/pthread_mutexattr_destroy.html">
 * Specification.</a>
 *
 * @apitags{thread-unrestricted}
 */
int pthread_mutexattr_destroy(pthread_mutexattr_t * attr);

/**
 * Get the mutex type attribute from a mutex attributes object.
 *
 * This service stores, at the address @a type, the value of the @a type
 * attribute in the mutex attributes object @a attr.
 *
 * See pthread_mutex_lock() and pthread_mutex_unlock() for a
 * description of the values of the @a type attribute and their effect
 * on a mutex.
 *
 * @param attr an initialized mutex attributes object,
 *
 * @param type address where the @a type attribute value will be stored on
 * success.
 *
 * @return 0 on sucess,
 * @return an error number if:
 * - EINVAL, the @a type address is invalid;
 * - EINVAL, the mutex attributes object @a attr is invalid.
 *
 * @see
 * <a href="http://www.opengroup.org/onlinepubs/000095399/functions/pthread_mutexattr_gettype.html">
 * Specification.</a>
 *
 * @apitags{thread-unrestricted}
 */
int pthread_mutexattr_gettype(const pthread_mutexattr_t * attr, int *type);

/**
 * Set the mutex type attribute of a mutex attributes object.
 *
 * This service set the @a type attribute of the mutex attributes object
 * @a attr.
 *
 * See pthread_mutex_lock() and pthread_mutex_unlock() for a
 * description of the values of the @a type attribute and their effect
 * on a mutex.
 *
 * The @a PTHREAD_MUTEX_DEFAULT default @a type is the same as @a
 * PTHREAD_MUTEX_NORMAL. Note that using a recursive Cobalt mutex with
 * a Cobalt condition variable is safe (see pthread_cond_wait()
 * documentation).
 *
 * @param attr an initialized mutex attributes object,
 *
 * @param type value of the @a type attribute.
 *
 * @return 0 on success,
 * @return an error number if:
 * - EINVAL, the mutex attributes object @a attr is invalid;
 * - EINVAL, the value of @a type is invalid for the @a type attribute.
 *
 * @see
 * <a href="http://www.opengroup.org/onlinepubs/000095399/functions/pthread_mutexattr_settype.html">
 * Specification.</a>
 *
 * @apitags{thread-unrestricted}
 */
int pthread_mutexattr_settype(pthread_mutexattr_t * attr, int type);

/**
 * Get the protocol attribute from a mutex attributes object.
 *
 * This service stores, at the address @a proto, the value of the @a protocol
 * attribute in the mutex attributes object @a attr.
 *
 * The @a protcol attribute may only be one of @a PTHREAD_PRIO_NONE or @a
 * PTHREAD_PRIO_INHERIT. See pthread_mutexattr_setprotocol() for the meaning of
 * these two constants.
 *
 * @param attr an initialized mutex attributes object;
 *
 * @param proto address where the value of the @a protocol attribute will be
 * stored on success.
 *
 * @return 0 on success,
 * @return an error number if:
 * - EINVAL, the @a proto address is invalid;
 * - EINVAL, the mutex attributes object @a attr is invalid.
 *
 * @see
 * <a href="http://www.opengroup.org/onlinepubs/000095399/functions/pthread_mutexattr_getprotocol.html">
 * Specification.</a>
 *
 * @apitags{thread-unrestricted}
 */
int pthread_mutexattr_getprotocol(const pthread_mutexattr_t * attr, int *proto);

/**
 * Set the protocol attribute of a mutex attributes object.
 *
 * This service set the @a type attribute of the mutex attributes object
 * @a attr.
 *
 * @param attr an initialized mutex attributes object,
 *
 * @param proto value of the @a protocol attribute, may be one of:
 * - PTHREAD_PRIO_NONE, meaning that a mutex created with the attributes object
 *   @a attr will not follow any priority protocol;
 * - PTHREAD_PRIO_INHERIT, meaning that a mutex created with the attributes
 *   object @a attr, will follow the priority inheritance protocol.
 *
 * The value PTHREAD_PRIO_PROTECT (priority ceiling protocol) is unsupported.
 *
 * @return 0 on success,
 * @return an error number if:
 * - EINVAL, the mutex attributes object @a attr is invalid;
 * - ENOTSUP, the value of @a proto is unsupported;
 * - EINVAL, the value of @a proto is invalid.
 *
 * @see
 * <a href="http://www.opengroup.org/onlinepubs/000095399/functions/pthread_mutexattr_setprotocol.html">
 * Specification.</a>
 *
 * @apitags{thread-unrestricted}
 */
int pthread_mutexattr_setprotocol(pthread_mutexattr_t * attr, int proto);

/**
 * Get the process-shared attribute of a mutex attributes object.
 *
 * This service stores, at the address @a pshared, the value of the @a pshared
 * attribute in the mutex attributes object @a attr.
 *
 * The @a pashared attribute may only be one of @a PTHREAD_PROCESS_PRIVATE or
 * @a PTHREAD_PROCESS_SHARED. See pthread_mutexattr_setpshared() for the meaning
 * of these two constants.
 *
 * @param attr an initialized mutex attributes object;
 *
 * @param pshared address where the value of the @a pshared attribute will be
 * stored on success.
 *
 * @return 0 on success;
 * @return an error number if:
 * - EINVAL, the @a pshared address is invalid;
 * - EINVAL, the mutex attributes object @a attr is invalid.
 *
 * @see
 * <a href="http://www.opengroup.org/onlinepubs/000095399/functions/pthread_mutexattr_getpshared.html">
 * Specification.</a>
 *
 * @apitags{thread-unrestricted}
 */
int pthread_mutexattr_getpshared(const pthread_mutexattr_t *attr, int *pshared);

/**
 * Set the process-shared attribute of a mutex attributes object.
 *
 * This service set the @a pshared attribute of the mutex attributes object @a
 * attr.
 *
 * @param attr an initialized mutex attributes object.
 *
 * @param pshared value of the @a pshared attribute, may be one of:
 * - PTHREAD_PROCESS_PRIVATE, meaning that a mutex created with the attributes
 *   object @a attr will only be accessible by threads within the same process
 *   as the thread that initialized the mutex;
 * - PTHREAD_PROCESS_SHARED, meaning that a mutex created with the attributes
 *   object @a attr will be accessible by any thread that has access to the
 *   memory where the mutex is allocated.
 *
 * @return 0 on success,
 * @return an error status if:
 * - EINVAL, the mutex attributes object @a attr is invalid;
 * - EINVAL, the value of @a pshared is invalid.
 *
 * @see
 * <a href="http://www.opengroup.org/onlinepubs/000095399/functions/pthread_mutexattr_setpshared.html">
 * Specification.</a>
 *
 * @apitags{thread-unrestricted}
 */
int pthread_mutexattr_setpshared(pthread_mutexattr_t *attr, int pshared);

/** @} */
