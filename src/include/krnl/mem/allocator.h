#ifndef _ALLOCATOR_H
#define _ALLOCATOR_H

#include <krnl/libraries/std/stdint.h>

struct stack {
    void * top;
    void * base;
};

void * kmalloc(uint64_t size);
void kfree(void *ptr);
struct stack * kstackalloc(uint64_t initial_size);
void kstackfree(struct stack *ptr);
#endif