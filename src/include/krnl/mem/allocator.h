#ifndef _ALLOCATOR_H
#define _ALLOCATOR_H

#include <krnl/libraries/std/stdint.h>

void * kmalloc(uint64_t size);
void kfree(void *ptr);

#endif