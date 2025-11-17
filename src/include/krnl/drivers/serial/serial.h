#ifndef _SERIAL_H
#define _SERIAL_H

#include <krnl/devices/devices.h>

#define SERIAL_DRIVER_MAJOR 3
#define MAX_SERIAL_DEVICES 32


#define SERIAL_IOCTL_SET_CONFIG 0x01
#define SERIAL_IOCTL_GET_CONFIG 0x02

struct serial_config {
    device_addr_t port_base;
    uint8_t interrupt_enable;
    uint32_t baud_rate;
    uint8_t data_bits;
    uint8_t stop_bits;
    char parity; // 'N' = None, 'E' = Even, 'O' = Odd
};

status_t serial_init(void);
status_t serial_init_pnp(void);
#endif