#ifndef _DEBUG_H_
#define _DEBUG_H_

#include <stdarg.h>
#include <krnl/devices/devices.h>

void panic(const char* format, ...);
void silent_panic(void);
void kprintf(const char* format, ...);
void expect_panic(uint8_t should_panic);
void ktrace(const char * format, ...);
uint8_t check_and_clear_panicked(void);
#endif