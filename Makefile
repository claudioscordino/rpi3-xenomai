export LINUX_DIR := $(PWD)/kernel/linux-4.9.51
export KERNEL_DIR := $(LINUX_DIR)/build/linux
export XENOMAI_DIR := $(PWD)/xenomai-3.0.6

.PHONY: clean kernel drivers tools tools_install configure

$(KERNEL_DIR)/.config:
	make -C $(LINUX_DIR) ARCH=arm O=build/linux multi_v7_defconfig
	cp -f kernel/kernel-config kernel/linux-4.9.51/build/linux/.config


configure: $(KERNEL_DIR)/.config
	make  -C $(LINUX_DIR) ARCH=arm O=build/linux menuconfig


kernel: $(KERNEL_DIR)/.config
	make  -C $(LINUX_DIR) ARCH=arm O=build/linux -j4 bzImage modules dtbs
	make  -C $(LINUX_DIR) ARCH=arm O=build/linux modules_install INSTALL_MOD_PATH=MODULES


drivers: kernel
	make -C drivers/RTDM_gpio_driver
	make -C drivers/RTDM_gpio_wave_driver
	make -C drivers/RTDM_timer_driver


tools:
	cd $(XENOMAI_DIR)
	$(XENOMAI_DIR)/configure --enable-smp --with-core=cobalt
	make -j4


tools_install:
	cd $(XENOMAI_DIR)
	make install


clean:
	make -C $(KERNEL_DIR) clean distclean
	make -C drivers/RTDM_gpio_driver clean
	make -C drivers/RTDM_gpio_wave_driver clean
	make -C drivers/RTDM_timer_driver clean

