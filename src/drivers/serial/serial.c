#include <krnl/drivers/serial/serial.h>
#include <krnl/devices/devices.h>
#include <krnl/libraries/std/stdint.h>
#include <krnl/libraries/std/stddef.h>
#include <krnl/io/io.h>

struct serial_device {
    device_addr_t port_base;
    //Configuration parameters below:
    uint8_t interrupt_enable;
    uint32_t baud_rate;
    uint8_t data_bits;
    uint8_t stop_bits;
    char parity; // 'N' = None, 'E' = Even, 'O' = Odd
};

struct serial_device serial_devices[MAX_SERIAL_DEVICES] = {0}; //Array to hold detected serial ports, 0 port_base means unused

status_t test_serial_port(int port) {
    outb(port + 1, 0x00);    // Disable all interrupts
    outb(port + 3, 0x80);    // Enable DLAB (set baud rate divisor)
    outb(port + 0, 0x03);    // Set divisor to 3 (lo byte) 38400 baud
    outb(port + 1, 0x00);    //                  (hi byte)
    outb(port + 3, 0x03);    // 8 bits, no parity, one stop bit
    outb(port + 2, 0xC7);    // Enable FIFO, clear them, with 14-byte threshold
    outb(port + 4, 0x0B);    // IRQs enabled, RTS/DSR set
    outb(port + 4, 0x1E);    // Set in loopback mode, test the serial chip
    outb(port + 0, 0xAE);    // Test serial chip (send byte 0xAE and check if serial returns same byte)

    // Check if serial is faulty (i.e: not same byte as sent)
    if(inb(port + 0) != 0xAE) return INVALID_ARGUMENT; //Serial port not functioning
    outb(port + 4, 0x0F);

    return SUCCESS;
}

int serial_received(int port) {
   return inb(port + 5) & 1;
}
 
char read_serial(int port) {
   while (serial_received(port) == 0);
 
   return inb(port);
}

int is_transmit_empty(int port) {
   return inb(port + 5) & 0x20;
}
 
void write_serial(int port, char a) {
   while (is_transmit_empty(port) == 0);
   outb(port,a);
}


int64_t read(device_addr_t id, uint64_t offset, uint64_t size, uint8_t* buffer) {
    if (id < 0 || id >= MAX_SERIAL_DEVICES) {
        return INVALID_ARGUMENT;
    }
    if (buffer == 0) {
        return INVALID_ARGUMENT;
    }

    struct serial_device* dev = &(serial_devices[id]);

    for (uint64_t i = 0; i < size; i++) {
        buffer[i] = read_serial(dev->port_base);
    }

    return size;
}

int64_t write(device_addr_t id, uint64_t offset, uint64_t size, const uint8_t* buffer) {
    if (id < 0 || id >= MAX_SERIAL_DEVICES) {
        return INVALID_ARGUMENT;
    }
    if (buffer == 0) {
        return INVALID_ARGUMENT;
    }

    struct serial_device* dev = &(serial_devices[id]);

    for (uint64_t i = 0; i < size; i++) {
        write_serial(dev->port_base, buffer[i]);
    }

    return size;
}

int64_t initialize(device_addr_t id) {
    if (id < 0 || id >= MAX_SERIAL_DEVICES) {
        return INVALID_ARGUMENT;
    }

    struct serial_device* dev = &(serial_devices[id]);

    outb(dev->port_base + 1, 0x00);    // Disable all interrupts
    outb(dev->port_base + 3, 0x80);    // Enable DLAB (set baud rate divisor)
    uint16_t divisor = 115200 / dev->baud_rate;
    outb(dev->port_base + 0, divisor & 0xFF);
    outb(dev->port_base + 1, (divisor >> 8) & 0xFF);
    uint8_t line_control = 0;
    line_control |= (dev->data_bits - 5); // Set data bits
    if (dev->stop_bits == 2) {
        line_control |= 0x04; // Set stop bits
    }
    if (dev->parity == 'E') {
        line_control |= 0x18; // Even parity
    } else if (dev->parity == 'O') {
        line_control |= 0x08; // Odd parity
    }
    outb(dev->port_base + 3, line_control);    // Set line control
    outb(dev->port_base + 2, 0xC7);    // Enable FIFO, clear them, with 14-byte threshold
    outb(dev->port_base + 4, 0x0B);    // IRQs enabled, RTS/DSR set

    //Set interrupts according to interrupt_enable
    outb(dev->port_base + 1, dev->interrupt_enable);
    return SUCCESS;
}

int64_t ioctl(device_addr_t id, uint64_t command, uint64_t input_buffer, uint64_t output_buffer) {
    if (id < 0 || id >= MAX_SERIAL_DEVICES) {
        return INVALID_ARGUMENT;
    }

    switch (command) {
        case SERIAL_IOCTL_SET_CONFIG: {
            if (input_buffer == 0) {
                return INVALID_ARGUMENT;
            }
            struct serial_config* config = (struct serial_config*)input_buffer;
            struct serial_device* dev = &(serial_devices[id]);
            dev->port_base = config->port_base;
            dev->interrupt_enable = config->interrupt_enable;
            dev->baud_rate = config->baud_rate;
            dev->data_bits = config->data_bits;
            dev->stop_bits = config->stop_bits;
            dev->parity = config->parity;

            //Update the serial port configuration if it's already initialized
            initialize(id);
            break;
        }
        case SERIAL_IOCTL_GET_CONFIG: {
            if (output_buffer == 0) {
                return INVALID_ARGUMENT;
            }
            struct serial_config* config = (struct serial_config*)output_buffer;
            struct serial_device* dev = &(serial_devices[id]);
            config->baud_rate = dev->baud_rate;
            config->data_bits = dev->data_bits;
            config->stop_bits = dev->stop_bits;
            config->parity = dev->parity;
            config->interrupt_enable = dev->interrupt_enable;
            config->port_base = dev->port_base;
            break;
        }
        
        default:
            return NOT_IMPLEMENTED;
    }

    return SUCCESS;
}

int64_t shutdown(device_addr_t id) {
    //There is no shutdown procedure for serial ports
    return SUCCESS;
}

status_t serial_init(void) {
    struct device_driver serial_driver = {
        .read = read,
        .write = write,
        .ioctl = ioctl,
        .initialize = initialize,
        .shutdown = shutdown
    };

    return devices_new_driver(SERIAL_DRIVER_MAJOR, serial_driver);
}

status_t serial_init_pnp(void) {

    status_t res = serial_init();

    if (res != SUCCESS) {
        return res;
    }

    int serial_ports[] = {0x3F8, 0x2F8, 0x3E8, 0x2E8};
    int valid_ports = 0;
    for(int i = 0; i < 4; i++) {
        if (test_serial_port(serial_ports[i]) == SUCCESS) {
            struct serial_device* dev = &serial_devices[i];
            dev->port_base = serial_ports[i];
            dev->baud_rate = 9600;
            dev->data_bits = 8;
            dev->stop_bits = 1;
            dev->parity = 'N';
            dev->interrupt_enable = 0x00; //Disable interrupts by default

            if (devices_new_device(SERIAL_DRIVER_MAJOR, i) != SUCCESS) {
                dev->port_base = 0; //Mark as unused
            } else {
                valid_ports++;
            }
        }
    }

    return (valid_ports > 0) ? SUCCESS : NOT_FOUND;
}