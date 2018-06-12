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

To configure the kernel (optional) type:

        export CROSS_COMPILE=arm-linux-gnueabihf- (or your specific toolchain)

        make configure

To cross-compile the kernel follow the next steps:

        export CROSS_COMPILE=arm-linux-gnueabihf- (or your specific toolchain)

        make kernel

Note that the kernel has been already patched through

        xenomai-3.0.6/scripts/prepare-kernel.sh --arch=arm --ipipe=ipipe-core-4.9.51-arm-4.patch --linux=linux-4.9.51

Note that the initial default kernel configuration is stored in the ```kernel/kernel-config``` file.


Kernel installation
-------------------

* Copy the following files to the target:

  * ```kernel/linux-4.9.51/build/linux/arch/arm/boot/dts/bcm2837-rpi-3-b-cobalt.dtb``` -> ```/boot/```
  * ```kernel/linux-4.9.51/build/linux/arch/arm/boot/zImage``` -> ```/boot/kernel-xenomai.img```
  * ```kernel/linux-4.9.51/build/linux/MODULES/``` -> ```/lib/modules```

* Append to ```/boot/config.txt```:

        device_tree=bcm2837-rpi-3-b-cobalt.dtb
        kernel=kernel-xenomai.img

* Append to ```/boot/cmdline.txt```:

        cma=256M@512M


Drivers build
-------------


To cross-compile the exemplary drivers follow the next steps:

        export CROSS_COMPILE=arm-linux-gnueabihf- (or your specific toolchain)

        make drivers


User-space tools
----------------

To natively compile the Xenomai user-space tools on the Rpi3 target, type:

       make tools

       sudo make tools_install

 To run the default benchmark, type

       sudo /usr/xenomai/bin/xeno-test


References
----------
* Xenomai installation guide: https://xenomai.org/installing-xenomai-3-x/
* I-pipe download: https://xenomai.org/downloads/ipipe/
* Xenomai download: https://xenomai.org/downloads/xenomai/stable/latest/
* I-pipe tracer: https://xenomai.org/2014/06/using-the-i-pipe-tracer/


