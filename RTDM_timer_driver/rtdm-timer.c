#include <linux/fs.h>
#include <linux/gpio.h>
#include <linux/interrupt.h>
#include <linux/module.h>

#include <cobalt/kernel/rtdm/driver.h>

#define PERIOD_NSEC	100000

static nanosecs_abs_t t1;
static rtdm_timer_t timer;

static void handler(rtdm_timer_t *timer)
{
	nanosecs_abs_t t2 = rtdm_clock_read_monotonic();
	int difference = ((int)(t2 -t1)) - PERIOD_NSEC;
	printk(KERN_INFO "[xenomai] Difference = %d nsec\n", difference);
	t1 = t2;
}

static int __init example_init (void)
{
	int ret = 0;

	ret = rtdm_timer_init(&timer, handler, "timer");
	if (ret !=0) {
		printk(KERN_ERR "ERROR in rtdm_timer_init()\n");
        	goto out;
	}

	ret = rtdm_timer_start (&timer, 0, PERIOD_NSEC, RTDM_TIMERMODE_RELATIVE);
	if (ret !=0) {
		printk(KERN_ERR "ERROR in rtdm_timer_start()\n");
    		rtdm_timer_destroy(&timer);
	}

out:
    	return ret;
}

static void __exit example_exit (void)
{
    	rtdm_timer_destroy(&timer);
}

module_init(example_init);
module_exit(example_exit);
MODULE_LICENSE("GPL");
