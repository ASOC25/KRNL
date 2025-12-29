#include <krnl/devices/devices.h>
#include <krnl/libraries/std/stddef.h>
#include <krnl/libraries/std/stdint.h>
#include <krnl/debug/debug.h>
#include <krnl/libraries/assert/assert.h>

struct device_subsystem device_s = {0};

void devices_init(void) {
    for (device_major_t i = 0; i < DEVICES_MAX_DRIVERS; i++) {
        device_s.drivers[i].initialize = 0x0;
    }
    for (device_major_t i = 0; i < DEVICES_MAX_DRIVERS; i++) {
        for (device_minor_t j = 0; j < DEVICES_MAX_DEVICES; j++) {
            device_s.devices[i][j] = -1;
        }
    }
}

device_minor_t devices_new_device(device_major_t major_number, device_addr_t internal_address) {
    if (device_s.drivers[major_number].initialize == 0x0) {
        silent_panic();
    }
    for (device_minor_t i = 0; i < DEVICES_MAX_DEVICES; i++) {
        if (device_s.devices[major_number][i] == -1) {
            device_s.devices[major_number][i] = internal_address;

            if (device_s.drivers[major_number].initialize) {
                if (device_s.drivers[major_number].initialize(internal_address) != SUCCESS) {
                    device_s.devices[major_number][i] = -1; // Rollback on failure
                    return FAILURE;
                }
            }
            return i;
        }
    }

    silent_panic();
    return FAILURE;
}

status_t devices_remove_device(device_major_t major_number, device_minor_t minor_number) {
    if (device_s.drivers[major_number].initialize == 0x0) {
        silent_panic();
    }
    device_addr_t addr = device_s.devices[major_number][minor_number];
    if (addr == -1) {
        silent_panic();
    }

    if (device_s.drivers[major_number].shutdown) {
        if (device_s.drivers[major_number].shutdown(addr) != SUCCESS) {
            return FAILURE;
        }
    }

    device_s.devices[major_number][minor_number] = -1;
    return SUCCESS;
}

status_t devices_new_driver(device_major_t major_number, struct device_driver ops) {
    if (device_s.drivers[major_number].initialize != 0x0) {
        silent_panic();
    }
    device_s.drivers[major_number] = ops;
    return SUCCESS;
}

status_t devices_remove_driver(device_major_t major_number) {
    if (device_s.drivers[major_number].initialize == 0x0) {
        silent_panic();
    }
    // We want to shutdown all devices managed by this driver here.
    for (device_minor_t i = 0; i < DEVICES_MAX_DEVICES; i++) {
        device_addr_t addr = device_s.devices[major_number][i];
        if (addr != -1) {
            if (device_s.drivers[major_number].shutdown) {
                if (device_s.drivers[major_number].shutdown(addr) != SUCCESS) {
                    //We failed to shutdown a device, abort the driver removal
                    return FAILURE;
                }
            }
            device_s.devices[major_number][i] = -1;
        }
    }

    device_s.drivers[major_number].initialize = 0x0;
    return SUCCESS;
}

int64_t devices_read(device_major_t major, device_minor_t minor, uint64_t offset, uint64_t size, uint8_t* buffer) {
    if (device_s.drivers[major].initialize == 0x0) {
        silent_panic();
    }

    device_addr_t addr = device_s.devices[major][minor];
    if (addr == -1) {
        silent_panic();
    }

    if (device_s.drivers[major].read) {
        int64_t res = device_s.drivers[major].read(addr, offset, size, buffer);
        return res;
    }

    silent_panic();
    return FAILURE;
}

int64_t devices_write(device_major_t major, device_minor_t minor, uint64_t offset, uint64_t size, const uint8_t* buffer) {
    if (device_s.drivers[major].initialize == 0x0) {
        silent_panic();
    }

    device_addr_t addr = device_s.devices[major][minor];
    if (addr == -1) {
        silent_panic();
    }

    if (device_s.drivers[major].write) {
        int64_t res = device_s.drivers[major].write(addr, offset, size, buffer);
        return res;
    }

    silent_panic();
    return FAILURE;
}

int64_t devices_ioctl(device_major_t major, device_minor_t minor, uint64_t command, uint64_t input_buffer, uint64_t output_buffer) {
    if (device_s.drivers[major].initialize == 0x0) {
        silent_panic();
    }

    device_addr_t addr = device_s.devices[major][minor];
    if (addr == -1) {
        silent_panic();
    }

    if (device_s.drivers[major].ioctl) {
        int64_t res = device_s.drivers[major].ioctl(addr, command, input_buffer, output_buffer);
        return res;
    }

    silent_panic();
    return FAILURE;
}