# ds_oc_kmod

Kernel module for overclocking the Sony DualSense controller on XHCI controllers, if you have a native EHCI controller then you may be better off using that.

## Building

Use `make` to build ds_oc.ko and `sudo insmod ds_oc.ko` to load the module into the running kernel.


If you want to unload the module (revert the increased polling rate) use `sudo rmmod ds_oc.ko`. You can also use `make clean` to clean up any files created by `make`.

If you get an error saying "building multiple external modules is not supported" it's because you have a space somewhere in the path to the gcadapter-oc-kmod directory.

GNU Make can't handle spaces in filenames so move the directory to a path without spaces (example: `/home/falco/My Games/wmo-oc-kmod` -> `/home/falco/wmo-oc-kmod`).

## Changing the polling rate

Polling rate is set according to the `bInterval` value in the USB endpoint descriptor. The value sets the polling rate in milliseconds, for example: an interval value of 4 equals 250 Hz.

You can change the rate by using the kernel parameter `ds_oc.rate=n` (if installed), passing the rate to `insmod ds_oc.ko rate=n` or going into `/sys/module/ds_oc/parameters` and using `echo n > rate` to change the value

Changing the polling rate may not take effect. Please test it yourself.