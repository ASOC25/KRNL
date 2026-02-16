#ifndef _X86_CPU_H
#define _X86_CPU_H

#include <krnl/libraries/std/stdint.h>
#include <krnl/mem/allocator.h>

#define KERNEL_STACK_SIZE 0x10000

#define RFLAGS_CARRY                        (1 << 0)
#define RFLAGS_ONE                          (1 << 1)
#define RFLAGS_PARITY                       (1 << 2)
#define RFLAGS_RESERVED1                    (1 << 3)
#define RFLAGS_AUX_CARRY                    (1 << 4)
#define RFLAGS_RESERVED2                    (1 << 5)
#define RFLAGS_ZERO                         (1 << 6)
#define RFLAGS_SIGN                         (1 << 7)
#define RFLAGS_TRAP                         (1 << 8)
#define RFLAGS_INTERRUPT_ENABLE             (1 << 9)
#define RFLAGS_DIRECTION                    (1 << 10)
#define RFLAGS_OVERFLOW                     (1 << 11)
#define RFLAGS_IO_PRIVILEGE                 (3 << 12)
#define RFLAGS_NESTED_TASK                  (1 << 14)
#define RFLAGS_RESERVED3                    (1 << 15)
#define RFLAGS_RESUME                       (1 << 16)
#define RFLAGS_VIRTUAL_8086                 (1 << 17)
#define RFLAGS_ALIGNMENT_CHECK              (1 << 18)
#define RFLAGS_ACCESS_CONTROL               (1 << 18)
#define RFLAGS_VIRTUAL_INTERRUPT            (1 << 19)
#define RFLAGS_VIRTUAL_INTERRUPT_PENDING    (1 << 20)
#define RFLAGS_ID                           (1 << 21)

typedef struct{
    void* kernel_stack;
    uint64_t cs;
    uint64_t ss;
    struct thread_t* thread;
}__attribute__((packed)) context_info_t;

typedef struct{
    uint64_t cr3;

    context_info_t* ctx_info;
    
    uint64_t rax;
    uint64_t rbx;
    uint64_t rcx;
    uint64_t rdx;
    uint64_t rsi;  
    uint64_t rdi;
    uint64_t rbp;

    uint64_t r8;
    uint64_t r9;
    uint64_t r10;
    uint64_t r11;
    uint64_t r12;
    uint64_t r13;
    uint64_t r14;
    uint64_t r15;

    uint64_t interrupt_number; 
    uint64_t error_code; 
    
    uint64_t rip; 
    uint64_t cs; 
    uint64_t rflags; 
    uint64_t rsp; 
    uint64_t ss;
}__attribute__((packed)) cpu_context_t; 
void cpu_init(void);
void cpu_set_context_info(context_info_t* info);
void * cpu_get_current_thread(void);
#endif