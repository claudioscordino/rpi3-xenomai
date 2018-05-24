/*
 * High resolution timer test software
 *
 * (C) 2013      Clark Williams <williams@redhat.com>
 * (C) 2013      John Kacur <jkacur@redhat.com>
 * (C) 2008-2012 Clark Williams <williams@redhat.com>
 * (C) 2005-2007 Thomas Gleixner <tglx@linutronix.de>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License Version
 * 2 as published by the Free Software Foundation.
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdarg.h>
#include <unistd.h>
#include <fcntl.h>
#include <getopt.h>
#include <pthread.h>
#include <signal.h>
#include <sched.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <limits.h>
#include <linux/unistd.h>

#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/sysinfo.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <sys/utsname.h>
#include <sys/mman.h>
#include "rt_numa.h"

#include "rt-utils.h"

#define DEFAULT_INTERVAL 1000
#define DEFAULT_DISTANCE 500

#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))

/* Ugly, but .... */
#define gettid() syscall(__NR_gettid)
#define sigev_notify_thread_id _sigev_un._tid

#define USEC_PER_SEC		1000000
#define NSEC_PER_SEC		1000000000

#define HIST_MAX		1000000

#define MODE_CYCLIC		0
#define MODE_CLOCK_NANOSLEEP	1
#define MODE_SYS_ITIMER		2
#define MODE_SYS_NANOSLEEP	3
#define MODE_SYS_OFFSET		2

#define TIMER_RELTIME		0

/* Must be power of 2 ! */
#define VALBUF_SIZE		16384

#define KVARS			32
#define KVARNAMELEN		32
#define KVALUELEN		32

int enable_events;

static char *policyname(int policy);

#define write_check(__fd, __buf, __len)			\
	do {						\
		int __ret = write(__fd, __buf, __len);	\
		(void)__ret;				\
	} while (0);

enum {
	NOTRACE,
	CTXTSWITCH,
	IRQSOFF,
	PREEMPTOFF,
	PREEMPTIRQSOFF,
	WAKEUP,
	WAKEUPRT,
	LATENCY,
	FUNCTION,
	CUSTOM,
};

/* Struct to transfer parameters to the thread */
struct thread_param {
	int prio;
	int policy;
	int mode;
	int timermode;
	int signal;
	int clock;
	unsigned long max_cycles;
	struct thread_stat *stats;
	int bufmsk;
	unsigned long interval;
	int cpu;
	int node;
	int tnum;
};

/* Struct for statistics */
struct thread_stat {
	unsigned long cycles;
	unsigned long cyclesread;
	long min;
	long max;
	long act;
	double avg;
	long *values;
	long *hist_array;
	long *outliers;
	pthread_t thread;
	int threadstarted;
	int tid;
	long reduce;
	long redmax;
	long cycleofmax;
	long hist_overflow;
	long num_outliers;
};

static int shutdown;
static int tracelimit = 0;
static int notrace = 0;
static int ftrace = 0;
static int kernelversion;
static int verbose = 0;
static int oscope_reduction = 1;
static int lockall = 0;
static int tracetype = NOTRACE;
static int histogram = 0;
static int histofall = 0;
static int duration = 0;
static int use_nsecs = 0;
static int refresh_on_max;
static int force_sched_other;
static int priospread = 0;
static int check_clock_resolution;
static int ct_debug;
static int use_fifo = 0;
static pthread_t fifo_threadid;
static int aligned = 0;
static int secaligned = 0;
static int offset = 0;
static int laptop = 0;

static pthread_cond_t refresh_on_max_cond = PTHREAD_COND_INITIALIZER;
static pthread_mutex_t refresh_on_max_lock = PTHREAD_MUTEX_INITIALIZER;

static pthread_mutex_t break_thread_id_lock = PTHREAD_MUTEX_INITIALIZER;
static pid_t break_thread_id = 0;
static uint64_t break_thread_value = 0;

static struct timespec globalt;

/* Backup of kernel variables that we modify */
static struct kvars {
	char name[KVARNAMELEN];
	char value[KVALUELEN];
} kv[KVARS];

static char *procfileprefix = "/proc/sys/kernel/";
static char *fileprefix;
static char tracer[MAX_PATH];
static char fifopath[MAX_PATH];
static char **traceptr;
static int traceopt_count;
static int traceopt_size;

static struct thread_param **parameters;
static struct thread_stat **statistics;

static void print_stat(FILE *fp, struct thread_param *par, int index, int verbose, int quiet);

static int latency_target_fd = -1;
static int32_t latency_target_value = 0;

/* Latency trick
 * if the file /dev/cpu_dma_latency exists,
 * open it and write a zero into it. This will tell
 * the power management system not to transition to
 * a high cstate (in fact, the system acts like idle=poll)
 * When the fd to /dev/cpu_dma_latency is closed, the behavior
 * goes back to the system default.
 *
 * Documentation/power/pm_qos_interface.txt
 */
static void set_latency_target(void)
{
	struct stat s;
	int err;

	if (laptop) {
		warn("not setting cpu_dma_latency to save battery power\n");
		return;
	}

	errno = 0;
	err = stat("/dev/cpu_dma_latency", &s);
	if (err == -1) {
		err_msg_n(errno, "WARN: stat /dev/cpu_dma_latency failed");
		return;
	}

	errno = 0;
	latency_target_fd = open("/dev/cpu_dma_latency", O_RDWR);
	if (latency_target_fd == -1) {
		err_msg_n(errno, "WARN: open /dev/cpu_dma_latency");
		return;
	}

	errno = 0;
	err = write(latency_target_fd, &latency_target_value, 4);
	if (err < 1) {
		err_msg_n(errno, "# error setting cpu_dma_latency to %d!", latency_target_value);
		close(latency_target_fd);
		return;
	}
	printf("# /dev/cpu_dma_latency set to %dus\n", latency_target_value);
}


enum kernelversion {
	KV_NOT_SUPPORTED,
	KV_26_LT18,
	KV_26_LT24,
	KV_26_33,
	KV_30
};

enum {
	ERROR_GENERAL	= -1,
	ERROR_NOTFOUND	= -2,
};

static char functiontracer[MAX_PATH];
static char traceroptions[MAX_PATH];

static int trace_fd     = -1;
static int tracemark_fd = -1;

static int kernvar(int mode, const char *name, char *value, size_t sizeofvalue)
{
	char filename[128];
	int retval = 1;
	int path;
	size_t len_prefix = strlen(fileprefix), len_name = strlen(name);

	if (len_prefix + len_name + 1 > sizeof(filename)) {
		errno = ENOMEM;
		return 1;
	}

	memcpy(filename, fileprefix, len_prefix);
	memcpy(filename + len_prefix, name, len_name + 1);

	path = open(filename, mode);
	if (path >= 0) {
		if (mode == O_RDONLY) {
			int got;
			if ((got = read(path, value, sizeofvalue)) > 0) {
				retval = 0;
				value[got-1] = '\0';
			}
		} else if (mode == O_WRONLY) {
			if (write(path, value, sizeofvalue) == sizeofvalue)
				retval = 0;
		}
		close(path);
	}
	return retval;
}

static void setkernvar(const char *name, char *value)
{
	int i;
	char oldvalue[KVALUELEN];

	if (kernelversion < KV_26_33) {
		if (kernvar(O_RDONLY, name, oldvalue, sizeof(oldvalue)))
			fprintf(stderr, "could not retrieve %s\n", name);
		else {
			for (i = 0; i < KVARS; i++) {
				if (!strcmp(kv[i].name, name))
					break;
				if (kv[i].name[0] == '\0') {
					strncpy(kv[i].name, name,
						sizeof(kv[i].name));
					strncpy(kv[i].value, oldvalue,
					    sizeof(kv[i].value));
					break;
				}
			}
			if (i == KVARS)
				fprintf(stderr, "could not backup %s (%s)\n",
					name, oldvalue);
		}
	}
	if (kernvar(O_WRONLY, name, value, strlen(value)))
		fprintf(stderr, "could not set %s to %s\n", name, value);

}

static void restorekernvars(void)
{
	int i;

	for (i = 0; i < KVARS; i++) {
		if (kv[i].name[0] != '\0') {
			if (kernvar(O_WRONLY, kv[i].name, kv[i].value,
			    strlen(kv[i].value)))
				fprintf(stderr, "could not restore %s to %s\n",
					kv[i].name, kv[i].value);
		}
	}
}

static inline void tsnorm(struct timespec *ts)
{
	while (ts->tv_nsec >= NSEC_PER_SEC) {
		ts->tv_nsec -= NSEC_PER_SEC;
		ts->tv_sec++;
	}
}

static inline int tsgreater(struct timespec *a, struct timespec *b)
{
	return ((a->tv_sec > b->tv_sec) ||
		(a->tv_sec == b->tv_sec && a->tv_nsec > b->tv_nsec));
}

static inline int64_t calcdiff(struct timespec t1, struct timespec t2)
{
	int64_t diff;
	diff = USEC_PER_SEC * (long long)((int) t1.tv_sec - (int) t2.tv_sec);
	diff += ((int) t1.tv_nsec - (int) t2.tv_nsec) / 1000;
	return diff;
}

static inline int64_t calcdiff_ns(struct timespec t1, struct timespec t2)
{
	int64_t diff;
	diff = NSEC_PER_SEC * (int64_t)((int) t1.tv_sec - (int) t2.tv_sec);
	diff += ((int) t1.tv_nsec - (int) t2.tv_nsec);
	return diff;
}

void traceopt(char *option)
{
	char *ptr;
	if (traceopt_count + 1 > traceopt_size) {
		traceopt_size += 16;
		printf("expanding traceopt buffer to %d entries\n", traceopt_size);
		traceptr = realloc(traceptr, sizeof(char*) * traceopt_size);
		if (traceptr == NULL)
			fatal ("Error allocating space for %d trace options\n",
			       traceopt_count+1);
	}
	ptr = malloc(strlen(option)+1);
	if (ptr == NULL)
		fatal("error allocating space for trace option %s\n", option);
	printf("adding traceopt %s\n", option);
	strcpy(ptr, option);
	traceptr[traceopt_count++] = ptr;
}

static int trace_file_exists(char *name)
{
	struct stat sbuf;
	char *tracing_prefix = get_debugfileprefix();
	char path[MAX_PATH];
	strcat(strcpy(path, tracing_prefix), name);
	return stat(path, &sbuf) ? 0 : 1;
}

#define TRACEBUFSIZ 1024
static __thread char tracebuf[TRACEBUFSIZ];

static void tracemark(char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void tracemark(char *fmt, ...)
{
	va_list ap;
	int len;

	/* bail out if we're not tracing */
	/* or if the kernel doesn't support trace_mark */
	if (tracemark_fd < 0)
		return;

	va_start(ap, fmt);
	len = vsnprintf(tracebuf, TRACEBUFSIZ, fmt, ap);
	va_end(ap);
	write_check(tracemark_fd, tracebuf, len);
}



void tracing(int on)
{
	if (on) {
		switch (kernelversion) {
		case KV_26_LT18: gettimeofday(0,(struct timezone *)1); break;
		case KV_26_LT24: prctl(0, 1); break;
		case KV_26_33:
		case KV_30:
			write_check(trace_fd, "1", 1);
			break;
		default:	 break;
		}
	} else {
		switch (kernelversion) {
		case KV_26_LT18: gettimeofday(0,0); break;
		case KV_26_LT24: prctl(0, 0); break;
		case KV_26_33:
		case KV_30:
			write_check(trace_fd, "0", 1);
			break;
		default:	break;
		}
	}
}

static int settracer(char *tracer)
{
	if (valid_tracer(tracer)) {
		setkernvar("current_tracer", tracer);
		return 0;
	}
	return -1;
}

static void setup_tracer(void)
{
	if (!tracelimit || notrace)
		return;

	if (mount_debugfs(NULL))
		fatal("could not mount debugfs");

	if (kernelversion >= KV_26_33) {
		char testname[MAX_PATH];

		fileprefix = get_debugfileprefix();
		if (!trace_file_exists("tracing_enabled") &&
		    !trace_file_exists("tracing_on"))
			warn("tracing_enabled or tracing_on not found\n"
			    "debug fs not mounted, "
			    "TRACERs not configured?\n", testname);
	} else
		fileprefix = procfileprefix;

	if (kernelversion >= KV_26_33) {
		int ret;

		if (trace_file_exists("tracing_enabled") &&
		    !trace_file_exists("tracing_on"))
			setkernvar("tracing_enabled", "1");

		/* ftrace_enabled is a sysctl variable */
		/* turn it on if you're doing anything but nop or event tracing */

		fileprefix = procfileprefix;
		if (tracetype)
			setkernvar("ftrace_enabled", "1");
		else
			setkernvar("ftrace_enabled", "0");
		fileprefix = get_debugfileprefix();

		/*
		 * Set default tracer to nop.
		 * this also has the nice side effect of clearing out
		 * old traces.
		 */
		ret = settracer("nop");

		switch (tracetype) {
		case NOTRACE:
			/* no tracer specified, use events */
			enable_events = 1;
			break;
		case FUNCTION:
			ret = settracer("function");
			break;
		case IRQSOFF:
			ret = settracer("irqsoff");
			break;
		case PREEMPTOFF:
			ret = settracer("preemptoff");
			break;
		case PREEMPTIRQSOFF:
			ret = settracer("preemptirqsoff");
			break;
		case CTXTSWITCH:
			if (valid_tracer("sched_switch"))
			    ret = settracer("sched_switch");
			else {
				if ((ret = event_enable("sched/sched_wakeup")))
					break;
				ret = event_enable("sched/sched_switch");
			}
			break;
		case WAKEUP:
			ret = settracer("wakeup");
			break;
		case WAKEUPRT:
			ret = settracer("wakeup_rt");
			break;
		default:
			if (strlen(tracer)) {
				ret = settracer(tracer);
				if (strcmp(tracer, "events") == 0 && ftrace)
					ret = settracer(functiontracer);
			}
			else {
				printf("cyclictest: unknown tracer!\n");
				ret = 0;
			}
			break;
		}

		if (enable_events)
			/* turn on all events */
			event_enable_all();

		if (ret)
			fprintf(stderr, "Requested tracer '%s' not available\n", tracer);

		setkernvar(traceroptions, "print-parent");
		setkernvar(traceroptions, "latency-format");
		if (verbose) {
			setkernvar(traceroptions, "sym-offset");
			setkernvar(traceroptions, "sym-addr");
			setkernvar(traceroptions, "verbose");
		} else {
			setkernvar(traceroptions, "nosym-offset");
			setkernvar(traceroptions, "nosym-addr");
			setkernvar(traceroptions, "noverbose");
		}
		if (traceopt_count) {
			int i;
			for (i = 0; i < traceopt_count; i++)
				setkernvar(traceroptions, traceptr[i]);
		}
		setkernvar("tracing_max_latency", "0");
		if (trace_file_exists("latency_hist"))
			setkernvar("latency_hist/wakeup/reset", "1");

		/* open the tracing on file descriptor */
		if (trace_fd == -1) {
			char path[MAX_PATH];
			strcpy(path, fileprefix);
			if (trace_file_exists("tracing_on"))
				strcat(path, "tracing_on");
			else
				strcat(path, "tracing_enabled");
			if ((trace_fd = open(path, O_WRONLY)) == -1)
				fatal("unable to open %s for tracing", path);
		}

		/* open the tracemark file descriptor */
		if (tracemark_fd == -1) {
			char path[MAX_PATH];
			strcat(strcpy(path, fileprefix), "trace_marker");
			if ((tracemark_fd = open(path, O_WRONLY)) == -1)
				warn("unable to open trace_marker file: %s\n", path);
		}

	} else {
		setkernvar("trace_all_cpus", "1");
		setkernvar("trace_freerunning", "1");
		setkernvar("trace_print_on_crash", "0");
		setkernvar("trace_user_triggered", "1");
		setkernvar("trace_user_trigger_irq", "-1");
		setkernvar("trace_verbose", "0");
		setkernvar("preempt_thresh", "0");
		setkernvar("wakeup_timing", "0");
		setkernvar("preempt_max_latency", "0");
		if (ftrace)
			setkernvar("mcount_enabled", "1");
		setkernvar("trace_enabled", "1");
		setkernvar("latency_hist/wakeup_latency/reset", "1");
	}

	tracing(1);
}

/*
 * parse an input value as a base10 value followed by an optional
 * suffix. The input value is presumed to be in seconds, unless
 * followed by a modifier suffix: m=minutes, h=hours, d=days
 *
 * the return value is a value in seconds
 */
int parse_time_string(char *val)
{
	char *end;
	int t = strtol(val, &end, 10);
	if (end) {
		switch (*end) {
		case 'm':
		case 'M':
			t *= 60;
			break;

		case 'h':
		case 'H':
			t *= 60*60;
			break;

		case 'd':
		case 'D':
			t *= 24*60*60;
			break;

		}
	}
	return t;
}

/*
 * Raise the soft priority limit up to prio, if that is less than or equal
 * to the hard limit
 * if a call fails, return the error
 * if successful return 0
 * if fails, return -1
*/
static int raise_soft_prio(int policy, const struct sched_param *param)
{
	int err;
	int policy_max;	/* max for scheduling policy such as SCHED_FIFO */
	int soft_max;
	int hard_max;
	int prio;
	struct rlimit rlim;

	prio = param->sched_priority;

	policy_max = sched_get_priority_max(policy);
	if (policy_max == -1) {
		err = errno;
		err_msg("WARN: no such policy\n");
		return err;
	}

	err = getrlimit(RLIMIT_RTPRIO, &rlim);
	if (err) {
		err = errno;
		err_msg_n(err, "WARN: getrlimit failed");
		return err;
	}

	soft_max = (rlim.rlim_cur == RLIM_INFINITY) ? policy_max : rlim.rlim_cur;
	hard_max = (rlim.rlim_max == RLIM_INFINITY) ? policy_max : rlim.rlim_max;

	if (prio > soft_max && prio <= hard_max) {
		rlim.rlim_cur = prio;
		err = setrlimit(RLIMIT_RTPRIO, &rlim);
		if (err) {
			err = errno;
			err_msg_n(err, "WARN: setrlimit failed");
			/* return err; */
		}
	} else {
		err = -1;
	}

	return err;
}

/*
 * Check the error status of sched_setscheduler
 * If an error can be corrected by raising the soft limit priority to
 * a priority less than or equal to the hard limit, then do so.
 */
static int setscheduler(pid_t pid, int policy, const struct sched_param *param)
{
	int err = 0;

try_again:
	err = sched_setscheduler(pid, policy, param);
	if (err) {
		err = errno;
		if (err == EPERM) {
			int err1;
			err1 = raise_soft_prio(policy, param);
			if (!err1) goto try_again;
		}
	}

	return err;
}

/* Work around lack of barriers in oldish uClibc-based toolchains. */

static struct thread_barrier {
	pthread_mutex_t lock;
	pthread_cond_t wait;
	unsigned int count;
} align_barr, globalt_barr;

static inline
void barrier_init(struct thread_barrier *__restrict barrier,
		 unsigned int count)
{
	pthread_mutex_init(&barrier->lock, NULL);
	pthread_cond_init(&barrier->wait, NULL);
	barrier->count = count;
}

static inline void barrier_destroy(struct thread_barrier *barrier)
{
	pthread_mutex_destroy(&barrier->lock);
	pthread_cond_destroy(&barrier->wait);
}

static inline void barrier_wait(struct thread_barrier *barrier)
{
	pthread_mutex_lock(&barrier->lock);

	if (barrier->count > 0) {
		barrier->count--;
		pthread_cond_broadcast(&barrier->wait);
		while (barrier->count > 0)
			pthread_cond_wait(&barrier->wait, &barrier->lock);
	}

	pthread_mutex_unlock(&barrier->lock);
}

/*
 * timer thread
 *
 * Modes:
 * - clock_nanosleep based
 * - cyclic timer based
 *
 * Clock:
 * - CLOCK_MONOTONIC
 * - CLOCK_REALTIME
 *
 */
void *timerthread(void *param)
{
	struct thread_param *par = param;
	struct sched_param schedp;
	struct sigevent sigev;
	sigset_t sigset;
	timer_t timer;
	struct timespec now, next, interval, stop;
	struct itimerval itimer;
	struct itimerspec tspec;
	struct thread_stat *stat = par->stats;
	int stopped = 0;
	cpu_set_t mask;
	pthread_t thread;

	/* if we're running in numa mode, set our memory node */
	if (par->node != -1)
		rt_numa_set_numa_run_on_node(par->node, par->cpu);

	if (par->cpu != -1) {
		CPU_ZERO(&mask);
		CPU_SET(par->cpu, &mask);
		thread = pthread_self();
		if(pthread_setaffinity_np(thread, sizeof(mask), &mask) == -1)
			warn("Could not set CPU affinity to CPU #%d\n", par->cpu);
	}

	interval.tv_sec = par->interval / USEC_PER_SEC;
	interval.tv_nsec = (par->interval % USEC_PER_SEC) * 1000;

	stat->tid = gettid();

	sigemptyset(&sigset);
	sigaddset(&sigset, par->signal);
	sigprocmask(SIG_BLOCK, &sigset, NULL);

	if (par->mode == MODE_CYCLIC) {
		sigev.sigev_notify = SIGEV_THREAD_ID | SIGEV_SIGNAL;
		sigev.sigev_signo = par->signal;
		sigev.sigev_notify_thread_id = stat->tid;
		timer_create(par->clock, &sigev, &timer);
		tspec.it_interval = interval;
	}

	memset(&schedp, 0, sizeof(schedp));
	schedp.sched_priority = par->prio;
	if (pthread_setschedparam(pthread_self(), par->policy, &schedp))
		fatal("timerthread%d: failed to set priority to %d\n", par->cpu, par->prio);

	/* Get current time */
	if (aligned || secaligned) {
		barrier_wait(&globalt_barr);
		if (par->tnum == 0) {
			clock_gettime(par->clock, &globalt);
			if (secaligned) {
				/* Ensure that the thread start timestamp is not
				   in the past */
				if (globalt.tv_nsec > 900000000)
					globalt.tv_sec += 2;
				else
					globalt.tv_sec++;
				globalt.tv_nsec = 0;
			}
		}
		barrier_wait(&align_barr);
		now = globalt;
		if(offset) {
			if (aligned)
				now.tv_nsec += offset * par->tnum;
			else
				now.tv_nsec += offset;
			tsnorm(&now);
		}
	}
	else
		clock_gettime(par->clock, &now);

	next = now;
	next.tv_sec += interval.tv_sec;
	next.tv_nsec += interval.tv_nsec;
	tsnorm(&next);

	memset(&stop, 0, sizeof(stop)); /* grrr */

	if (duration) {
		stop = now;
		stop.tv_sec += duration;
	}
	if (par->mode == MODE_CYCLIC) {
		if (par->timermode == TIMER_ABSTIME)
			tspec.it_value = next;
		else {
			tspec.it_value = interval;
		}
		timer_settime(timer, par->timermode, &tspec, NULL);
	}

	if (par->mode == MODE_SYS_ITIMER) {
		itimer.it_interval.tv_sec = interval.tv_sec;
		itimer.it_interval.tv_usec = interval.tv_nsec / 1000;
		itimer.it_value = itimer.it_interval;
		setitimer (ITIMER_REAL, &itimer, NULL);
	}

	stat->threadstarted++;

	while (!shutdown) {

		uint64_t diff;
		int sigs, ret;

		/* Wait for next period */
		switch (par->mode) {
		case MODE_CYCLIC:
		case MODE_SYS_ITIMER:
			if (sigwait(&sigset, &sigs) < 0)
				goto out;
			break;

		case MODE_CLOCK_NANOSLEEP:
			if (par->timermode == TIMER_ABSTIME) {
				if ((ret = clock_nanosleep(par->clock, TIMER_ABSTIME, &next, NULL))) {
					if (ret != EINTR)
						warn("clock_nanosleep failed. errno: %d\n", errno);
					goto out;
				}
			} else {
				if ((ret = clock_gettime(par->clock, &now))) {
					if (ret != EINTR)
						warn("clock_gettime() failed: %s", strerror(errno));
					goto out;
				}
				if ((ret = clock_nanosleep(par->clock, TIMER_RELTIME, &interval, NULL))) {
					if (ret != EINTR)
						warn("clock_nanosleep() failed. errno: %d\n", errno);
					goto out;
				}
				next.tv_sec = now.tv_sec + interval.tv_sec;
				next.tv_nsec = now.tv_nsec + interval.tv_nsec;
				tsnorm(&next);
			}
			break;

		case MODE_SYS_NANOSLEEP:
			if ((ret = clock_gettime(par->clock, &now))) {
				if (ret != EINTR)
					warn("clock_gettime() failed: errno %d\n", errno);
				goto out;
			}
			if (nanosleep(&interval, NULL)) {
				if (errno != EINTR)
					warn("nanosleep failed. errno: %d\n", errno);
				goto out;
			}
			next.tv_sec = now.tv_sec + interval.tv_sec;
			next.tv_nsec = now.tv_nsec + interval.tv_nsec;
			tsnorm(&next);
			break;
		}

		if ((ret = clock_gettime(par->clock, &now))) {
			if (ret != EINTR)
				warn("clock_getttime() failed. errno: %d\n", errno);
			goto out;
		}

		if (use_nsecs)
			diff = calcdiff_ns(now, next);
		else
			diff = calcdiff(now, next);
		if (diff < stat->min)
			stat->min = diff;
		if (diff > stat->max) {
			stat->max = diff;
			if (refresh_on_max)
				pthread_cond_signal(&refresh_on_max_cond);
		}
		stat->avg += (double) diff;

		if (duration && (calcdiff(now, stop) >= 0))
			shutdown++;

		if (!stopped && tracelimit && (diff > tracelimit)) {
			stopped++;
			tracemark("hit latency threshold (%llu > %d)",
				  (unsigned long long) diff, tracelimit);
			tracing(0);
			shutdown++;
			pthread_mutex_lock(&break_thread_id_lock);
			if (break_thread_id == 0)
				break_thread_id = stat->tid;
			break_thread_value = diff;
			pthread_mutex_unlock(&break_thread_id_lock);
		}
		stat->act = diff;

		if (par->bufmsk)
			stat->values[stat->cycles & par->bufmsk] = diff;

		/* Update the histogram */
		if (histogram) {
			if (diff >= histogram) {
				stat->hist_overflow++;
				if (stat->num_outliers < histogram)
					stat->outliers[stat->num_outliers++] = stat->cycles;
			}
			else
				stat->hist_array[diff]++;
		}

		stat->cycles++;

		next.tv_sec += interval.tv_sec;
		next.tv_nsec += interval.tv_nsec;
		if (par->mode == MODE_CYCLIC) {
			int overrun_count = timer_getoverrun(timer);
			next.tv_sec += overrun_count * interval.tv_sec;
			next.tv_nsec += overrun_count * interval.tv_nsec;
		}
		tsnorm(&next);

		while (tsgreater(&now, &next)) {
			next.tv_sec += interval.tv_sec;
			next.tv_nsec += interval.tv_nsec;
			tsnorm(&next);
		}

		if (par->max_cycles && par->max_cycles == stat->cycles)
			break;
	}

out:
	if (par->mode == MODE_CYCLIC)
		timer_delete(timer);

	if (par->mode == MODE_SYS_ITIMER) {
		itimer.it_value.tv_sec = 0;
		itimer.it_value.tv_usec = 0;
		itimer.it_interval.tv_sec = 0;
		itimer.it_interval.tv_usec = 0;
		setitimer (ITIMER_REAL, &itimer, NULL);
	}

	/* switch to normal */
	schedp.sched_priority = 0;
	sched_setscheduler(0, SCHED_OTHER, &schedp);

	stat->threadstarted = -1;

	return NULL;
}


/* Print usage information */
static void display_help(int error)
{
	char tracers[MAX_PATH];
	char *prefix;

	prefix = get_debugfileprefix();
	if (prefix[0] == '\0')
		strcpy(tracers, "unavailable (debugfs not mounted)");
	else {
		fileprefix = prefix;
		if (kernvar(O_RDONLY, "available_tracers", tracers, sizeof(tracers)))
			strcpy(tracers, "none");
	}

	printf("cyclictest V %1.2f\n", VERSION_STRING);
	printf("Usage:\n"
	       "cyclictest <options>\n\n"
#if LIBNUMA_API_VERSION >= 2
	       "-a [CPUSET] --affinity     Run thread #N on processor #N, if possible, or if CPUSET\n"
	       "                           given, pin threads to that set of processors in round-\n"
	       "                           robin order.  E.g. -a 2 pins all threads to CPU 2,\n"
	       "                           but -a 3-5,0 -t 5 will run the first and fifth\n"
	       "                           threads on CPU (0),thread #2 on CPU 3, thread #3\n"
	       "                           on CPU 4, and thread #5 on CPU 5.\n"
#else
	       "-a [NUM] --affinity        run thread #N on processor #N, if possible\n"
	       "                           with NUM pin all threads to the processor NUM\n"
#endif
	       "-A USEC  --aligned=USEC    align thread wakeups to a specific offset\n"
	       "-b USEC  --breaktrace=USEC send break trace command when latency > USEC\n"
	       "-B       --preemptirqs     both preempt and irqsoff tracing (used with -b)\n"
	       "-c CLOCK --clock=CLOCK     select clock\n"
	       "                           0 = CLOCK_MONOTONIC (default)\n"
	       "                           1 = CLOCK_REALTIME\n"
	       "-C       --context         context switch tracing (used with -b)\n"
	       "-d DIST  --distance=DIST   distance of thread intervals in us default=500\n"
	       "-D       --duration=t      specify a length for the test run\n"
	       "                           default is in seconds, but 'm', 'h', or 'd' maybe added\n"
	       "                           to modify value to minutes, hours or days\n"
	       "	 --latency=PM_QOS  write PM_QOS to /dev/cpu_dma_latency\n"
	       "-E       --event           event tracing (used with -b)\n"
	       "-f       --ftrace          function trace (when -b is active)\n"
	       "-F       --fifo=<path>     create a named pipe at path and write stats to it\n"
	       "-h       --histogram=US    dump a latency histogram to stdout after the run\n"
	       "                           (with same priority about many threads)\n"
	       "                           US is the max time to be be tracked in microseconds\n"
	       "-H       --histofall=US    same as -h except with an additional summary column\n"
	       "-i INTV  --interval=INTV   base interval of thread in us default=1000\n"
	       "-I       --irqsoff         Irqsoff tracing (used with -b)\n"
	       "-l LOOPS --loops=LOOPS     number of loops: default=0(endless)\n"
	       "	 --laptop	   Save battery when running cyclictest\n"
	       "			   This will give you poorer realtime results\n"
	       "			   but will not drain your battery so quickly\n"
	       "-m       --mlockall        lock current and future memory allocations\n"
	       "-M       --refresh_on_max  delay updating the screen until a new max latency is hit\n"
	       "-n       --nanosleep       use clock_nanosleep\n"
	       "	 --notrace	   suppress tracing\n"
	       "-N       --nsecs           print results in ns instead of us (default us)\n"
	       "-o RED   --oscope=RED      oscilloscope mode, reduce verbose output by RED\n"
	       "-O TOPT  --traceopt=TOPT   trace option\n"
	       "-p PRIO  --prio=PRIO       priority of highest prio thread\n"
	       "-P       --preemptoff      Preempt off tracing (used with -b)\n"
	       "-q       --quiet           print only a summary on exit\n"
	       "	 --priospread       spread priority levels starting at specified value\n"
	       "-r       --relative        use relative timer instead of absolute\n"
	       "-R       --resolution      check clock resolution, calling clock_gettime() many\n"
	       "                           times.  list of clock_gettime() values will be\n"
	       "                           reported with -X\n"
	       "         --secaligned [USEC] align thread wakeups to the next full second,\n"
	       "                           and apply the optional offset\n"
	       "-s       --system          use sys_nanosleep and sys_setitimer\n"
	       "-S       --smp             Standard SMP testing: options -a -t -n and\n"
	       "                           same priority of all threads\n"
	       "-t       --threads         one thread per available processor\n"
	       "-t [NUM] --threads=NUM     number of threads:\n"
	       "                           without NUM, threads = max_cpus\n"
	       "                           without -t default = 1\n"
	       "-T TRACE --tracer=TRACER   set tracing function\n"
	       "    configured tracers: %s\n"
	       "-u       --unbuffered      force unbuffered output for live processing\n"
#ifdef NUMA
	       "-U       --numa            Standard NUMA testing (similar to SMP option)\n"
	       "                           thread data structures allocated from local node\n"
#endif
	       "-v       --verbose         output values on stdout for statistics\n"
	       "                           format: n:c:v n=tasknum c=count v=value in us\n"
	       "-w       --wakeup          task wakeup tracing (used with -b)\n"
	       "-W       --wakeuprt        rt task wakeup tracing (used with -b)\n"
	       "	 --dbg_cyclictest  print info useful for debugging cyclictest\n"
	       "	 --policy=POLI     policy of realtime thread, POLI may be fifo(default) or rr\n"
	       "                           format: --policy=fifo(default) or --policy=rr\n",
	       tracers
		);
	if (error)
		exit(EXIT_FAILURE);
	exit(EXIT_SUCCESS);
}

void application_usage(void)
{
	display_help(0);
}

static int use_nanosleep;
static int timermode = TIMER_ABSTIME;
static int use_system;
static int priority;
static int policy = SCHED_OTHER;	/* default policy if not specified */
static int num_threads = 1;
static int max_cycles;
static int clocksel = 0;
static int quiet;
static int interval = DEFAULT_INTERVAL;
static int distance = -1;
static struct bitmask *affinity_mask = NULL;
static int smp = 0;

enum {
	AFFINITY_UNSPECIFIED,
	AFFINITY_SPECIFIED,
	AFFINITY_USEALL
};
static int setaffinity = AFFINITY_UNSPECIFIED;

static int clocksources[] = {
	CLOCK_MONOTONIC,
	CLOCK_REALTIME,
};

static unsigned int is_cpumask_zero(const struct bitmask *mask)
{
	return (rt_numa_bitmask_count(mask) == 0);
}

static int cpu_for_thread(int thread_num, int max_cpus)
{
	unsigned int m, cpu, i, num_cpus;
	num_cpus = rt_numa_bitmask_count(affinity_mask);

	m = thread_num % num_cpus;

	/* there are num_cpus bits set, we want position of m'th one */
	for (i = 0, cpu = 0; i < max_cpus; i++) {
		if (rt_numa_bitmask_isbitset(affinity_mask, i)) {
			if (cpu == m)
				return i;
			cpu++;
		}
	}
	fprintf(stderr, "Bug in cpu mask handling code.\n");
	return 0;
}


static void parse_cpumask(const char *option, const int max_cpus)
{
	affinity_mask = rt_numa_parse_cpustring(option, max_cpus);
	if (affinity_mask) {
		if (is_cpumask_zero(affinity_mask)) {
			rt_bitmask_free(affinity_mask);
			affinity_mask = NULL;
		}
	}
	if (!affinity_mask)
		display_help(1);

	if (verbose) {
		printf("%s: Using %u cpus.\n", __func__,
			rt_numa_bitmask_count(affinity_mask));
	}
}


static void handlepolicy(char *polname)
{
	if (strncasecmp(polname, "other", 5) == 0)
		policy = SCHED_OTHER;
	else if (strncasecmp(polname, "batch", 5) == 0)
		policy = SCHED_BATCH;
	else if (strncasecmp(polname, "idle", 4) == 0)
		policy = SCHED_IDLE;
	else if (strncasecmp(polname, "fifo", 4) == 0)
		policy = SCHED_FIFO;
	else if (strncasecmp(polname, "rr", 2) == 0)
		policy = SCHED_RR;
	else	/* default policy if we don't recognize the request */
		policy = SCHED_OTHER;
}

static char *policyname(int policy)
{
	char *policystr = "";

	switch(policy) {
	case SCHED_OTHER:
		policystr = "other";
		break;
	case SCHED_FIFO:
		policystr = "fifo";
		break;
	case SCHED_RR:
		policystr = "rr";
		break;
	case SCHED_BATCH:
		policystr = "batch";
		break;
	case SCHED_IDLE:
		policystr = "idle";
		break;
	}
	return policystr;
}


enum option_values {
	OPT_AFFINITY=1, OPT_NOTRACE, OPT_BREAKTRACE, OPT_PREEMPTIRQ, OPT_CLOCK,
	OPT_CONTEXT, OPT_DISTANCE, OPT_DURATION, OPT_LATENCY, OPT_EVENT,
	OPT_FTRACE, OPT_FIFO, OPT_HISTOGRAM, OPT_HISTOFALL, OPT_INTERVAL,
	OPT_IRQSOFF, OPT_LOOPS, OPT_MLOCKALL, OPT_REFRESH, OPT_NANOSLEEP,
	OPT_NSECS, OPT_OSCOPE, OPT_TRACEOPT, OPT_PRIORITY, OPT_PREEMPTOFF,
	OPT_QUIET, OPT_PRIOSPREAD, OPT_RELATIVE, OPT_RESOLUTION, OPT_SYSTEM,
	OPT_SMP, OPT_THREADS, OPT_TRACER, OPT_UNBUFFERED, OPT_NUMA, OPT_VERBOSE,
	OPT_WAKEUP, OPT_WAKEUPRT, OPT_DBGCYCLIC, OPT_POLICY, OPT_HELP, OPT_NUMOPTS,
	OPT_ALIGNED, OPT_LAPTOP, OPT_SECALIGNED,
};

/* Process commandline options */
static void process_options (int argc, char *argv[], int max_cpus)
{
	int error = 0;
	int option_affinity = 0;

	for (;;) {
		int option_index = 0;
		/*
		 * Options for getopt
		 * Ordered alphabetically by single letter name
		 */
		static struct option long_options[] = {
			{"affinity",         optional_argument, NULL, OPT_AFFINITY},
			{"notrace",          no_argument,       NULL, OPT_NOTRACE },
			{"aligned",          optional_argument, NULL, OPT_ALIGNED },
			{"breaktrace",       required_argument, NULL, OPT_BREAKTRACE },
			{"preemptirqs",      no_argument,       NULL, OPT_PREEMPTIRQ },
			{"clock",            required_argument, NULL, OPT_CLOCK },
			{"context",          no_argument,       NULL, OPT_CONTEXT },
			{"distance",         required_argument, NULL, OPT_DISTANCE },
			{"duration",         required_argument, NULL, OPT_DURATION },
			{"latency",          required_argument, NULL, OPT_LATENCY },
			{"event",            no_argument,       NULL, OPT_EVENT },
			{"ftrace",           no_argument,       NULL, OPT_FTRACE },
			{"fifo",             required_argument, NULL, OPT_FIFO },
			{"histogram",        required_argument, NULL, OPT_HISTOGRAM },
			{"histofall",        required_argument, NULL, OPT_HISTOFALL },
			{"interval",         required_argument, NULL, OPT_INTERVAL },
			{"irqsoff",          no_argument,       NULL, OPT_IRQSOFF },
			{"laptop",	     no_argument,	NULL, OPT_LAPTOP },
			{"loops",            required_argument, NULL, OPT_LOOPS },
			{"mlockall",         no_argument,       NULL, OPT_MLOCKALL },
			{"refresh_on_max",   no_argument,       NULL, OPT_REFRESH },
			{"nanosleep",        no_argument,       NULL, OPT_NANOSLEEP },
			{"nsecs",            no_argument,       NULL, OPT_NSECS },
			{"oscope",           required_argument, NULL, OPT_OSCOPE },
			{"traceopt",         required_argument, NULL, OPT_TRACEOPT },
			{"priority",         required_argument, NULL, OPT_PRIORITY },
			{"preemptoff",       no_argument,       NULL, OPT_PREEMPTOFF },
			{"quiet",            no_argument,       NULL, OPT_QUIET },
			{"priospread",       no_argument,       NULL, OPT_PRIOSPREAD },
			{"relative",         no_argument,       NULL, OPT_RELATIVE },
			{"resolution",       no_argument,       NULL, OPT_RESOLUTION },
			{"secaligned",       optional_argument, NULL, OPT_SECALIGNED },
			{"system",           no_argument,       NULL, OPT_SYSTEM },
			{"smp",              no_argument,       NULL, OPT_SMP },
			{"threads",          optional_argument, NULL, OPT_THREADS },
			{"tracer",           required_argument, NULL, OPT_TRACER },
			{"unbuffered",       no_argument,       NULL, OPT_UNBUFFERED },
			{"numa",             no_argument,       NULL, OPT_NUMA },
			{"verbose",          no_argument,       NULL, OPT_VERBOSE },
			{"wakeup",           no_argument,       NULL, OPT_WAKEUP },
			{"wakeuprt",         no_argument,       NULL, OPT_WAKEUPRT },
			{"dbg_cyclictest",   no_argument,       NULL, OPT_DBGCYCLIC },
			{"policy",           required_argument, NULL, OPT_POLICY },
			{"help",             no_argument,       NULL, OPT_HELP },
			{NULL, 0, NULL, 0}
		};
		int c = getopt_long(argc, argv, "a::A::b:Bc:Cd:D:Efh:H:i:Il:MnNo:O:p:PmqrRsSt::uUvD:wWT:",
				    long_options, &option_index);
		if (c == -1)
			break;
		switch (c) {
		case 'a':
		case OPT_AFFINITY:
			option_affinity = 1;
			if (smp || numa)
				break;
			if (optarg != NULL) {
				parse_cpumask(optarg, max_cpus);
				setaffinity = AFFINITY_SPECIFIED;
			} else if (optind<argc && atoi(argv[optind])) {
				parse_cpumask(argv[optind], max_cpus);
				setaffinity = AFFINITY_SPECIFIED;
			} else {
				setaffinity = AFFINITY_USEALL;
			}
			break;
		case 'A':
		case OPT_ALIGNED:
			aligned=1;
			if (optarg != NULL)
				offset = atoi(optarg) * 1000;
			else if (optind<argc && atoi(argv[optind]))
				offset = atoi(argv[optind]) * 1000;
			else
				offset = 0;
			break;
		case 'b':
		case OPT_BREAKTRACE:
			tracelimit = atoi(optarg); break;
		case 'B':
		case OPT_PREEMPTIRQ:
			tracetype = PREEMPTIRQSOFF; break;
		case 'c':
		case OPT_CLOCK:
			clocksel = atoi(optarg); break;
		case 'C':
		case OPT_CONTEXT:
			tracetype = CTXTSWITCH; break;
		case 'd':
		case OPT_DISTANCE:
			distance = atoi(optarg); break;
		case 'D':
		case OPT_DURATION:
			duration = parse_time_string(optarg); break;
		case 'E':
		case OPT_EVENT:
			enable_events = 1; break;
		case 'f':
		case OPT_FTRACE:
			tracetype = FUNCTION; ftrace = 1; break;
		case 'F':
		case OPT_FIFO:
			use_fifo = 1;
			strncpy(fifopath, optarg, strlen(optarg));
			break;

		case 'H':
		case OPT_HISTOFALL:
			histofall = 1; /* fall through */
		case 'h':
		case OPT_HISTOGRAM:
			histogram = atoi(optarg); break;
		case 'i':
		case OPT_INTERVAL:
			interval = atoi(optarg); break;
		case 'I':
		case OPT_IRQSOFF:
			if (tracetype == PREEMPTOFF) {
				tracetype = PREEMPTIRQSOFF;
				strncpy(tracer, "preemptirqsoff", sizeof(tracer));
			} else {
				tracetype = IRQSOFF;
				strncpy(tracer, "irqsoff", sizeof(tracer));
			}
			break;
		case 'l':
		case OPT_LOOPS:
			max_cycles = atoi(optarg); break;
		case 'm':
		case OPT_MLOCKALL:
			lockall = 1; break;
		case 'M':
		case OPT_REFRESH:
			refresh_on_max = 1; break;
		case 'n':
		case OPT_NANOSLEEP:
			use_nanosleep = MODE_CLOCK_NANOSLEEP; break;
		case 'N':
		case OPT_NSECS:
			use_nsecs = 1; break;
		case 'o':
		case OPT_OSCOPE:
			oscope_reduction = atoi(optarg); break;
		case 'O':
		case OPT_TRACEOPT:
			traceopt(optarg); break;
		case 'p':
		case OPT_PRIORITY:
			priority = atoi(optarg);
			if (policy != SCHED_FIFO && policy != SCHED_RR)
				policy = SCHED_FIFO;
			break;
		case 'P':
		case OPT_PREEMPTOFF:
			if (tracetype == IRQSOFF) {
				tracetype = PREEMPTIRQSOFF;
				strncpy(tracer, "preemptirqsoff", sizeof(tracer));
			} else {
				tracetype = PREEMPTOFF;
				strncpy(tracer, "preemptoff", sizeof(tracer));
			}
			break;
		case 'q':
		case OPT_QUIET:
			quiet = 1; break;
		case 'r':
		case OPT_RELATIVE:
			timermode = TIMER_RELTIME; break;
		case 'R':
		case OPT_RESOLUTION:
			check_clock_resolution = 1; break;
		case 's':
		case OPT_SECALIGNED:
			secaligned = 1;
			if (optarg != NULL)
				offset = atoi(optarg) * 1000;
			else if (optind < argc && atoi(argv[optind]))
				offset = atoi(argv[optind]) * 1000;
			else
				offset = 0;
			break;
		case OPT_SYSTEM:
			use_system = MODE_SYS_OFFSET; break;
		case 'S':
		case OPT_SMP: /* SMP testing */
			if (numa)
				fatal("numa and smp options are mutually exclusive\n");
			smp = 1;
			num_threads = max_cpus;
			setaffinity = AFFINITY_USEALL;
			use_nanosleep = MODE_CLOCK_NANOSLEEP;
			break;
		case 't':
		case OPT_THREADS:
			if (smp) {
				warn("-t ignored due to --smp\n");
				break;
			}
			if (optarg != NULL)
				num_threads = atoi(optarg);
			else if (optind<argc && atoi(argv[optind]))
				num_threads = atoi(argv[optind]);
			else
				num_threads = max_cpus;
			break;
		case 'T':
		case OPT_TRACER:
			tracetype = CUSTOM;
			strncpy(tracer, optarg, sizeof(tracer));
			break;
		case 'u':
		case OPT_UNBUFFERED:
			setvbuf(stdout, NULL, _IONBF, 0); break;
		case 'U':
		case OPT_NUMA: /* NUMA testing */
			if (smp)
				fatal("numa and smp options are mutually exclusive\n");
#ifdef NUMA
			if (numa_available() == -1)
				fatal("NUMA functionality not available!");
			numa = 1;
			num_threads = max_cpus;
			setaffinity = AFFINITY_USEALL;
			use_nanosleep = MODE_CLOCK_NANOSLEEP;
#else
			warn("cyclictest was not built with the numa option\n");
			warn("ignoring --numa or -U\n");
#endif
			break;
		case 'v':
		case OPT_VERBOSE: verbose = 1; break;
		case 'w':
		case OPT_WAKEUP:
			tracetype = WAKEUP; break;
		case 'W':
		case OPT_WAKEUPRT:
			tracetype = WAKEUPRT; break;
		case '?':
		case OPT_HELP:
			display_help(0); break;

		/* long only options */
		case OPT_PRIOSPREAD:
			priospread = 1; break;
		case OPT_LATENCY:
                          /* power management latency target value */
			  /* note: default is 0 (zero) */
			latency_target_value = atoi(optarg);
			if (latency_target_value < 0)
				latency_target_value = 0;
			break;
		case OPT_NOTRACE:
			notrace = 1; break;
		case OPT_POLICY:
			handlepolicy(optarg); break;
		case OPT_DBGCYCLIC:
			ct_debug = 1; break;
		case OPT_LAPTOP:
			laptop = 1; break;
		}
	}

	if (option_affinity) {
		if (smp) {
			warn("-a ignored due to --smp\n");
		} else if (numa) {
			warn("-a ignored due to --numa\n");
		}
	}

	if (tracelimit)
		fileprefix = procfileprefix;

	if (clocksel < 0 || clocksel > ARRAY_SIZE(clocksources))
		error = 1;

	if (oscope_reduction < 1)
		error = 1;

	if (oscope_reduction > 1 && !verbose) {
		warn("-o option only meaningful, if verbose\n");
		error = 1;
	}

	if (histogram < 0)
		error = 1;

	if (histogram > HIST_MAX)
		histogram = HIST_MAX;

	if (histogram && distance != -1)
		warn("distance is ignored and set to 0, if histogram enabled\n");
	if (distance == -1)
		distance = DEFAULT_DISTANCE;

	if (priority < 0 || priority > 99)
		error = 1;

	if (priospread && priority == 0) {
		fprintf(stderr, "defaulting realtime priority to %d\n",
			num_threads+1);
		priority = num_threads+1;
	}

	if (priority && (policy != SCHED_FIFO && policy != SCHED_RR)) {
		fprintf(stderr, "policy and priority don't match: setting policy to SCHED_FIFO\n");
		policy = SCHED_FIFO;
	}

	if ((policy == SCHED_FIFO || policy == SCHED_RR) && priority == 0) {
		fprintf(stderr, "defaulting realtime priority to %d\n",
			num_threads+1);
		priority = num_threads+1;
	}

	if (num_threads < 1)
		error = 1;

	if (aligned && secaligned)
		error = 1;

	if (aligned || secaligned) {
		barrier_init(&globalt_barr, num_threads);
		barrier_init(&align_barr, num_threads);
	}

	if (error) {
		if (affinity_mask)
			rt_bitmask_free(affinity_mask);
		display_help(1);
	}
}

static int check_kernel(void)
{
	struct utsname kname;
	int maj, min, sub, kv, ret;

	ret = uname(&kname);
	if (ret) {
		fprintf(stderr, "uname failed: %s. Assuming not 2.6\n",
				strerror(errno));
		return KV_NOT_SUPPORTED;
	}
	sscanf(kname.release, "%d.%d.%d", &maj, &min, &sub);
	if (maj == 2 && min == 6) {
		if (sub < 18)
			kv = KV_26_LT18;
		else if (sub < 24)
			kv = KV_26_LT24;
		else if (sub < 28) {
			kv = KV_26_33;
			strcpy(functiontracer, "ftrace");
			strcpy(traceroptions, "iter_ctrl");
		} else {
			kv = KV_26_33;
			strcpy(functiontracer, "function");
			strcpy(traceroptions, "trace_options");
		}
	} else if (maj >= 3) {
		kv = KV_30;
		strcpy(functiontracer, "function");
		strcpy(traceroptions, "trace_options");

	} else
		kv = KV_NOT_SUPPORTED;

	return kv;
}

static int check_timer(void)
{
	struct timespec ts;

	if (clock_getres(CLOCK_MONOTONIC, &ts))
		return 1;

	return (ts.tv_sec != 0 || ts.tv_nsec != 1);
}

static void sighand(int sig)
{
	if (sig == SIGUSR1) {
		int i;
		int oldquiet = quiet;

		quiet = 0;
		fprintf(stderr, "#---------------------------\n");
		fprintf(stderr, "# cyclictest current status:\n");
		for (i = 0; i < num_threads; i++)
			print_stat(stderr, parameters[i], i, 0, 0);
		fprintf(stderr, "#---------------------------\n");
		quiet = oldquiet;
		return;
	}
	shutdown = 1;
	if (refresh_on_max)
		pthread_cond_signal(&refresh_on_max_cond);
	if (tracelimit && !notrace)
		tracing(0);
}

static void print_tids(struct thread_param *par[], int nthreads)
{
	int i;

	printf("# Thread Ids:");
	for (i = 0; i < nthreads; i++)
		printf(" %05d", par[i]->stats->tid);
	printf("\n");
}

static void print_hist(struct thread_param *par[], int nthreads)
{
	int i, j;
	unsigned long long int log_entries[nthreads+1];
	unsigned long maxmax, alloverflows;

	bzero(log_entries, sizeof(log_entries));

	printf("# Histogram\n");
	for (i = 0; i < histogram; i++) {
		unsigned long long int allthreads = 0;

		printf("%06d ", i);

		for (j = 0; j < nthreads; j++) {
			unsigned long curr_latency=par[j]->stats->hist_array[i];
			printf("%06lu", curr_latency);
			if (j < nthreads - 1)
				printf("\t");
			log_entries[j] += curr_latency;
			allthreads += curr_latency;
		}
		if (histofall && nthreads > 1) {
			printf("\t%06llu", allthreads);
			log_entries[nthreads] += allthreads;
		}
		printf("\n");
	}
	printf("# Total:");
	for (j = 0; j < nthreads; j++)
		printf(" %09llu", log_entries[j]);
	if (histofall && nthreads > 1)
		printf(" %09llu", log_entries[nthreads]);
	printf("\n");
	printf("# Min Latencies:");
	for (j = 0; j < nthreads; j++)
		printf(" %05lu", par[j]->stats->min);
	printf("\n");
	printf("# Avg Latencies:");
	for (j = 0; j < nthreads; j++)
		printf(" %05lu", par[j]->stats->cycles ?
		       (long)(par[j]->stats->avg/par[j]->stats->cycles) : 0);
	printf("\n");
	printf("# Max Latencies:");
	maxmax = 0;
	for (j = 0; j < nthreads; j++) {
		printf(" %05lu", par[j]->stats->max);
		if (par[j]->stats->max > maxmax)
			maxmax = par[j]->stats->max;
	}
	if (histofall && nthreads > 1)
		printf(" %05lu", maxmax);
	printf("\n");
	printf("# Histogram Overflows:");
	alloverflows = 0;
	for (j = 0; j < nthreads; j++) {
		printf(" %05lu", par[j]->stats->hist_overflow);
		alloverflows += par[j]->stats->hist_overflow;
	}
	if (histofall && nthreads > 1)
		printf(" %05lu", alloverflows);
	printf("\n");

	printf("# Histogram Overflow at cycle number:\n");
	for (i = 0; i < nthreads; i++) {
		printf("# Thread %d:", i);
		for (j = 0; j < par[i]->stats->num_outliers; j++)
			printf(" %05lu", par[i]->stats->outliers[j]);
		if (par[i]->stats->num_outliers < par[i]->stats->hist_overflow)
			printf(" # %05lu others", par[i]->stats->hist_overflow - par[i]->stats->num_outliers);
		printf("\n");
	}
	printf("\n");
}

static void print_stat(FILE *fp, struct thread_param *par, int index, int verbose, int quiet)
{
	struct thread_stat *stat = par->stats;

	if (!verbose) {
		if (quiet != 1) {
			char *fmt;
			if (use_nsecs)
				fmt = "T:%2d (%5d) P:%2d I:%ld C:%7lu "
					"Min:%7ld Act:%8ld Avg:%8ld Max:%8ld\n";
			else
				fmt = "T:%2d (%5d) P:%2d I:%ld C:%7lu "
					"Min:%7ld Act:%5ld Avg:%5ld Max:%8ld\n";
			fprintf(fp, fmt, index, stat->tid, par->prio,
				par->interval, stat->cycles, stat->min, stat->act,
				stat->cycles ?
				(long)(stat->avg/stat->cycles) : 0, stat->max);
		}
	} else {
		while (stat->cycles != stat->cyclesread) {
			long diff = stat->values
			    [stat->cyclesread & par->bufmsk];

			if (diff > stat->redmax) {
				stat->redmax = diff;
				stat->cycleofmax = stat->cyclesread;
			}
			if (++stat->reduce == oscope_reduction) {
				fprintf(fp, "%8d:%8lu:%8ld\n", index,
					stat->cycleofmax, stat->redmax);
				stat->reduce = 0;
				stat->redmax = 0;
			}
			stat->cyclesread++;
		}
	}
}


/*
 * thread that creates a named fifo and hands out run stats when someone
 * reads from the fifo.
 */
void *fifothread(void *param)
{
	int ret;
	int fd;
	FILE *fp;
	int i;

	if (use_fifo == 0)
		return NULL;

	unlink(fifopath);
	ret = mkfifo(fifopath, 0666);
	if (ret) {
		fprintf(stderr, "Error creating fifo %s: %s\n", fifopath, strerror(errno));
		return NULL;
	}
	while (!shutdown) {
		fd = open(fifopath, O_WRONLY|O_NONBLOCK);
		if (fd < 0) {
			usleep(500000);
			continue;
		}
		fp = fdopen(fd, "w");
		for (i=0; i < num_threads; i++)
			print_stat(fp, parameters[i], i, 0, 0);
		fclose(fp);
		usleep(250);
	}
	unlink(fifopath);
	return NULL;
}


int main(int argc, char **argv)
{
	sigset_t sigset;
	int signum = SIGALRM;
	int mode;
	int max_cpus = sysconf(_SC_NPROCESSORS_ONLN);
	int i, ret = -1;
	int status;

	process_options(argc, argv, max_cpus);

	if (check_privs())
		exit(EXIT_FAILURE);

	if (verbose)
		printf("Max CPUs = %d\n", max_cpus);

	/* Checks if numa is on, program exits if numa on but not available */
	numa_on_and_available();

	/* lock all memory (prevent swapping) */
	if (lockall)
		if (mlockall(MCL_CURRENT|MCL_FUTURE) == -1) {
			perror("mlockall");
			goto out;
		}

	/* use the /dev/cpu_dma_latency trick if it's there */
	set_latency_target();

	kernelversion = check_kernel();

	if (kernelversion == KV_NOT_SUPPORTED)
		warn("Running on unknown kernel version...YMMV\n");

	setup_tracer();

	if (check_timer())
		warn("High resolution timers not available\n");

	if (check_clock_resolution) {
		int clock;
		uint64_t diff;
		int k;
		uint64_t min_non_zero_diff = UINT64_MAX;
		struct timespec now;
		struct timespec prev;
		uint64_t reported_resolution = UINT64_MAX;
		struct timespec res;
		struct timespec *time;
		int times;

		clock = clocksources[clocksel];

		if (clock_getres(clock, &res)) {
			warn("clock_getres failed");
		} else {
			reported_resolution = (NSEC_PER_SEC * res.tv_sec) + res.tv_nsec;
		}


		/*
		 * Calculate how many calls to clock_gettime are needed.
		 * Then call it that many times.
		 * Goal is to collect timestamps for ~ 0.001 sec.
		 * This will reliably capture resolution <= 500 usec.
		 */
		times = 1000;
		clock_gettime(clock, &prev);
		for (k=0; k < times; k++) {
			clock_gettime(clock, &now);
		}

		diff = calcdiff_ns(now, prev);
		if (diff == 0) {
			/*
			 * No clock rollover occurred.
			 * Use the default value for times.
			 */
			times = -1;
		} else {
			int call_time;
			call_time = diff / times;         /* duration 1 call */
			times = NSEC_PER_SEC / call_time; /* calls per second */
			times /= 1000;                    /* calls per msec */
			if (times < 1000)
				times = 1000;
		}
		/* sanity check */
		if ((times <= 0) || (times > 100000))
			times = 100000;

		time = calloc(times, sizeof(*time));

		for (k=0; k < times; k++) {
			clock_gettime(clock, &time[k]);
		}

		if (ct_debug) {
			info("For %d consecutive calls to clock_gettime():\n", times);
			info("time, delta time (nsec)\n");
		}

		prev = time[0];
		for (k=1; k < times; k++) {

			diff = calcdiff_ns(time[k], prev);
			prev = time[k];

			if (diff && (diff < min_non_zero_diff)) {
				min_non_zero_diff = diff;
			}

			if (ct_debug)
				info("%ld.%06ld  %5llu\n",
				     time[k].tv_sec, time[k].tv_nsec,
				     (unsigned long long)diff);
		}

		free(time);


		if (verbose ||
		    (min_non_zero_diff && (min_non_zero_diff > reported_resolution))) {
			/*
			 * Measured clock resolution includes the time to call
			 * clock_gettime(), so it will be slightly larger than
			 * actual resolution.
			 */
			warn("reported clock resolution: %llu nsec\n",
			     (unsigned long long)reported_resolution);
			warn("measured clock resolution approximately: %llu nsec\n",
			     (unsigned long long)min_non_zero_diff);
		}

	}

	mode = use_nanosleep + use_system;

	sigemptyset(&sigset);
	sigaddset(&sigset, signum);
	sigprocmask (SIG_BLOCK, &sigset, NULL);

	signal(SIGINT, sighand);
	signal(SIGTERM, sighand);
	signal(SIGUSR1, sighand);

	parameters = calloc(num_threads, sizeof(struct thread_param *));
	if (!parameters)
		goto out;
	statistics = calloc(num_threads, sizeof(struct thread_stat *));
	if (!statistics)
		goto outpar;

	for (i = 0; i < num_threads; i++) {
		pthread_attr_t attr;
		int node;
		struct thread_param *par;
		struct thread_stat *stat;

		status = pthread_attr_init(&attr);
		if (status != 0)
			fatal("error from pthread_attr_init for thread %d: %s\n", i, strerror(status));

		node = -1;
		if (numa) {
			void *stack;
			void *currstk;
			size_t stksize;

			/* find the memory node associated with the cpu i */
			node = rt_numa_numa_node_of_cpu(i);

			/* get the stack size set for for this thread */
			if (pthread_attr_getstack(&attr, &currstk, &stksize))
				fatal("failed to get stack size for thread %d\n", i);

			/* if the stack size is zero, set a default */
			if (stksize == 0)
				stksize = PTHREAD_STACK_MIN * 2;

			/*  allocate memory for a stack on appropriate node */
			stack = rt_numa_numa_alloc_onnode(stksize, node, i);

			/* set the thread's stack */
			if (pthread_attr_setstack(&attr, stack, stksize))
				fatal("failed to set stack addr for thread %d to 0x%x\n",
				      i, stack+stksize);
		}

		/* allocate the thread's parameter block  */
		parameters[i] = par = threadalloc(sizeof(struct thread_param), node);
		if (par == NULL)
			fatal("error allocating thread_param struct for thread %d\n", i);
		memset(par, 0, sizeof(struct thread_param));

		/* allocate the thread's statistics block */
		statistics[i] = stat = threadalloc(sizeof(struct thread_stat), node);
		if (stat == NULL)
			fatal("error allocating thread status struct for thread %d\n", i);
		memset(stat, 0, sizeof(struct thread_stat));

		/* allocate the histogram if requested */
		if (histogram) {
			int bufsize = histogram * sizeof(long);

			stat->hist_array = threadalloc(bufsize, node);
			stat->outliers = threadalloc(bufsize, node);
			if (stat->hist_array == NULL || stat->outliers == NULL)
				fatal("failed to allocate histogram of size %d on node %d\n",
				      histogram, i);
			memset(stat->hist_array, 0, bufsize);
			memset(stat->outliers, 0, bufsize);
		}

		if (verbose) {
			int bufsize = VALBUF_SIZE * sizeof(long);
			stat->values = threadalloc(bufsize, node);
			if (!stat->values)
				goto outall;
			memset(stat->values, 0, bufsize);
			par->bufmsk = VALBUF_SIZE - 1;
		}

		par->prio = priority;
		if (priority && (policy == SCHED_FIFO || policy == SCHED_RR))
			par->policy = policy;
		else {
			par->policy = SCHED_OTHER;
			force_sched_other = 1;
		}
		if (priospread)
			priority--;
		par->clock = clocksources[clocksel];
		par->mode = mode;
		par->timermode = timermode;
		par->signal = signum;
		par->interval = interval;
		if (!histogram) /* same interval on CPUs */
			interval += distance;
		if (verbose)
			printf("Thread %d Interval: %d\n", i, interval);
		par->max_cycles = max_cycles;
		par->stats = stat;
		par->node = node;
		par->tnum = i;
		switch (setaffinity) {
		case AFFINITY_UNSPECIFIED: par->cpu = -1; break;
		case AFFINITY_SPECIFIED:
			par->cpu = cpu_for_thread(i, max_cpus);
			if (verbose)
				printf("Thread %d using cpu %d.\n", i,
					par->cpu);
			break;
		case AFFINITY_USEALL: par->cpu = i % max_cpus; break;
		}
		stat->min = 1000000;
		stat->max = 0;
		stat->avg = 0.0;
		stat->threadstarted = 1;
		status = pthread_create(&stat->thread, &attr, timerthread, par);
		if (status)
			fatal("failed to create thread %d: %s\n", i, strerror(status));

	}
	if (use_fifo)
		status = pthread_create(&fifo_threadid, NULL, fifothread, NULL);

	while (!shutdown) {
		char lavg[256];
		int fd, len, allstopped = 0;
		static char *policystr = NULL;
		static char *slash = NULL;
		static char *policystr2;

		if (!policystr)
			policystr = policyname(policy);

		if (!slash) {
			if (force_sched_other) {
				slash = "/";
				policystr2 = policyname(SCHED_OTHER);
			} else
				slash = policystr2 = "";
		}
		if (!verbose && !quiet) {
			fd = open("/proc/loadavg", O_RDONLY, 0666);
			len = read(fd, &lavg, 255);
			close(fd);
			lavg[len-1] = 0x0;
			printf("policy: %s%s%s: loadavg: %s          \n\n",
			       policystr, slash, policystr2, lavg);
		}

		for (i = 0; i < num_threads; i++) {

			print_stat(stdout, parameters[i], i, verbose, quiet);
			if(max_cycles && statistics[i]->cycles >= max_cycles)
				allstopped++;
		}

		usleep(10000);
		if (shutdown || allstopped)
			break;
		if (!verbose && !quiet)
			printf("\033[%dA", num_threads + 2);

		if (refresh_on_max) {
			pthread_mutex_lock(&refresh_on_max_lock);
			pthread_cond_wait(&refresh_on_max_cond,
					  &refresh_on_max_lock);
			pthread_mutex_unlock(&refresh_on_max_lock);
		}
	}
	ret = EXIT_SUCCESS;

 outall:
	shutdown = 1;
	usleep(50000);

	if (quiet)
		quiet = 2;
	for (i = 0; i < num_threads; i++) {
		if (statistics[i]->threadstarted > 0)
			pthread_kill(statistics[i]->thread, SIGTERM);
		if (statistics[i]->threadstarted) {
			pthread_join(statistics[i]->thread, NULL);
			if (quiet && !histogram)
				print_stat(stdout, parameters[i], i, 0, 0);
		}
		if (statistics[i]->values)
			threadfree(statistics[i]->values, VALBUF_SIZE*sizeof(long), parameters[i]->node);
	}

	if (histogram) {
		print_hist(parameters, num_threads);
		for (i = 0; i < num_threads; i++) {
			threadfree(statistics[i]->hist_array, histogram*sizeof(long), parameters[i]->node);
			threadfree(statistics[i]->outliers, histogram*sizeof(long), parameters[i]->node);
		}
	}

	if (tracelimit) {
		print_tids(parameters, num_threads);
		if (break_thread_id) {
			printf("# Break thread: %d\n", break_thread_id);
			printf("# Break value: %llu\n", (unsigned long long)break_thread_value);
		}
	}


	for (i=0; i < num_threads; i++) {
		if (!statistics[i])
			continue;
		threadfree(statistics[i], sizeof(struct thread_stat), parameters[i]->node);
	}

 outpar:
	for (i = 0; i < num_threads; i++) {
		if (!parameters[i])
			continue;
		threadfree(parameters[i], sizeof(struct thread_param), parameters[i]->node);
	}
 out:
	/* ensure that the tracer is stopped */
	if (tracelimit && !notrace)
		tracing(0);


	/* close any tracer file descriptors */
	if (tracemark_fd >= 0)
		close(tracemark_fd);
	if (trace_fd >= 0)
		close(trace_fd);

	if (enable_events)
		/* turn off all events */
		event_disable_all();

	/* turn off the function tracer */
	fileprefix = procfileprefix;
	if (tracetype && !notrace)
		setkernvar("ftrace_enabled", "0");
	fileprefix = get_debugfileprefix();

	/* unlock everything */
	if (lockall)
		munlockall();

	/* Be a nice program, cleanup */
	if (kernelversion < KV_26_33)
		restorekernvars();

	/* close the latency_target_fd if it's open */
	if (latency_target_fd >= 0)
		close(latency_target_fd);

	if (affinity_mask)
		rt_bitmask_free(affinity_mask);

	exit(ret);
}
