#include <krnl/drivers/ramdisk/ramdisk.h>
#include <krnl/devices/devices.h>
#include <krnl/libraries/std/stdint.h>
#include <krnl/libraries/std/stddef.h>
#include <krnl/debug/debug.h>
#include <krnl/libraries/std/string.h>

struct ramdisk_info ramdisk_devices[MAX_RAMDISK_DEVICES] = {0}; //Array to hold detected ramdisks, 0 start_address means unused

static int64_t read(device_addr_t id, uint64_t offset, uint64_t size, uint8_t* buffer) {
    struct ramdisk_info* dev = &(ramdisk_devices[id]);
    if (dev->start_address == 0 || dev->size == 0) {
        silent_panic();
    }

    if (offset + size > dev->size) {
        silent_panic();
    }

    memcpy(buffer, (uint8_t*)(dev->start_address + offset), size);
    return size;
}

static int64_t write(device_addr_t id, uint64_t offset, uint64_t size, const uint8_t* buffer) {
    struct ramdisk_info* dev = &(ramdisk_devices[id]);
    if (dev->start_address == 0 || dev->size == 0) {
        silent_panic();
    }

    if (offset + size > dev->size) {
        silent_panic();
    }

    memcpy((uint8_t*)(dev->start_address + offset), buffer, size);
    return size;
}

static int64_t initialize(device_addr_t id) {
    (void)id;
    return SUCCESS;
}

static int64_t ioctl(device_addr_t id, uint64_t command, uint64_t input_buffer, uint64_t output_buffer) {
    (void)id;
    (void)command;
    (void)input_buffer;
    (void)output_buffer;
    return SUCCESS;
}

static int64_t shutdown(device_addr_t id) {
    //There is no shutdown procedure for serial ports
    (void)id;
    return SUCCESS;
}

status_t ramdisk_init(void) {
    struct device_driver ramdisk_driver = {
        .read = read,
        .write = write,
        .ioctl = ioctl,
        .initialize = initialize,
        .shutdown = shutdown
    };

    return devices_new_driver(RAMDISK_DRIVER_MAJOR, ramdisk_driver);
}

status_t ramdisk_init_pnp(void) {
    status_t st = ramdisk_init();
    if (st != SUCCESS) {
        silent_panic();
    }

    //Iterate all physical memory searching for a RAMDISK_SIGNATURE
    uint64_t ramdisk_size = (uint64_t)(RAMDISK_END - RAMDISK_START);
    if (ramdisk_size == 0) {
        silent_panic();
    }

    ramdisk_devices[0].start_address = (uint64_t)RAMDISK_START;
    ramdisk_devices[0].size = ramdisk_size;

    if (devices_new_device(RAMDISK_DRIVER_MAJOR, 0) != SUCCESS) {
        silent_panic();
    }

    return SUCCESS;
}