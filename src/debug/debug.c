#include <krnl/debug/debug.h>
#include <krnl/libraries/std/stdio.h>
#include <krnl/libraries/std/stdint.h>

device_major_t major_number_global;
device_minor_t minor_number_global;

void debug_init(device_major_t major_number, device_minor_t minor_number) {
    major_number_global = major_number;
    minor_number_global = minor_number;
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