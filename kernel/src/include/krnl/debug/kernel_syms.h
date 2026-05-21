#ifndef _KERNEL_SYMS_H
#define _KERNEL_SYMS_H

#include <krnl/libraries/std/stdint.h>

const char * kernel_resolve_symbol(uint64_t addr);

#endif
