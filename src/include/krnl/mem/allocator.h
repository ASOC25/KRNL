#ifndef _ALLOCATOR_H
#define _ALLOCATOR_H

#include <krnl/libraries/std/stdint.h>
#include <krnl/mem/vmm.h>

struct stack {
    void * top;
    void * base;
};

void * kmalloc(uint64_t size);
void * kmalloc_user(uint64_t size);
void kfree(void *ptr); //Common for both user and kernel
struct stack * kstackalloc(uint64_t initial_size);
struct stack * kstackalloc_user(vmm_root * root, uint64_t initial_size);
void kstackfree(struct stack *ptr); //Common for both user and kernel
#endif