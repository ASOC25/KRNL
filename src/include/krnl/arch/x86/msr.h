#ifndef _X86_MSR_H
#define _X86_MSR_H

#include <krnl/libraries/std/stdint.h>

#define MSR_APIC                0x1B
#define MSR_EFER                0xC0000080
#define MSR_STAR                0xC0000081
#define MSR_LSTAR               0xC0000082
#define MSR_COMPAT_STAR         0xC0000083
#define MSR_SYSCALL_FLAG_MASK   0xC0000084
#define MSR_FS_BASE             0xC0000100
#define MSR_GS_BASE             0xC0000101
#define MSR_KERN_GS_BASE        0xc0000102

void msr_get(uint32_t msr, uint32_t *lo, uint32_t *hi);
void msr_set(uint32_t msr, uint32_t lo, uint32_t hi);
#endif