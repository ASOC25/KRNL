#ifndef _ALLOCATOR_H
#define _ALLOCATOR_H

#include <krnl/libraries/std/stdint.h>
#include <krnl/mem/vmm.h>

typedef struct stack {
    void * top;
    void * base;
    int flags;
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

} farlands_stack_t;

void * kmalloc(uint64_t size);
stack_t * kstackalloc(uint64_t initial_size);
void kfree(void * virtual_address);
void kstackfree(stack_t * stk);

status_t malloc(vmm_root_t * root, uint64_t size, uint64_t vaddr, uint8_t flags, farlands_t * farlands);
status_t stackalloc(vmm_root_t * root, uint64_t size, uint64_t vaddr, uint8_t flags, farlands_stack_t * farstack);
void free(vmm_root_t * cr3, void * virtual_address);
void stackfree(vmm_root_t * cr3, farlands_stack_t * farstack);
    
stack_t * copy_kstack(vmm_root_t * dest_root, stack_t * source);
farlands_stack_t * copy_stack(vmm_root_t * dest_root, farlands_stack_t * source);

#endif