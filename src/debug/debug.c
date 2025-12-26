#include <krnl/debug/debug.h>
#include <krnl/libraries/std/stdio.h>
#include <krnl/libraries/std/stdint.h>
#include <krnl/libraries/std/string.h>
#include <krnl/libraries/lock/spinlock.h>
#include <krnl/libraries/assert/assert.h>
#include <krnl/drivers/framebuffer/framebuffer.h>
#include <krnl/drivers/serial/serial.h>

#include <krnl/devices/devices.h>

spinlock_t debug_spinlock = SPINLOCK_INIT;
uint8_t panic_expected = 0;
uint8_t panicked = 0;

void expect_panic(uint8_t should_panic) {
    assert(!spinlock_acquire(&debug_spinlock));
    panic_expected = should_panic;
    spinlock_release(&debug_spinlock);
}

uint8_t check_and_clear_panicked(void) {
    assert(!spinlock_acquire(&debug_spinlock));
    uint8_t has_panicked = panicked;
    panicked = 0;
    spinlock_release(&debug_spinlock);
    return has_panicked;
}

void panic(const char* format, ...) {
    __asm__ volatile ("cli");
    char buffer[1024] = {0};
    int offset = 0;
    if (panic_expected) {
        strcpy(buffer, "EXPECTED KERNEL PANIC: ");
        offset = 23; // Length of "EXPECTED KERNEL PANIC: "
    } else {
        strcpy(buffer, "KERNEL PANIC: ");
        offset = 14; // Length of "KERNEL PANIC: "
    }
    va_list args;
    va_start(args, format);
    int len = vsnprintf_(buffer+offset, sizeof(buffer) - offset, format, args);
    va_end(args);
    if (len > 0) {
        devices_write(FRAMEBUFFER_DRIVER_MAJOR, 0, 0, (uint64_t)(len + offset), (const uint8_t *)buffer);
    }
    // Halt the system
    if (!panic_expected) {
        while (1) {
            __asm__ volatile ("hlt");
        }
    } else {
        panicked = 1;
    }
}

void silent_panic(void) {
    // Similar to panic but does not output any message
    assert(!spinlock_acquire(&debug_spinlock));
    while (1) {
        __asm__ volatile ("hlt");
    }
    spinlock_release(&debug_spinlock);
}

void kprintf(const char* format, ...) {
    assert(!spinlock_acquire(&debug_spinlock));
    char buffer[1024];
    va_list args;
    va_start(args, format);
    int len = vsnprintf_(buffer, sizeof(buffer), format, args);
    va_end(args);
    if (len > 0) {
        devices_write(SERIAL_DRIVER_MAJOR, 0, 0, (uint64_t)len, (const uint8_t *)buffer);
    }
    spinlock_release(&debug_spinlock);
}

void ktrace(const char * format, ...) {
    char buffer[1024];
    va_list args;
    va_start(args, format);
    int len = vsnprintf_(buffer, sizeof(buffer), format, args);
    va_end(args);
    if (len > 0) {
        for (int i = 0; i < len; i++) {
            devices_ioctl(FRAMEBUFFER_DRIVER_MAJOR, 0, FRAMEBUFFER_IOCTL_PUTCHAR, (uint64_t)(uintptr_t)&buffer[i], 0);
        }
    }
}