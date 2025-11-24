#ifndef _ALLOCATOR_H
#define _ALLOCATOR_H

#include <krnl/libraries/std/stdint.h>

void * kmalloc(uint64_t size);
void kfree(void *ptr);

/* Helpers to inspect the simple bump allocator (early boot) */
uint64_t kmalloc_used(void);
uint64_t kmalloc_capacity(void);
void *kmalloc_base(void);
void *kmalloc_current(void);

#endif