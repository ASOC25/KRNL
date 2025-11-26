#ifndef _RAMDISK_H
#define _RAMDISK_H
#include <krnl/devices/devices.h>

struct ramdisk_info {
    uint64_t start_address;
    uint64_t size;
};

#define RAMDISK_DRIVER_MAJOR 4
#define MAX_RAMDISK_DEVICES 32

extern unsigned char RAMDISK_START[];
extern unsigned char RAMDISK_END[];

status_t ramdisk_init(void);
status_t ramdisk_init_pnp(void);
#endif