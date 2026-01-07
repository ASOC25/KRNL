#ifndef _x86_HPET_H
#define _x86_HPET_H

#include <krnl/libraries/std/stdint.h>

#define HPET_TIMER_IRQ 0x22 //Do not change, IRQ2 is connected to hpet on most systems!!!
#define HPET_SYSTEM_TASK_FEMTOS 77000000000000 //77 milliseconds in femtoseconds
#define HPET_SYSTEM_TASK_NANO 77000000  //77 milliseconds in nanoseconds
#define HPET_SYSTEM_TASK_MICROS 77000  //77 milliseconds in microseconds

void hpet_init();

uint64_t hpet_get_current_time(void);

uint64_t hpet_get_resolution(void);

void hpet_sleep(uint64_t us);

void arm_hpet_interrupt_timer(uint64_t us);

void hpet_enable();
void hpet_disable();
#endif