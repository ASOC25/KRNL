#include <krnl/libraries/std/string.h>
#include <krnl/libraries/std/stdint.h>
#include <krnl/devices/devices.h>
#include <krnl/arch/x86/io.h>
#include <krnl/arch/x86/cpu.h>
#include <krnl/arch/x86/idt.h>
#include <krnl/mem/allocator.h>
#include <krnl/debug/debug.h>
#include "krnl/arch/x86/apic.h"

#include "ps2.h"
#include "mouse.h"
#include "keyboard.h"

struct ps2_subscriber {
    void * parent;
    void (*handler)(void* data, char c, int ignore);
    struct ps2_subscriber *next;
};

struct ps2_subscriber *keyboard_all_subscribers = 0;
struct ps2_subscriber *mouse_all_subscribers = 0;
struct ps2_subscriber *keyboard_event_subscribers = 0;
struct ps2_subscriber *mouse_event_subscribers = 0;

void KeyboardInt_Handler(cpu_context_t* ctx, uint8_t cpuid) {
    (void)ctx;
    (void)cpuid;
    uint8_t scancode = inb(0x60);
    char result = handle_keyboard(scancode);

    struct ps2_subscriber *current = keyboard_all_subscribers;
    while (current) {
        current->handler(current->parent, result, 0);
        current = current->next;
    }
    current = keyboard_event_subscribers;
    while (current) {
        current->handler(current->parent, result, 0);
        current = current->next;
    }
}

void MouseInt_Handler(cpu_context_t* ctx, uint8_t cpuid) {
    (void)ctx;
    (void)cpuid;
    struct ps2_mouse_status mouse;
    uint8_t scancode = inb(0x60);
    handle_mouse(scancode);

    if (process_current_mouse_packet(&mouse)) {
        struct ps2_subscriber *current = mouse_all_subscribers;
        while (current) {
            if (current->handler)
                current->handler((void*)&mouse, 0, 0);
            current = current->next;
        }
        if (mouse.buttons) {
            current = mouse_event_subscribers;
            while (current) {
                if (current->handler)
                    current->handler((void*)&mouse, 0, 0);
                current = current->next;
            }
        }
    }
}

uint8_t read_mouse(struct ps2_mouse_status * mouse) {
    if (get_mouse(mouse)) {
        return 1;
    } else {
        return 0;
    }
}

uint8_t read_keyboard(char * key) {
    *key = get_last_key();
    return 1;
}

void keyboard_halt_until_enter() {
    halt_until_enter();
}

void ps2_subscribe(void* handler, uint8_t device, uint8_t event) {
    struct ps2_subscriber *new_subscriber = (struct ps2_subscriber*)kmalloc(sizeof(struct ps2_subscriber));
    memset(new_subscriber, 0, sizeof(struct ps2_subscriber));
    struct ps2_subscriber *current = 0;
    if (device == PS2_DEVICE_MOUSE) {
        new_subscriber->parent = 0x0;
        new_subscriber->handler = handler;
        
        if (event == PS2_DEVICE_GENERIC_EVENT) {
            current = mouse_all_subscribers;
            new_subscriber->next = current;
            mouse_all_subscribers = new_subscriber;
        } else if (event == PS2_DEVICE_SPECIAL_EVENT) {
            current = mouse_event_subscribers;
            new_subscriber->next = current;
            mouse_event_subscribers = new_subscriber;
        } else {
            return;
        }
        
    } else if (device == PS2_DEVICE_KEYBOARD) {
        struct ps2_kbd_ioctl_subscriptor *kbdsub = (struct ps2_kbd_ioctl_subscriptor*)handler;
        new_subscriber->parent = kbdsub->parent;
        new_subscriber->handler = kbdsub->handler;

        if (event == PS2_DEVICE_GENERIC_EVENT) {
            current = keyboard_all_subscribers;
            new_subscriber->next = current;
            keyboard_all_subscribers = new_subscriber;
        } else {
            return;
        }
    }
}

void ps2_unsubscribe(void *handler) {
    struct ps2_subscriber *current = keyboard_all_subscribers;
    struct ps2_subscriber *prev = 0;
    while (current) {
        if (current->handler == handler) {
            if (prev) {
                prev->next = current->next;
            } else {
                keyboard_all_subscribers = current->next;
            }
            kfree(current);
            return;
        }
        prev = current;
        current = current->next;
    }

    current = mouse_all_subscribers;
    prev = 0;
    while (current) {
        if (current->handler == handler) {
            if (prev) {
                prev->next = current->next;
            } else {
                mouse_all_subscribers = current->next;
            }
            kfree(current);
            return;
        }
        prev = current;
        current = current->next;
    }

    current = keyboard_event_subscribers;
    prev = 0;
    while (current) {
        if (current->handler == handler) {
            if (prev) {
                prev->next = current->next;
            } else {
                keyboard_event_subscribers = current->next;
            }
            kfree(current);
            return;
        }
        prev = current;
        current = current->next;
    }

    current = mouse_event_subscribers;
    prev = 0;
    while (current) {
        if (current->handler == handler) {
            if (prev) {
                prev->next = current->next;
            } else {
                mouse_event_subscribers = current->next;
            }
            kfree(current);
            return;
        }
        prev = current;
        current = current->next;
    }
}

static int64_t mouse_dd_read(device_addr_t id, uint64_t offset, uint64_t size, uint8_t* buffer) {
    (void)id;
    (void)size;
    (void)offset;
    struct ps2_mouse_status mouse;
    if (read_mouse(&mouse)) {
        memcpy(buffer, &mouse, sizeof(struct ps2_mouse_status));
        return 1;
    } else {
        return 0;
    }
}

static int64_t mouse_dd_write(device_addr_t id, uint64_t offset, uint64_t size, const uint8_t* buffer) {
    (void)id;
    (void)size;
    (void)offset;
    (void)buffer;
    return 0;
}

static int64_t mouse_dd_ioctl(device_addr_t id, uint64_t command, uint64_t input_buffer, uint64_t output_buffer) {
    (void)id;
    switch (command) {
        case IOCTL_MOUSE_SUBSCRIBE: {
            ps2_subscribe((void*)input_buffer, PS2_DEVICE_MOUSE, PS2_DEVICE_GENERIC_EVENT);
            return 1;
        }
        case IOCTL_MOUSE_UNSUBSCRIBE: {
            ps2_unsubscribe((void*)input_buffer);
            return 1;
        }
        case IOCTL_MOUSE_SUBSCRIBE_EVENT: {
            ps2_subscribe((void*)input_buffer, PS2_DEVICE_MOUSE, PS2_DEVICE_SPECIAL_EVENT);
            return 1;
        }
        case IOCTL_MOUSE_GET_STATUS: {
            struct ps2_mouse_status mouse;
            if (read_mouse(&mouse)) {
                memcpy((void*)output_buffer, &mouse, sizeof(struct ps2_mouse_status));
                return 1;
            } else {
                return 0;
            }
        }
        default: {
            return 0;
        }
    }

    return 0;
}

static int64_t keyboard_dd_read(device_addr_t id, uint64_t offset, uint64_t size, uint8_t* buffer) {
    (void)id;
    (void)size;
    (void)offset;
    char key;
    if (read_keyboard(&key)) {
        buffer[0] = key;
        return 1;
    } else {
        return 0;
    }
}

static int64_t keyboard_dd_write(device_addr_t id, uint64_t offset, uint64_t size, const uint8_t* buffer) {
    (void)id;
    (void)size;
    (void)offset;
    (void)buffer;
    return 0;
}

static int64_t keyboard_dd_ioctl(device_addr_t id, uint64_t command, uint64_t input_buffer, uint64_t output_buffer) {
    (void)id;
    (void)output_buffer;
    switch (command) {
        case IOCTL_KEYBOARD_SUBSCRIBE: {
            ps2_subscribe((void*)input_buffer, PS2_DEVICE_KEYBOARD, PS2_DEVICE_GENERIC_EVENT);
            return 1;
        }
        case IOCTL_KEYBOARD_UNSUBSCRIBE: {
            ps2_unsubscribe((void*)input_buffer);
            return 1;
        }
        case IOCTL_KEYBOARD_HALT_UNTIL_ENTER: {
            keyboard_halt_until_enter();
            return 1;
        }
        default: {
            return 0;
        }
    }

    return 0;
}


static int64_t initialize(device_addr_t id) {
    (void)id;
    return SUCCESS;
}

static int64_t shutdown(device_addr_t id) {
    //There is no shutdown procedure for serial ports
    (void)id;
    return SUCCESS;
}


status_t init_ps2_dd() {   
    status_t st = devices_new_driver(PS2_DEVICE_MAJOR_MOUSE, (struct device_driver){
        .read = mouse_dd_read,
        .write = mouse_dd_write,
        .ioctl = mouse_dd_ioctl,
        .initialize = initialize,
        .shutdown = shutdown
    });

    if (st != SUCCESS) {
        silent_panic();
    }

    st = devices_new_driver(PS2_DEVICE_MAJOR_KEYBOARD, (struct device_driver){
        .read = keyboard_dd_read,
        .write = keyboard_dd_write,
        .ioctl = keyboard_dd_ioctl,
        .initialize = initialize,
        .shutdown = shutdown
    });

    if (st != SUCCESS) {
        silent_panic();
    }


    return SUCCESS;
}

status_t ps2_init_pnp(void) {
    init_keyboard();
    init_mouse(PS2_MOUSE_DEFAULT_SCREEN_WIDTH, PS2_MOUSE_DEFAULT_SCREEN_HEIGHT);
    status_t st = init_ps2_dd();
    if (st != SUCCESS) {
        silent_panic();
    } else {
        if (devices_new_device(PS2_DEVICE_MAJOR_MOUSE, 0) != SUCCESS) {
            silent_panic();
        }

        if (devices_new_device(PS2_DEVICE_MAJOR_KEYBOARD, 0) != SUCCESS) {
            silent_panic();
        }
    }
    apic_ioapic_mask(0x21, 1); //Enable PS2 keyboard
    apic_ioapic_mask(0x2c, 1); //Enable PS2 mouse
    register_dynamic_interrupt(0x21, KeyboardInt_Handler); //Register keyboard interrupt handler
    register_dynamic_interrupt(0x2c, MouseInt_Handler); //Register mouse interrupt handler
    return SUCCESS;
}