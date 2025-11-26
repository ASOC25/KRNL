#include <krnl/arch/x86/idt.h>
#include <krnl/arch/x86/gdt.h>
#include <krnl/arch/x86/apic.h>
#include <krnl/arch/x86/cpu.h>
#include <krnl/debug/debug.h>
#include <krnl/process/scheduler.h>

extern void* __interrupt_vector[IDT_ENTRY_COUNT];
static __attribute__((aligned(IDT_PAGE_SIZE))) idt_t global_idt = {};
extern void _load_idt(idtr_t* idtr);
uint8_t idt_initialized = 0;

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

void idt_init(void) {
    if (idt_initialized) {
        _load_idt(&_idtr);
        return;
    }

    for (uint16_t i = 0; i < 256; i++) {
        global_idt.entries[i] = idt_entry(__interrupt_vector[i], 0, 0x8e);
    }
    _load_idt(&_idtr);
    idt_initialized = 1;
}

void interrupt_handler(cpu_context_t* ctx, uint8_t cpu_id) {
    if (ctx->interrupt_number < 32) {
        panic("Unhandled CPU Exception: %d", ctx->interrupt_number);
    } else if (ctx->interrupt_number >= 32 && ctx->interrupt_number < 48) {
        panic("Unhandled IRQ: %d", ctx->interrupt_number - 32);
        //apic_handle_interrupt(ctx->interrupt_number, cpu_id);
    } else if (ctx->interrupt_number == INT_SCHEDULE_APIC_TIMER) {
        scheduler_handler(ctx, cpu_id);
    } else {
        panic("Unknown Interrupt: %d", ctx->interrupt_number);
    }
    apic_local_eoi(cpu_id);
    return;
}