Xenomai 3.0.6 on Raspberry Pi 3
===============================

This short guide explains how to put Xenomai 3 Cobalt on the Raspberry Pi 3.

System setup
------------

* Download Raspian from https://www.raspberrypi.org/downloads/raspbian/

* Use ```lsblk``` to discover the flash device. Then, flash the image through:

       dd bs=4M if=2018-04-18-raspbian-stretch.img of=/dev/sdX conv=fsync

* Credentials: pi/raspberry

* Run ```rpi-update```

* Run ```raspi-config``` for enabling SSH and serial console (in "Interfacing Options")


Kernel build
------------

The kernel has been already patched through

        xenomai-3.0.6/scripts/prepare-kernel.sh --arch=arm --ipipe=ipipe-core-4.9.51-arm-4.patch --linux=linux-4.9.51

To cross-compile the kernel follow the next steps:

        export CROSS_COMPILE=arm-linux-gnueabihf-

        cd linux-4.9.51

        make ARCH=arm O=build/linux multi_v7_defconfig

        cp -f ../config linux-4.9.51/build/linux/.config

        make ARCH=arm O=build/linux menuconfig

        make ARCH=arm O=build/linux -j4 bzImage modules dtbs

        make ARCH=arm O=build/linux modules_install INSTALL_MOD_PATH=MODULES


Kernel installation
-------------------

* Copy the following files to the target:

  * ```build/linux/arch/arm/boot/dts/bcm2837-rpi-3-b-cobalt.dtb``` -> ```/boot/```
  * ```build/linux/arch/arm/boot/zImage``` -> ```/boot/kernel-xenomai.img```
  * ```build/linux/MODULES/``` -> ```/lib/modules```

* Append to ```/boot/config.txt```:

        device_tree=bcm2837-rpi-3-b-cobalt.dtb
        kernel=kernel-xenomai.img

* Append to ```/boot/cmdline.txt```:

        cma=256M@512M


Drivers build
-------------

The exemplary drivers can be built by setting the following environment variables:

* ```CROSS_COMPILE```
* ```XENOMAI_DIR```
* ```KERNEL_DIR```


User-space tools
----------------

The Xenomai user-space tools can be natively built on the Rpi3 target as follows:

       cd xenomai-3.0.6

       ./configure --enable-smp --with-core=cobalt

       make -j4

       sudo make install

 To run the default benchmark, type

       sudo /usr/xenomai/bin/xeno-test


References
----------
* Xenomai installation guide: https://xenomai.org/installing-xenomai-3-x/
* I-pipe download: https://xenomai.org/downloads/ipipe/
* Xenomai download: https://xenomai.org/downloads/xenomai/stable/latest/
* I-pipe tracer: https://xenomai.org/2014/06/using-the-i-pipe-tracer/


