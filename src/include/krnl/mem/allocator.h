#ifndef _ALLOCATOR_H
#define _ALLOCATOR_H

#include <krnl/libraries/std/stdint.h>
#include <krnl/mem/vmm.h>

struct stack {
    void * top;
    void * base;
};

typedef struct allocator_farlands {
    void * farland_address;
    void * access_address;
} farlands_t;

void * kmalloc(uint64_t size);
status_t kmalloc_farlands(vmm_root * root, uint64_t size, uint64_t vaddr, uint8_t flags, farlands_t * farlands);
void kfree(void *ptr); //Common for both user and kernel
struct stack * kstackalloc(uint64_t initial_size);
struct stack * kstackalloc_user(vmm_root * root, uint64_t initial_size);
void kstackfree(struct stack *ptr); //Common for both user and kernel
#endif