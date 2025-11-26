#ifndef _X86_SYSCALL_H
#define _X86_SYSCALL_H

#include <krnl/arch/x86/cpu.h>

void syscall_handler(cpu_context_t * context);

#endif