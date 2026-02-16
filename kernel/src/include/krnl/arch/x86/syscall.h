#ifndef _X86_SYSCALL_H
#define _X86_SYSCALL_H

#define SYS_COUNT 51

#include <krnl/arch/x86/cpu.h>

void syscall_handler(cpu_context_t * context);

#endif