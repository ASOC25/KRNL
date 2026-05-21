#include <krnl/arch/x86/idt.h>
#include <krnl/arch/x86/gdt.h>
#include <krnl/arch/x86/apic.h>
#include <krnl/arch/x86/cpu.h>
#include <krnl/debug/debug.h>
#include <krnl/process/scheduler.h>
#include <krnl/process/process.h>
#include <krnl/arch/x86/hpet.h>
#include <krnl/mem/mmap.h>
#include <krnl/process/signals.h>
#include <krnl/debug/kernel_syms.h>

extern void* __interrupt_vector[IDT_ENTRY_COUNT];
static __attribute__((aligned(IDT_PAGE_SIZE))) idt_t global_idt = {};
extern void _load_idt(idtr_t* idtr);
uint8_t idt_initialized = 0;

#define DYNAMIC_INTERRUPT_HANDLERS_NO 256
//Array of function pointers for interrupt handlers
void (*dynamic_interrupt_handlers[DYNAMIC_INTERRUPT_HANDLERS_NO])(cpu_context_t* ctx, uint8_t cpu_id) = {0};

static idtr_t _idtr = {
    .limit = sizeof(idt_t) - 1,
    .base = (uint64_t) &global_idt,
};

idt_entry_t idt_entry(void* handler, uint8_t ist, uint8_t idt_flags) {
    return (idt_entry_t) {
        .offset_low = ((uint64_t)handler) & 0xffff,
        .code_segment = GDT_KERNEL_CODE * sizeof(gdt_entry_t),
        .ist = ist,
        .attributes = idt_flags,
        .offset_middle = ((uint64_t)handler >> 16) & 0xffff,
        .offset_high = ((uint64_t)handler >> 32) & 0xffffffff,
        .zero = 0,
    };
}

void undefined_exception(cpu_context_t * ctx, uint8_t cpu_id) {
    panic("Undefined CPU EXCEPTION: %d on CPU %d", ctx->interrupt_number, cpu_id);
}

void register_dynamic_interrupt(uint8_t vector, void* handler) {
    dynamic_interrupt_handlers[vector] = handler;
}

void unregister_dynamic_interrupt(uint8_t vector) {
    dynamic_interrupt_handlers[vector] = undefined_exception;
}

void idt_init(void) {
    if (idt_initialized) {
        _load_idt(&_idtr);
        return;
    }

    for (uint16_t i = 0; i < 256; i++) {
        global_idt.entries[i] = idt_entry(__interrupt_vector[i], 0, 0x8e);
    }
    _load_idt(&_idtr);

    for (uint16_t i = 0; i < DYNAMIC_INTERRUPT_HANDLERS_NO; i++) {
        dynamic_interrupt_handlers[i] = undefined_exception;
    }
    idt_initialized = 1;
}

struct stackframe {
    struct stackframe *rbp;
    uint64_t rip;
};

static const char *exception_names[] = {
    "#DE  Division Error",
    "#DB  Debug",
    "     NMI Non-Maskable Interrupt",
    "#BP  Breakpoint",
    "#OF  Overflow",
    "#BR  Bound Range Exceeded",
    "#UD  Invalid Opcode",
    "#NM  Device Not Available",
    "#DF  Double Fault",
    "     Coprocessor Segment Overrun",
    "#TS  Invalid TSS",
    "#NP  Segment Not Present",
    "#SS  Stack Segment Fault",
    "#GP  General Protection Fault",
    "#PF  Page Fault",
    "     Reserved",
    "#MF  x87 Floating-Point",
    "#AC  Alignment Check",
    "#MC  Machine Check",
    "#XM  SIMD Floating-Point",
    "#VE  Virtualization",
    "#CP  Control Protection",
};
#define EXCEPTION_NAMES_COUNT ((int)(sizeof(exception_names) / sizeof(exception_names[0])))

static void exception_dump_rflags(uint64_t rflags) {
    kprintf("RFLAGS: 0x%016llx  [", rflags);
    if (rflags & (1 << 21)) kprintf("ID ");
    if (rflags & (1 << 19)) kprintf("VIP ");
    if (rflags & (1 << 18)) kprintf("VIF ");
    if (rflags & (1 << 17)) kprintf("AC ");
    if (rflags & (1 << 16)) kprintf("RF ");
    if (rflags & (1 << 14)) kprintf("NT ");
    if (rflags & (3 << 12)) kprintf("IOPL%llu ", (rflags >> 12) & 3);
    if (rflags & (1 << 11)) kprintf("OF ");
    if (rflags & (1 << 10)) kprintf("DF ");
    if (rflags & (1 <<  9)) kprintf("IF ");
    if (rflags & (1 <<  8)) kprintf("TF ");
    if (rflags & (1 <<  7)) kprintf("SF ");
    if (rflags & (1 <<  6)) kprintf("ZF ");
    if (rflags & (1 <<  4)) kprintf("AF ");
    if (rflags & (1 <<  2)) kprintf("PF ");
    if (rflags & (1 <<  0)) kprintf("CF ");
    kprintf("]\n");
}

static void exception_dump_registers(cpu_context_t *ctx) {
    uint64_t cr0, cr2, cr3, cr4;
    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));

    kprintf("  RAX: 0x%016llx  RBX: 0x%016llx\n", ctx->rax, ctx->rbx);
    kprintf("  RCX: 0x%016llx  RDX: 0x%016llx\n", ctx->rcx, ctx->rdx);
    kprintf("  RSI: 0x%016llx  RDI: 0x%016llx\n", ctx->rsi, ctx->rdi);
    kprintf("  RBP: 0x%016llx  RSP: 0x%016llx\n", ctx->rbp, ctx->rsp);
    kprintf("  R8:  0x%016llx  R9:  0x%016llx\n", ctx->r8,  ctx->r9);
    kprintf("  R10: 0x%016llx  R11: 0x%016llx\n", ctx->r10, ctx->r11);
    kprintf("  R12: 0x%016llx  R13: 0x%016llx\n", ctx->r12, ctx->r13);
    kprintf("  R14: 0x%016llx  R15: 0x%016llx\n", ctx->r14, ctx->r15);
    kprintf("  RIP: 0x%016llx  ERR: 0x%016llx\n", ctx->rip, ctx->error_code);
    kprintf("  CS:  0x%04llx                SS:  0x%04llx\n", ctx->cs, ctx->ss);
    kprintf("  CR0: 0x%016llx  CR2: 0x%016llx\n", cr0, cr2);
    kprintf("  CR3: 0x%016llx  CR4: 0x%016llx\n", cr3, cr4);
    exception_dump_rflags(ctx->rflags);
}

static void exception_decode_error(cpu_context_t *ctx) {
    uint64_t vec = ctx->interrupt_number;
    uint64_t err = ctx->error_code;

    if (vec == 14) {
        /* Page Fault error code */
        kprintf("  PF cause: %s | %s | %s",
            (err & 0x1) ? "protection-violation" : "not-present",
            (err & 0x2) ? "write"                : "read",
            (err & 0x4) ? "user-mode"            : "kernel-mode");
        if (err & 0x08) kprintf(" | reserved-bit-set");
        if (err & 0x10) kprintf(" | instruction-fetch");
        if (err & 0x20) kprintf(" | protection-key");
        if (err & 0x40) kprintf(" | shadow-stack");
        kprintf("\n");

        uint64_t cr2;
        __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));

        /* Stack proximity check — skip on instruction-fetch faults (bit 4), as
           those are bad-pointer calls, not stack accesses. */
        if (!(err & 0x10)) {
            thread_t *thread = ctx->ctx_info ? (thread_t *)ctx->ctx_info->thread : 0;
            if (thread && thread->ustack) {
                stack_t *us = thread->ustack;
                uint64_t base = (uint64_t)us->base;
                uint64_t top  = (uint64_t)us->top;
                uint64_t guard = 0x8000; /* 32 KB proximity window */
                if      (cr2 >= base - guard && cr2 < base) kprintf("  Stack overflow  (CR2 0x%llx near ustack base 0x%llx)\n", cr2, base);
                else if (cr2 >= top && cr2 < top + guard)   kprintf("  Stack underflow (CR2 0x%llx near ustack top  0x%llx)\n", cr2, top);
            }
        }
    } else if (vec == 13 || vec == 10 || vec == 11 || vec == 12) {
        /* Selector-based error codes */
        if (err == 0) {
            kprintf("  Error code: 0 (no specific selector)\n");
        } else {
            const char *tbl = (err & 0x6) == 0x0 ? "GDT"
                            : (err & 0x6) == 0x2 ? "LDT"
                            : "IDT";
            kprintf("  Selector: index=%llu table=%s external=%llu\n",
                (err >> 3) & 0x1FFF, tbl, err & 0x1);
        }
    }
}

static const char * resolve_any_symbol(process_t *proc, uint64_t addr) {
    /* Kernel addresses: use the embedded kernel symbol table */
    if (addr >= 0xffffffff80000000ULL)
        return kernel_resolve_symbol(addr);
    /* User addresses: use per-process symbol tables */
    return process_resolve_symbol(proc, addr);
}

static void exception_stacktrace(cpu_context_t *ctx, unsigned int max_frames) {
    process_t * proc = NULL;
    if (ctx->ctx_info && ctx->ctx_info->thread)
        proc = (process_t *)((thread_t *)ctx->ctx_info->thread)->process;

    // Lazily load shared-library symbol tables from the process's link map
    if (proc) process_load_shlib_symtabs(proc);

    const char * sym = resolve_any_symbol(proc, ctx->rip);
    kprintf("  #0  0x%016llx  <%s>\n", ctx->rip, sym ? sym : "RIP at fault");

    struct stackframe *frame = (struct stackframe *)ctx->rbp;
    for (unsigned int i = 1; i <= max_frames; i++) {
        if (!frame || (uint64_t)frame < 0x1000 || ((uint64_t)frame & 0x7))
            break;
        sym = resolve_any_symbol(proc, frame->rip);
        if (sym)
            kprintf("  #%-2u 0x%016llx  <%s>\n", i, frame->rip, sym);
        else
            kprintf("  #%-2u 0x%016llx\n", i, frame->rip);
        frame = frame->rbp;
    }
}

void exception(cpu_context_t *ctx) {
    __asm__ volatile("cli");

    uint64_t vec = ctx->interrupt_number;
    const char *name = (vec < (uint64_t)EXCEPTION_NAMES_COUNT)
                       ? exception_names[vec] : "Unknown Exception";

    kprintf("\n====== CPU EXCEPTION #%llu %s ======\n", vec, name);

    /* Process / thread context */
    thread_t *thread = ctx->ctx_info ? (thread_t *)ctx->ctx_info->thread : 0;
    if (thread) {
        process_t *proc = (process_t *)thread->process;
        if (proc) {
            const char *argv0 = (proc->argv && proc->argv[0]) ? proc->argv[0] : "<none>";
            kprintf("  Process: PID=%d  PPID=%d  argv[0]=%s\n",
                    proc->pid, proc->ppid, argv0);
            kprintf("  CWD: %s\n", proc->cwd.internal_path);
        }
        kprintf("  Thread:  TID=%d\n", thread->tid);
        if (thread->ustack) {
            stack_t *us = thread->ustack;
            kprintf("  UStack:  base=0x%llx  top=0x%llx  size=0x%llx\n",
                    (uint64_t)us->base, (uint64_t)us->top,
                    (uint64_t)us->top - (uint64_t)us->base);
        }
    } else {
        kprintf("  (kernel context — no thread)\n");
    }

    kprintf("--- Registers ---\n");
    exception_dump_registers(ctx);

    kprintf("--- Error code ---\n");
    exception_decode_error(ctx);

    kprintf("--- Stack trace ---\n");
    exception_stacktrace(ctx, 16);

    kprintf("======================================\n\n");

    panic("CPU EXCEPTION #%llu %s at RIP 0x%llx", vec, name, ctx->rip);
}

//uint64_t last_us = 0;
void interrupt_handler(cpu_context_t* ctx, uint8_t cpu_id) {
    if (ctx->interrupt_number == 13) {
        //General Protection Fault
        kprintf("General Protection Fault on CPU %d\n", cpu_id);
        exception(ctx);
    } else if (ctx->interrupt_number == 14) {
        //Page fault
        uint64_t cr2;
        __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
        // Bit 2 of the page fault error code (U/S) is 0 for supervisor-mode
        // faults. A kernel-mode fault is never a user-space COW fault.
        if (!(ctx->error_code & 0x4)) {
            exception(ctx);
        }
        process_t * current_process = 0x0;
        thread_t *  current_thread = (thread_t *)ctx->ctx_info->thread;
        if (current_thread) {
            current_process = current_thread->process;
        } else {
            exception(ctx);
        }
        status_t status = vmarea_try_cow(current_process, (void *)cr2);
        if (status == SUCCESS) {
            apic_local_eoi(cpu_id);
            return;
        } else {
            exception(ctx);
        }
    } else if (ctx->interrupt_number == HPET_TIMER_IRQ) {
        //HPET System Timer Interrupt
        //uint64_t current_us = hpet_get_current_time();
        //kprintf("Elapsed: %llu microseconds\n", current_us - last_us);
        //last_us = current_us;
        update_counters();
    } else if (ctx->interrupt_number == INT_SCHEDULE_APIC_TIMER) {
        // Send EOI *before* the context switch so it is never bypassed by a
        // thread that never returns here.  Only send it when the LAPIC ISR
        // actually has this vector in service (i.e. a real hardware timer
        // interrupt); skip the EOI for software int $0x40 (e.g. syscall_exit)
        // to avoid spuriously acknowledging an unrelated in-service interrupt.
        if (apic_lapic_vector_in_service(cpu_id, INT_SCHEDULE_APIC_TIMER)) {
            apic_local_eoi(cpu_id);
        }
        /* If the timer interrupted kernel-mode code (CS == 0x8) treat the
         * saved frame as a kernel context, not a user context.  This happens
         * when the timer fires inside the int $0x81 sti;hlt;cli sleep loop. */
        uint8_t ctx_type = (ctx->cs == GDT_KERNEL_CODE * sizeof(gdt_entry_t))
                           ? SCHEDULER_KERNEL_CONTEXT
                           : SCHEDULER_USER_CONTEXT;
        scheduler_handler(ctx, cpu_id, ctx_type, 1);
        return;
    } else if (ctx->interrupt_number == SIGNAL_SLEEP_INTERRUPT) {
        if (apic_lapic_vector_in_service(cpu_id, SIGNAL_SLEEP_INTERRUPT)) {
            apic_local_eoi(cpu_id);
        }
        scheduler_handler(ctx, cpu_id, SCHEDULER_KERNEL_CONTEXT, 1);
        return;
    } else if (ctx->interrupt_number < 32) {
        exception(ctx);
    } else {
        //Dynamic interrupt
        void (*handler)(cpu_context_t* ctx, uint8_t cpu_id) = dynamic_interrupt_handlers[ctx->interrupt_number];
        if (handler) {
            handler(ctx, cpu_id);
        } else {
            panic("No handler for interrupt %d on CPU %d", ctx->interrupt_number, cpu_id);
        }
    }
    apic_local_eoi(cpu_id);
    return;
}