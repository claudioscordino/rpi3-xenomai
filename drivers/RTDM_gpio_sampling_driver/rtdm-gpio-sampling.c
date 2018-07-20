#include <linux/fs.h>
#include <linux/gpio.h>
#include <linux/interrupt.h>
#include <linux/module.h>

#include <cobalt/kernel/rtdm/driver.h>

#define PERIOD_NSEC	10000		// 10 usec
#define GPIO		1999		// PIN29 -> GPIO5

static rtdm_timer_t timer;
static int value = 0;

static void handler(rtdm_timer_t *timer)
{
    	value = gpio_get_value(GPIO);
	printk (KERN_ERR "gpio = %d\n", value);
}

static int __init example_init (void)
{
	int ret = 0;
	printk (KERN_ERR "GPIO sampling starting...\n");

    	if ((ret = gpio_request(GPIO, THIS_MODULE->name)) != 0) {
		printk(KERN_ERR "ERROR in gpio_request()\n");
        	goto err;
    	}

    	if ((ret = gpio_direction_input(GPIO)) != 0) {
		printk(KERN_ERR "ERROR in gpio_direction_input()\n");
        	goto err_gpio;
    	}

	ret = rtdm_timer_init(&timer, handler, "timer");
	if (ret !=0) {
		printk(KERN_ERR "ERROR in rtdm_timer_init()\n");
        	goto err_gpio;
	}

	ret = rtdm_timer_start (&timer, 0, PERIOD_NSEC, RTDM_TIMERMODE_RELATIVE);
	if (ret !=0) {
		printk(KERN_ERR "ERROR in rtdm_timer_start()\n");
        	goto err_timer;
	}

	return 0;

err_timer:
    	rtdm_timer_destroy(&timer);

err_gpio:
        gpio_free(GPIO);

err:
    	return ret;
}

static void __exit example_exit (void)
{
        gpio_free(GPIO);
    	rtdm_timer_destroy(&timer);
}

module_init(example_init);
module_exit(example_exit);
MODULE_AUTHOR("Claudio Scordino <claudio@evidence.eu.com>");
MODULE_LICENSE("GPL");
