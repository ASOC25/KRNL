#include <krnl/debug/debug.h>
#include <krnl/libraries/std/stdio.h>
#include <krnl/libraries/std/stdint.h>
#include <krnl/libraries/std/string.h>

device_major_t major_number_global;
device_minor_t minor_number_global;
uint8_t panic_expected = 0;
uint8_t panicked = 0;

void debug_init(device_major_t major_number, device_minor_t minor_number) {
    major_number_global = major_number;
    minor_number_global = minor_number;
}

void expect_panic(uint8_t should_panic) {
    panic_expected = should_panic;
}

uint8_t check_and_clear_panicked(void) {
    uint8_t has_panicked = panicked;
    panicked = 0;
    return has_panicked;
}

void panic(const char* format, ...) {
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
        devices_write(major_number_global, minor_number_global, 0, (uint64_t)(len + offset), (const uint8_t *)buffer);
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
    while (1) {
        __asm__ volatile ("hlt");
    }
}

void kprintf(const char* format, ...) {
    char buffer[1024];
    va_list args;
    va_start(args, format);
    int len = vsnprintf_(buffer, sizeof(buffer), format, args);
    va_end(args);
    if (len > 0) {
        devices_write(major_number_global, minor_number_global, 0, (uint64_t)len, (const uint8_t *)buffer);
    }
}