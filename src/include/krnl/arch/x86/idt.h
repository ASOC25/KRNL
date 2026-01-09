#ifndef _X86_IDT_H
#define _X86_IDT_H

#include <krnl/libraries/std/stdint.h>

//Credits to https://github.com/kot-org/Kot/blob/main/sources/core/kernel/source/arch/amd64/idt.h

#define IDT_PAGE_SIZE 4096
#define IDT_ENTRY_COUNT (IDT_PAGE_SIZE / sizeof(idt_entry_t))

#define INT_SCHEDULE_APIC_TIMER 0x40

typedef struct {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed)) idtr_t;

typedef struct {
    uint16_t offset_low;
    uint16_t code_segment;
    uint8_t ist;
    uint8_t attributes;
    uint16_t offset_middle;
    uint32_t offset_high;
    uint32_t zero;
} __attribute__((packed)) idt_entry_t;

typedef struct {
    idt_entry_t entries[IDT_ENTRY_COUNT];
} __attribute__((packed)) idt_t;

void idt_init(void);
void idt_update(idtr_t* idtr);
void register_dynamic_interrupt(uint8_t vector, void* handler);
void unregister_dynamic_interrupt(uint8_t vector);
#endif