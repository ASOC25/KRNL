#ifndef _DEBUG_H_
#define _DEBUG_H_

#include <stdarg.h>
#include <krnl/devices/devices.h>

void debug_init(device_major_t major_number, device_minor_t minor_number);

void kprintf(const char* format, ...);
#endif