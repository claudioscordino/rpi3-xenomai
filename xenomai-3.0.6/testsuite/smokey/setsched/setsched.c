/*
 * Scheduler live-adjustment test.
 *
 * Copyright (c) Siemens AG 2016
 *
 * Authors:
 *  Jan Kiszka <jan.kiszka@siemens.com>
 *
 * Released under the terms of GPLv2.
 */
#include <stdio.h>
#include <stdbool.h>
#include <pthread.h>
#include <error.h>
#include <semaphore.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <smokey/smokey.h>
#include <sys/cobalt.h>

smokey_test_plugin(setsched, SMOKEY_NOARGS,
   "Validate correct application of scheduling parameters to running threads."
);

static pid_t thread_pid;

static void __check_linux_schedparams(int expected_policy, int expected_prio,
				      int line)
{
	struct sched_param linux_param;
	int linux_policy;

	linux_policy = syscall(SYS_sched_getscheduler, thread_pid);
	if (smokey_check_status(syscall(SYS_sched_getparam, thread_pid,
					&linux_param)))
		pthread_exit((void *)(long)-EINVAL);

	if (!smokey_assert(linux_policy == expected_policy) ||
	    !smokey_assert(linux_param.sched_priority == expected_prio)) {
		smokey_warning("called from line %d", line);
		pthread_exit((void *)(long)-EINVAL);
	}
}

#define check_linux_schedparams(pol, prio)	\
	__check_linux_schedparams(pol, prio, __LINE__)

static void __check_rt_schedparams(int expected_policy, int expected_prio,
				   int line)
{
	struct sched_param cobalt_param;
	int cobalt_policy;

	if (smokey_check_status(pthread_getschedparam(pthread_self(),
						      &cobalt_policy,
						      &cobalt_param)))
		pthread_exit((void *)(long)-EINVAL);

	if (!smokey_assert(cobalt_policy == expected_policy) ||
	    !smokey_assert(cobalt_param.sched_priority == expected_prio)) {
		smokey_warning("called from line %d", line);
		pthread_exit((void *)(long)-EINVAL);
	}
}

#define check_rt_schedparams(pol, prio)	\
	__check_rt_schedparams(pol, prio, __LINE__)

static void *thread_body(void *arg)
{
	struct cobalt_threadstat stats;
	struct sched_param param;
	unsigned long long msw;

	thread_pid = syscall(SYS_gettid);

	check_rt_schedparams(SCHED_FIFO, 1);
	check_linux_schedparams(SCHED_FIFO, 1);

	if (smokey_check_status(cobalt_thread_stat(thread_pid, &stats)))
		pthread_exit((void *)(long)-EINVAL);
	msw = stats.msw;

	param.sched_priority = 2;
	if (smokey_check_status(pthread_setschedparam(pthread_self(),
						      SCHED_FIFO, &param)))
		pthread_exit((void *)(long)-EINVAL);

	check_rt_schedparams(SCHED_FIFO, 2);

	if (smokey_check_status(cobalt_thread_stat(thread_pid, &stats)) ||
	    !smokey_assert(stats.msw == msw))
		pthread_exit((void *)(long)-EINVAL);

	check_linux_schedparams(SCHED_FIFO, 2);

	return (void *)0L;
}

static int run_setsched(struct smokey_test *t, int argc, char *const argv[])
{
	struct sched_param param;
	pthread_attr_t attr;
	pthread_t thread;
	void *retval;
	int ret;

	pthread_attr_init(&attr);
	pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);
	pthread_attr_setschedpolicy(&attr, SCHED_FIFO);
	param.sched_priority = 1;
	pthread_attr_setschedparam(&attr, &param);
	ret = pthread_create(&thread, &attr, thread_body, NULL);
	if (ret)
		error(1, ret, "pthread_create");

	pthread_join(thread, &retval);

	return (long)retval;
}
