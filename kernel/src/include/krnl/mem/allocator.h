#ifndef _ALLOCATOR_H
#define _ALLOCATOR_H

#include <krnl/libraries/std/stdint.h>
#include <krnl/mem/vmm.h>

typedef struct stack {
    void * top;
    void * base;
    int flags;
} stack_t;

void * kmalloc(uint64_t size);
stack_t * kstackalloc(vmm_root_t * root, uint64_t initial_size);
void kfree(void * virtual_address);
void kstackfree(stack_t * stk);
void add_allocation(vmm_root_t * root, void * physical_address, void * virtual_address, uint64_t size, uint8_t permisions);

void* malloc(vmm_root_t * root, uint64_t size, uint64_t vaddr, uint8_t flags);
stack_t * stackalloc(vmm_root_t * root, uint64_t size, uint64_t vaddr, uint8_t flags);
void free(vmm_root_t * cr3, void * virtual_address);
void stackfree(vmm_root_t * cr3, stack_t * stk);
    
stack_t * copy_kstack(vmm_root_t * dest_root, stack_t * source);
stack_t * copy_stack(vmm_root_t * dest_root, stack_t * source);
void * to_kident(vmm_root_t * cr3, void * vaddr);

#endif