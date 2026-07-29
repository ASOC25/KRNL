#ifndef _MEMDEV_H
#define _MEMDEV_H

/* VFS-only driver tag: memdev has no real hardware behind it, so unlike
   ramdisk/serial/ahci it is never registered with the krnl/devices/devices.h
   layer -- there's nothing there to indirect through. */
#define MEMDEV_DRIVER_MAJOR   6

#define MEMDEV_MINOR_NULL     0
#define MEMDEV_MINOR_ZERO     1
#define MEMDEV_MINOR_RANDOM   2
#define MEMDEV_MINOR_URANDOM  3

void memdev_init(void);
#endif
