#!/bin/bash

#./mkbootimg_tools/mkbootimg --kernel ./zImage_Ready/new/zImage --ramdisk ./zImage_Ready/new/ramdisk.img --ramdisk_offset 0x02008000 --cmdline 'console=ttyHSL0,115200,n8 androidboot.console=ttyHSL0 androidboot.hardware=qcom user_debug=31 ehci-hcd.park=3 androidboot.bootdevice=7824900.sdhci' --base 0x80000000 --pagesize 2048 --dt ./zImage_Ready/new/dt.img --tags_offset 0x01e00000 -o ./zImage_Ready/boot_new.img

./mkbootimg_tools/mkboot ./zImage_Ready/new/ ./zImage_Ready/boot_rdy.img
