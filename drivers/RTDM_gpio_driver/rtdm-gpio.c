#include <linux/fs.h>
#include <linux/gpio.h>
#include <linux/interrupt.h>
#include <linux/module.h>

#include <cobalt/kernel/rtdm/driver.h>

// PIN29 -> GPIO5
#define GPIO_IN  1999
// PIN31 -> GPIO6
#define GPIO_OUT 2000

static rtdm_irq_t irq_rtdm;

static int handler(rtdm_irq_t *irq)
{
    	static int value = 0;
    	value = gpio_get_value(GPIO_IN);
    	trace_printk("[xenomai] GPIO value :%d\n", value);
    	printk(KERN_INFO "[xenomai] GPIO value :%d\n", value);
    	gpio_set_value(GPIO_OUT, value);
    	return RTDM_IRQ_HANDLED;
}

static int __init example_init (void)
{
    	int ret = 0;
    	int irq;
    	printk(KERN_INFO "Initializing driver...\n");

    	irq = gpio_to_irq(GPIO_IN);

    	if ((ret = gpio_request(GPIO_IN, THIS_MODULE->name)) != 0) {
		printk(KERN_ERR "ERROR in gpio_request()\n");
		goto err;
    	}
    	if ((ret = gpio_direction_input(GPIO_IN)) != 0) {
		printk(KERN_ERR "ERROR in gpio_direction_input()\n");
		goto err_gpio_in;
    	}
    	if ((ret = gpio_request(GPIO_OUT, THIS_MODULE->name)) != 0) {
		printk(KERN_ERR "ERROR in gpio_request() 2\n");
		goto err_gpio_in;
    	}
    	if ((ret = gpio_direction_output(GPIO_OUT, 1)) != 0) {
		printk(KERN_ERR "ERROR in gpio_direction_output()\n");
		goto err_gpio_out;
    	}

    	irq_set_irq_type(irq, 0x00000003);

    	if ((ret = rtdm_irq_request(& irq_rtdm,
                     			irq, handler,
                     			RTDM_IRQTYPE_EDGE,
                     			THIS_MODULE->name, NULL)) != 0) {
		printk(KERN_ERR "ERROR in rtdm_irq_request()\n");
		goto err_gpio_out;
    	}

	return 0;

err_gpio_out:
        gpio_free(GPIO_OUT);

err_gpio_in:
        gpio_free(GPIO_IN);

err:
    	return ret;
}

static void __exit example_exit (void)
{
    	rtdm_irq_free(&irq_rtdm);
    	gpio_free(GPIO_OUT);
    	gpio_free(GPIO_IN);
}

module_init(example_init);
module_exit(example_exit);
MODULE_LICENSE("GPL");
