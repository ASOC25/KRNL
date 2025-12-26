#include <krnl/devices/devices.h>
#include <krnl/libraries/std/stddef.h>
#include <krnl/libraries/std/stdint.h>
#include <krnl/debug/debug.h>
#include <krnl/libraries/lock/spinlock.h>
#include <krnl/libraries/assert/assert.h>

struct device_subsystem device_s = {0};

void devices_init(void) {
    device_s.global_lock.lock = 0;
    device_s.global_lock.last_acquirer = NULL;
    for (device_major_t i = 0; i < DEVICES_MAX_DRIVERS; i++) {
        device_s.drivers[i].initialize = 0x0;
    }
    for (device_major_t i = 0; i < DEVICES_MAX_DRIVERS; i++) {
        for (device_minor_t j = 0; j < DEVICES_MAX_DEVICES; j++) {
            device_s.devices[i][j] = -1;
            device_s.devices_lock[i][j].lock = 0;
            device_s.devices_lock[i][j].last_acquirer = NULL;
            device_s.devices_use_spinlock[i][j] = 0;
        }
    }
}

device_minor_t devices_new_device(device_major_t major_number, device_addr_t internal_address, uint64_t use_spinlock) {
    if (device_s.drivers[major_number].initialize == 0x0) {
        silent_panic();
    }
    assert(!spinlock_acquire(&device_s.global_lock));
    for (device_minor_t i = 0; i < DEVICES_MAX_DEVICES; i++) {
        if (device_s.devices[major_number][i] == -1) {
            device_s.devices[major_number][i] = internal_address;
            device_s.devices_lock[major_number][i].lock = 0;
            device_s.devices_lock[major_number][i].last_acquirer = 0;
            device_s.devices_use_spinlock[major_number][i] = use_spinlock;

            if (device_s.drivers[major_number].initialize) {
                if (device_s.devices_use_spinlock[major_number][i]) {
                    assert(!spinlock_acquire(&(device_s.devices_lock[major_number][i])));
                }
                if (device_s.drivers[major_number].initialize(internal_address) != SUCCESS) {
                    device_s.devices[major_number][i] = -1; // Rollback on failure
                    spinlock_release(&device_s.global_lock);
                    if (device_s.devices_use_spinlock[major_number][i]) {
                        spinlock_release(&(device_s.devices_lock[major_number][i]));
                    }
                    return FAILURE;
                }
                if (device_s.devices_use_spinlock[major_number][i]) {
                    spinlock_release(&(device_s.devices_lock[major_number][i]));
                }
            }
            spinlock_release(&device_s.global_lock);
            return i;
        }
    }

    spinlock_release(&device_s.global_lock);
    silent_panic();
    return FAILURE;
}

status_t devices_remove_device(device_major_t major_number, device_minor_t minor_number) {
    if (device_s.drivers[major_number].initialize == 0x0) {
        silent_panic();
    }
    assert(!spinlock_acquire(&device_s.global_lock));
    device_addr_t addr = device_s.devices[major_number][minor_number];
    if (addr == -1) {
        silent_panic();
    }

    if (device_s.drivers[major_number].shutdown) {
        if (device_s.devices_use_spinlock[major_number][minor_number]) {
            assert(!spinlock_acquire(&(device_s.devices_lock[major_number][minor_number])));
        }
        if (device_s.drivers[major_number].shutdown(addr) != SUCCESS) {
            spinlock_release(&device_s.global_lock);
            if (device_s.devices_use_spinlock[major_number][minor_number]) {
                spinlock_release(&(device_s.devices_lock[major_number][minor_number]));
            }
            return FAILURE;
        }
        if (device_s.devices_use_spinlock[major_number][minor_number]) {
            spinlock_release(&(device_s.devices_lock[major_number][minor_number]));
        }
    }

    device_s.devices[major_number][minor_number] = -1;
    spinlock_release(&device_s.global_lock);
    return SUCCESS;
}

status_t devices_new_driver(device_major_t major_number, struct device_driver ops) {
    if (device_s.drivers[major_number].initialize != 0x0) {
        silent_panic();
    }
    assert(!spinlock_acquire(&device_s.global_lock));
    device_s.drivers[major_number] = ops;
    spinlock_release(&device_s.global_lock);
    return SUCCESS;
}

status_t devices_remove_driver(device_major_t major_number) {
    if (device_s.drivers[major_number].initialize == 0x0) {
        silent_panic();
    }
    assert(!spinlock_acquire(&device_s.global_lock));
    // We want to shutdown all devices managed by this driver here.
    for (device_minor_t i = 0; i < DEVICES_MAX_DEVICES; i++) {
        device_addr_t addr = device_s.devices[major_number][i];
        if (addr != -1) {
            if (device_s.drivers[major_number].shutdown) {
                if (device_s.drivers[major_number].shutdown(addr) != SUCCESS) {
                    //We failed to shutdown a device, abort the driver removal
                    spinlock_release(&device_s.global_lock);
                    return FAILURE;
                }
            }
            device_s.devices[major_number][i] = -1;
        }
    }

    device_s.drivers[major_number].initialize = 0x0;
    spinlock_release(&device_s.global_lock);
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
        if (device_s.devices_use_spinlock[major][minor]) {
            assert(!spinlock_acquire(&(device_s.devices_lock[major][minor])));
        }
        int64_t res = device_s.drivers[major].read(addr, offset, size, buffer);
        if (device_s.devices_use_spinlock[major][minor]) {
            spinlock_release(&(device_s.devices_lock[major][minor]));
        }
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
        if (device_s.devices_use_spinlock[major][minor]) {
            assert(!spinlock_acquire(&(device_s.devices_lock[major][minor])));
        }
        int64_t res = device_s.drivers[major].write(addr, offset, size, buffer);
        if (device_s.devices_use_spinlock[major][minor]) {
            spinlock_release(&(device_s.devices_lock[major][minor]));
        }
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
        if (device_s.devices_use_spinlock[major][minor]) {
            assert(!spinlock_acquire(&(device_s.devices_lock[major][minor])));
        }
        int64_t res = device_s.drivers[major].ioctl(addr, command, input_buffer, output_buffer);
        if (device_s.devices_use_spinlock[major][minor]) {
            spinlock_release(&(device_s.devices_lock[major][minor]));
        }
        return res;
    }

    silent_panic();
    return FAILURE;
}