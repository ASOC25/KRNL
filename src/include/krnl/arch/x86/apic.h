#ifndef _APIC_H
#define _APIC_H

#include <krnl/libraries/std/stdint.h>

#define APIC_ENTRIES_HARD_LIMIT 256

#define MADT_RECORD_TYPE_LAPIC                  0
#define MADT_RECORD_TYPE_IOAPIC                 1
#define MADT_RECORD_TYPE_IOAPIC_ISO             2
#define MADT_RECORD_TYPE_IOAPIC_NMI             3
#define MADT_RECORD_TYPE_LAPIC_NMI              4
#define MADT_RECORD_TYPE_LAPIC_ADDR_OVERRIDE    5
#define MADT_RECORD_TYPE_PLX2APIC               9

#define MADT_LAPIC_FLAGS_ENABLED                1

#define MADT_IOAPIC_ISO_FLAGS_ACTIVE_LOW        0x2
#define MADT_IOAPIC_ISO_FLAGS_LEVEL_TRIGGERED   0x8

#define LOCAL_APIC_ENABLE                       0x800
#define LOCAL_APIC_SPURIOUS_ALL                 0x100
#define LOCAL_APIC_SPURIOUS_ENABLE_APIC         0xFF

#define LAPIC_TASK_PRIORITY                     0x80
#define LAPIC_LOGICAL_DESTINATION               0xD0
#define LAPIC_DESTINATION_FORMAT                0xE0
#define LAPIC_SPURIOUS_INTERRUPT_VECTOR         0xF0

#define LAPIC_EOI                               0xB0

#define LAPIC_LVT_INT_MASKED                 0x10000

#define IOAPIC_VERSION                          0x01

#define IOAPIC_REDIRECTION_BITS_VECTOR              0x00
#define IOAPIC_REDIRECTION_BITS_DELIVERY_MODE       0x08
#define IOAPIC_REDIRECTION_BITS_DESTINATION_MODE    0x0B
#define IOAPIC_REDIRECTION_BITS_DELIVERY_STATUS     0x0C
#define IOAPIC_REDIRECTION_BITS_PIN_POLARITY        0x0D
#define IOAPIC_REDIRECTION_BITS_REMOTE_IRR          0x0E
#define IOAPIC_REDIRECTION_BITS_TRIGGER_MODE        0x0F
#define IOAPIC_REDIRECTION_BITS_MASK                0x10
#define IOAPIC_REDIRECTION_BITS_DESTINATION         0x18

#define IOAPIC_REDIRECTION_DELIVERY_MODE_FIXED      0x0
#define IOAPIC_REDIRECTION_DELIVERY_MODE_LOWEST     0x1
#define IOAPIC_REDIRECTION_DELIVERY_MODE_SMI        0x2
#define IOAPIC_REDIRECTION_DELIVERY_MODE_NMI        0x4
#define IOAPIC_REDIRECTION_DELIVERY_MODE_INIT       0x5
#define IOAPIC_REDIRECTION_DELIVERY_MODE_STARTUP    0x6

#define IOAPIC_REDIRECTION_DESTINATION_MODE_PHYSICAL    0x0
#define IOAPIC_REDIRECTION_DESTINATION_MODE_LOGICAL     0x1

#define IOAPIC_REDIRECTION_ENTRY_DELIVERY_STATUS_IDLE   0x0
#define IOAPIC_REDIRECTION_ENTRY_DELIVERY_STATUS_PENDING    0x1

#define IOAPIC_REDIRECTION_PIN_POLARITY_ACTIVE_HIGH  0x0
#define IOAPIC_REDIRECTION_PIN_POLARITY_ACTIVE_LOW   0x1

#define IOAPIC_REDIRECTION_REMOTE_IRR_NONE          0x0
#define IOAPIC_REDIRECTION_REMOTE_IRR_INFLIGHT       0x1

#define IOAPIC_REDIRECTION_TRIGGER_MODE_EDGE         0x0
#define IOAPIC_REDIRECTION_TRIGGER_MODE_LEVEL        0x1

#define IOAPIC_REDIRECTION_MASK_ENABLE               0x0
#define IOAPIC_REDIRECTION_MASK_DISABLE              0x1

#define IOAPIC_REDIRECTION_TABLE              0x10

#define local_apic_interrupt_register_message_type_fixed   0x0
#define local_apic_interrupt_register_message_type_idle    0x0
#define local_apic_interrupt_register_remote_irr_completed 0x0
#define local_apic_interrupt_register_trigger_mode_edge 0x0
#define local_apic_interrupt_register_mask_enable 0x0
#define local_apic_interrupt_timer_mode_periodic 0x1
#define local_apic_interrupt_timer_mode_one_shot 0x0
#define local_apic_register_offset_lvt_timer 0x320
#define local_apic_register_offset_initial_count 0x380
#define local_apic_register_offset_divide			       0x3E0
#define local_apic_register_offset_curent_count          0x390

#define local_apic_interrupt_vector 0
#define local_apic_interrupt_message_type 8
#define local_apic_interrupt_delivery_status 12
#define local_apic_interrupt_triger_mode 15
#define local_apic_interrupt_mask 16
#define local_apic_interrupt_timer_mode 17

struct local_apic_interrupt_register{
    uint8_t	vector:8;
    uint8_t message_type:3;
    uint8_t delivery_status:1;
    uint8_t remote_irr:1;
    uint8_t trigger_mode:1;
    uint8_t	mask:1;
    uint8_t timer_mode:1;
};

struct entry_record {
    uint8_t type;
    uint8_t length;
} __attribute__((packed));

struct lapic {
    struct entry_record record;
    uint8_t processor_id;
    uint8_t apic_id;
    uint32_t flags; //bit 0: processor enabled, bit 1: online capable
} __attribute__((packed));

struct ioapic {
    struct entry_record record;
    uint8_t ioapic_id;
    uint8_t reserved;
    uint32_t ioapic_address;
    uint32_t gsi_base;
    uint8_t max_interrupts;
} __attribute__((packed));

struct ioapic_iso {
    struct entry_record record;
    uint8_t bus_source;
    uint8_t irq_source;
    uint32_t gsi;
    uint16_t flags;
} __attribute__((packed));

struct ioapic_nmi {
    struct entry_record record;
    uint8_t nmi_source;
    uint8_t reserved;
    uint16_t flags;
    uint32_t gsi;
} __attribute__((packed));

struct lapic_nmi {
    struct entry_record record;
    uint8_t processor_id; //0xFF for all
    uint16_t flags;
    uint8_t lint; //0 or 1
} __attribute__((packed));

struct lapic_addr_override {
    struct entry_record record;
    uint16_t reserved;
    uint64_t address;
} __attribute__((packed));

struct plx2apic {
    struct entry_record record;
    uint16_t reserved;
    uint32_t processor_local_x2apic_id;
    uint32_t flags;
    uint32_t acpi_id;
} __attribute__((packed));

struct lapic_address {
    void * physical_address;
    void * virtual_address;
}__attribute__((packed));

struct ioapic_redirection_entry {
    uint16_t vector:8;
    uint16_t delivery_mode:3;
    uint16_t destination_mode:1;
    uint16_t delivery_status:1;
    uint16_t pin_polarity:1;
    uint16_t remote_irr:1;
    uint16_t trigger_mode:1;
    uint16_t mask:1;
    uint16_t destination:8;
}__attribute__((packed));

struct apic_context {
    struct lapic lapic_entries[APIC_ENTRIES_HARD_LIMIT];
    struct lapic_address lapic_addresses[APIC_ENTRIES_HARD_LIMIT];
    uint16_t lapic_count;

    struct ioapic ioapic_entries[APIC_ENTRIES_HARD_LIMIT];
    uint16_t ioapic_count;
    struct ioapic_iso ioapic_iso_entries[APIC_ENTRIES_HARD_LIMIT];
    uint16_t ioapic_iso_count;

    uint32_t lapic_timer_ticks_per_ms;

    uint8_t initialized;
};

void apic_init(void);
void apic_local_eoi(uint8_t cpu_id);
void apic_start_lapic_timer(void);
uint8_t apic_ioapic_mask(uint8_t irq, uint8_t enable);
void apic_arm_lapic_timer(uint8_t cpu_id, uint32_t ms);

#endif