#ifndef _SIGNALS_H
#define _SIGNALS_H

#include <krnl/libraries/std/stdint.h>
#include <krnl/libraries/std/stddef.h>
#include <krnl/process/process.h>
#include <krnl/libraries/std/time.h>
void sleep(thread_t * process, int condition);
void wakeup(int condition);
int nanosleep(thread_t * thread, struct timespec *duration, struct timespec *rem);

#endif