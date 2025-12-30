#ifndef _SIGNALS_H
#define _SIGNALS_H

#include <krnl/libraries/std/stdint.h>
#include <krnl/libraries/std/stddef.h>
#include <krnl/process/process.h>
#include <krnl/libraries/std/time.h>

#define SIGNAL_SLEEP_INTERRUPT 0x81

void sleep(thread_t * process, int condition);
void wakeup(int condition);
status_t nanosleep(thread_t * thread, struct timespec *duration, struct timespec *rem);
void update_counters();
#endif