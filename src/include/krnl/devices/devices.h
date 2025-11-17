#ifndef _DEVICES_H
#define _DEVICES_H

#include <krnl/libraries/std/stddef.h>
#include <krnl/libraries/std/stdint.h>

#define DEVICES_MAX_DEVICES 256
#define DEVICES_MAX_DRIVERS 256

typedef int64_t device_addr_t;
typedef int32_t device_major_t;
typedef int32_t device_minor_t;

struct device_driver {
    int64_t (*read)(device_addr_t, uint64_t, uint64_t, uint8_t*); // returns: number of bytes read (-1 on error) | params: device_addr, offset, size, destination buffer
    int64_t (*write)(device_addr_t, uint64_t, uint64_t, const uint8_t*); // returns: number of bytes written (-1 on error) | params: device_addr, offset, size, source buffer
    int64_t (*ioctl)(device_addr_t, uint64_t, uint64_t, uint64_t); // returns: ioctl result (-1 on error) | params: device_addr, command, input_buffer, output_buffer
    int64_t (*initialize)(device_addr_t); // returns: 0 on success, -1 on error | params: device_addr
    int64_t (*shutdown)(device_addr_t); // returns: 0 on success, -1 on error | params: device_addr
};

struct device_subsystem {
    device_addr_t devices[DEVICES_MAX_DRIVERS][DEVICES_MAX_DEVICES];
    struct device_driver drivers[DEVICES_MAX_DRIVERS];
};

void devices_init(void);

device_minor_t devices_new_device(device_major_t major_number, device_addr_t internal_address); //-1 on error | returns: minor number assigned
status_t devices_remove_device(device_major_t major_number, device_minor_t minor_number); 

status_t devices_new_driver(device_major_t major_number, struct device_driver ops);
status_t devices_remove_driver(device_major_t major_number);
int64_t devices_read(device_major_t major, device_minor_t minor, uint64_t offset, uint64_t size, uint8_t* buffer);
int64_t devices_write(device_major_t major, device_minor_t minor, uint64_t offset, uint64_t size, const uint8_t* buffer);
int64_t devices_ioctl(device_major_t major, device_minor_t minor, uint64_t command, uint64_t input_buffer, uint64_t output_buffer);

#endif