#ifndef _ALLOCATOR_H
#define _ALLOCATOR_H

#include <krnl/libraries/std/stdint.h>
#include <krnl/mem/vmm.h>

typedef struct stack {
    void * top;
    void * base;
    int flags;
    uint64_t guard_size;
} stack_t;

typedef struct allocator_farlands {
    void * address;
    void * handle;
    int flags;
    uint64_t size;
} farlands_t;

typedef struct allocator_stack_farlands {
    void * top;
    void * base;
    void * handle_top;
    void * handle_base;
    int flags;
    uint64_t guard_size;

} farlands_stack_t;

void * kmalloc(uint64_t size);
status_t kmalloc_farlands(vmm_root * root, uint64_t size, uint64_t vaddr, uint8_t flags, farlands_t * farlands);
void kfree(void *ptr); //Common for both user and kernel

stack_t * kstackalloc(uint64_t size);
status_t kstackalloc_farlands(vmm_root * root, uint64_t size, uint64_t vaddr, uint8_t flags, farlands_stack_t * farlands);
void kstackfree(stack_t *ptr); //Common for both user and kernel
#endif