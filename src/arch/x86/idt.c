#include <krnl/arch/x86/idt.h>
#include <krnl/arch/x86/gdt.h>
#include <krnl/arch/x86/apic.h>
#include <krnl/arch/x86/cpu.h>
#include <krnl/debug/debug.h>

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
    kprintf("Interrupt context: %p on CPU %d\n", ctx, cpu_id);
    apic_local_eoi(cpu_id);
}