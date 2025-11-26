#ifndef _DEBUG_H_
#define _DEBUG_H_

#include <stdarg.h>
#include <krnl/devices/devices.h>

void debug_init(device_major_t major_number, device_minor_t minor_number);
void panic(const char* format, ...);
void silent_panic(void);
void kprintf(const char* format, ...);
void expect_panic(uint8_t should_panic);
uint8_t check_and_clear_panicked(void);
#endif