#ifndef _x86_HPET_H
#define _x86_HPET_H

#include <krnl/libraries/std/stdint.h>

void hpet_init();

uint64_t hpet_get_current_time(void);

void hpet_sleep(uint64_t us);

#endif