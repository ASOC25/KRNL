#include <krnl/arch/x86/apic.h>
#include <krnl/arch/x86/acpi.h>
#include <krnl/arch/x86/idt.h>
#include <krnl/arch/x86/msr.h>
#include <krnl/arch/x86/hpet.h>
#include <krnl/arch/x86/io.h>
#include <krnl/mem/vmm.h>
#include <krnl/debug/debug.h>
#include <krnl/libraries/assert/assert.h>

struct apic_context actx = {0};

void write_lapic_register(void * lapic_addr, uint64_t offset, uint32_t value) {
    *((volatile uint32_t*)((void*)((uint64_t)lapic_addr+offset))) = value;
}

uint32_t read_lapic_register(void * lapic_addr, uint64_t offset) {
    return *((volatile uint32_t*)((void*)((uint64_t)lapic_addr+offset)));
}

uint32_t ioapic_read_register(void* apic_ptr, uint8_t offset) {
    *(volatile uint32_t*)(apic_ptr) = offset;
    return *(volatile uint32_t*)((uint64_t)apic_ptr+0x10);
}

void ioapic_write_register(void* apic_ptr, uint8_t offset, uint32_t value) {
    *(volatile uint32_t*)(apic_ptr) = offset;
    *(volatile uint32_t*)((uint64_t)apic_ptr+0x10) = value;
}

extern uint8_t getApicId(void);

void io_change_irq_state(uint8_t irq, uint8_t io_apic_id, uint8_t is_enable){
    struct ioapic* ioapic = &actx.ioapic_entries[io_apic_id];
    if (ioapic == 0) {
        return;
    }
    uint64_t ioapic_address = (uint64_t)vmm_to_device_map(ioapic->ioapic_address);
    uint32_t base = ioapic->gsi_base;
    size_t index = irq - base;
    
    volatile uint32_t low = ioapic_read_register(
        (void*)ioapic_address,
        IOAPIC_REDIRECTION_TABLE + 2 * index
    );
    
    if(!is_enable){
        low |= 1 << IOAPIC_REDIRECTION_BITS_MASK;
    }else{
        low &= ~(1 << IOAPIC_REDIRECTION_BITS_MASK);
    }

    ioapic_write_register((void*)ioapic_address, IOAPIC_REDIRECTION_TABLE + 2 * index, low);
}

uint8_t apic_ioapic_mask(uint8_t irq, uint8_t enable) {
    assert(actx.initialized);
    if(irq >= 0x20 && (actx.ioapic_entries[0].max_interrupts + 0x20) > irq) {
        io_change_irq_state(irq - 0x20, 0, enable);
        return 1;
    }
    return 0;
}

void apic_enable_for_cpu(uint8_t apic_id) {
    uint32_t lo;
    uint32_t hi;
    msr_get(MSR_APIC, &lo, &hi);
    uint64_t base_addr = ((uint64_t)(hi) << 32) | (lo & 0xFFFFF000);
    if (base_addr == 0) {
        panic("APIC base address is 0");
    }
    actx.lapic_addresses[apic_id].physical_address = (void*)base_addr;
    actx.lapic_addresses[apic_id].virtual_address = (void*)vmm_to_identity_map(base_addr);

    void * lapic_address = actx.lapic_addresses[getApicId()].virtual_address;

    uint64_t current_local_destination = read_lapic_register(lapic_address, LAPIC_LOGICAL_DESTINATION);
    uint64_t current_svr = read_lapic_register(lapic_address, LAPIC_SPURIOUS_INTERRUPT_VECTOR);

    write_lapic_register(lapic_address, LAPIC_DESTINATION_FORMAT, 0xffffffff);
    write_lapic_register(lapic_address, LAPIC_LOGICAL_DESTINATION, ((current_local_destination & ~((0xff << 24))) | (apic_id << 24)));
    write_lapic_register(lapic_address, LAPIC_SPURIOUS_INTERRUPT_VECTOR, current_svr | (LOCAL_APIC_SPURIOUS_ALL | LOCAL_APIC_SPURIOUS_ENABLE_APIC));
    write_lapic_register(lapic_address, LAPIC_TASK_PRIORITY, 0);

    uint64_t value = ((uint64_t)actx.lapic_addresses[apic_id].physical_address) | (LOCAL_APIC_ENABLE & ~((1 << 10)));
    lo = value & 0xFFFFFFFF;
    hi = value >> 32;
    msr_set(MSR_APIC, lo, hi);
}

void ioapic_set_redirection_entry(void* apic_ptr, uint64_t index, struct ioapic_redirection_entry entry) {
    volatile uint32_t low = (
        (entry.vector << IOAPIC_REDIRECTION_BITS_VECTOR) |
        (entry.delivery_mode << IOAPIC_REDIRECTION_BITS_DELIVERY_MODE) |
        (entry.destination_mode << IOAPIC_REDIRECTION_BITS_DESTINATION_MODE) |
        (entry.delivery_status << IOAPIC_REDIRECTION_BITS_DELIVERY_STATUS) |
        (entry.pin_polarity << IOAPIC_REDIRECTION_BITS_PIN_POLARITY) |
        (entry.remote_irr << IOAPIC_REDIRECTION_BITS_REMOTE_IRR) |
        (entry.trigger_mode << IOAPIC_REDIRECTION_BITS_TRIGGER_MODE) |
        (entry.mask << IOAPIC_REDIRECTION_BITS_MASK)
    );

    volatile uint32_t high = (
        (entry.destination << IOAPIC_REDIRECTION_BITS_DESTINATION)
    );

    ioapic_write_register(apic_ptr, IOAPIC_REDIRECTION_TABLE + (index * 2), low);
    ioapic_write_register(apic_ptr, IOAPIC_REDIRECTION_TABLE + (index * 2) + 1, high);
}

void create_apic_context_entry(struct entry_record* record) {
    switch (record->type) {
        case MADT_RECORD_TYPE_LAPIC: {
            struct lapic* lapic_record = (struct lapic*)record;
            struct lapic* entry = &actx.lapic_entries[actx.lapic_count++];
            if (actx.lapic_count > APIC_ENTRIES_HARD_LIMIT) {
                panic("APIC initialization failed: too many LAPIC entries");
            }
            entry->record = lapic_record->record;
            entry->processor_id = lapic_record->processor_id;
            entry->apic_id = lapic_record->apic_id;
            entry->flags = lapic_record->flags;
            
            struct lapic_address* addr_entry = &actx.lapic_addresses[entry->apic_id];
            addr_entry->physical_address = NULL;
            addr_entry->virtual_address = NULL;
            break;
        }
        case MADT_RECORD_TYPE_IOAPIC: {
            struct ioapic* ioapic_record = (struct ioapic*)record;
            struct ioapic* entry = &actx.ioapic_entries[actx.ioapic_count++];
            if (actx.ioapic_count > APIC_ENTRIES_HARD_LIMIT) {
                panic("APIC initialization failed: too many IOAPIC entries");
            }
            entry->record = ioapic_record->record;
            entry->reserved = ioapic_record->reserved;
            entry->ioapic_id = ioapic_record->ioapic_id;
            entry->ioapic_address = ioapic_record->ioapic_address;
            entry->gsi_base = ioapic_record->gsi_base;
            entry->max_interrupts = ioapic_record->max_interrupts;
            break;
        }
        case MADT_RECORD_TYPE_IOAPIC_ISO: {
            struct ioapic_iso* iso_record = (struct ioapic_iso*)record;
            struct ioapic_iso* entry = &actx.ioapic_iso_entries[actx.ioapic_iso_count++];
            if (actx.ioapic_iso_count > APIC_ENTRIES_HARD_LIMIT) {
                panic("APIC initialization failed: too many IOAPIC ISO entries");
            }
            entry->record = iso_record->record;
            entry->bus_source = iso_record->bus_source;
            entry->irq_source = iso_record->irq_source;
            entry->gsi = iso_record->gsi;
            entry->flags = iso_record->flags;
            break;
        }
        default:
            //Unknown entry type, ignore
            break;
    }
}

void ioapic_init(uint64_t ioapic_id) {
    // disable pic
    outb(0xa1, 0xff);
    outb(0x21, 0xff);
    uint8_t apic_id = getApicId();
    apic_enable_for_cpu(apic_id);

    struct ioapic* ioapic_entry = &actx.ioapic_entries[ioapic_id];
    if (ioapic_entry == NULL || ioapic_entry->ioapic_address == 0) {
        panic("IOAPIC initialization failed: invalid IOAPIC entry");
    }
    void * ioapic_address = (void*)vmm_to_device_map((uint64_t)(ioapic_entry->ioapic_address));
    uint64_t ioapic_version = ioapic_read_register(ioapic_address, IOAPIC_VERSION);
    ioapic_entry->max_interrupts = ((ioapic_version >> 16) & 0xff) + 1;
    
    for (uint64_t i = 0; i < ioapic_entry->max_interrupts; i++) {
        struct ioapic_redirection_entry entry = {
            .vector = 0x20 + i,
            .delivery_mode = IOAPIC_REDIRECTION_DELIVERY_MODE_FIXED,
            .destination_mode = IOAPIC_REDIRECTION_DESTINATION_MODE_PHYSICAL,
            .delivery_status = IOAPIC_REDIRECTION_ENTRY_DELIVERY_STATUS_IDLE,
            .pin_polarity = IOAPIC_REDIRECTION_PIN_POLARITY_ACTIVE_HIGH,
            .remote_irr = IOAPIC_REDIRECTION_REMOTE_IRR_NONE,
            .trigger_mode = IOAPIC_REDIRECTION_TRIGGER_MODE_EDGE,
            .mask = IOAPIC_REDIRECTION_MASK_DISABLE,
            .destination = 0
        };

        ioapic_set_redirection_entry((void*)ioapic_address, i - ioapic_entry->gsi_base, entry);
    }

    for (uint64_t i = 0; i < actx.ioapic_iso_count; i++) {
        struct ioapic_iso* iso = &actx.ioapic_iso_entries[i];
        uint8_t irq_number = iso->irq_source + 0x20;
        struct ioapic_redirection_entry entry = {
            .vector = irq_number,
            .delivery_mode = IOAPIC_REDIRECTION_DELIVERY_MODE_FIXED,
            .destination_mode = IOAPIC_REDIRECTION_DESTINATION_MODE_PHYSICAL,
            .delivery_status = IOAPIC_REDIRECTION_ENTRY_DELIVERY_STATUS_IDLE,
            .pin_polarity = (iso->flags & 0x3) == 0x03 ? IOAPIC_REDIRECTION_PIN_POLARITY_ACTIVE_LOW : IOAPIC_REDIRECTION_PIN_POLARITY_ACTIVE_HIGH,
            .remote_irr = IOAPIC_REDIRECTION_REMOTE_IRR_NONE,
            .trigger_mode = (iso->flags & 0xc) == 0x0c ? IOAPIC_REDIRECTION_TRIGGER_MODE_LEVEL : IOAPIC_REDIRECTION_TRIGGER_MODE_EDGE,
            .mask = IOAPIC_REDIRECTION_MASK_DISABLE,
            .destination = 0
        };

        ioapic_set_redirection_entry((void*)ioapic_address, iso->irq_source, entry);
    }
}

uint32_t create_register_value_interrupts(struct local_apic_interrupt_register reg){
    return (
        (reg.vector << local_apic_interrupt_vector) |
        (reg.message_type << local_apic_interrupt_message_type) |
        (reg.delivery_status << local_apic_interrupt_delivery_status) |
        (reg.trigger_mode << local_apic_interrupt_triger_mode) |
        (reg.mask << local_apic_interrupt_mask) |
        (reg.timer_mode << local_apic_interrupt_timer_mode)
    );
}

void apic_start_lapic_timer(void){
    assert(actx.initialized);
    void * lapic_address = actx.lapic_addresses[getApicId()].virtual_address;
    // setup local apic timer
    write_lapic_register(lapic_address, local_apic_register_offset_divide, 0x3); // divide by 16    
    write_lapic_register(lapic_address, local_apic_register_offset_initial_count, 0xffffffff);

    hpet_sleep(10000); // sleep 10ms
    
    write_lapic_register(lapic_address, local_apic_register_offset_lvt_timer, LAPIC_LVT_INT_MASKED); // mask timer interrupt
    uint32_t ticksin10ms = 0xffffffff - read_lapic_register(lapic_address, local_apic_register_offset_curent_count);

    struct local_apic_interrupt_register timer_registers;

    /* don't forget to define all the struct because it can be corrupt by the stack */
    timer_registers.vector = INT_SCHEDULE_APIC_TIMER;
    timer_registers.message_type = local_apic_interrupt_register_message_type_fixed;
    timer_registers.delivery_status = local_apic_interrupt_register_message_type_idle;
    timer_registers.remote_irr = local_apic_interrupt_register_remote_irr_completed;
    timer_registers.trigger_mode = local_apic_interrupt_register_trigger_mode_edge;
    timer_registers.mask = local_apic_interrupt_register_mask_enable;
    timer_registers.timer_mode = local_apic_interrupt_timer_mode_one_shot;
    
    uint32_t timer = read_lapic_register(lapic_address, local_apic_register_offset_lvt_timer);
    write_lapic_register(lapic_address, local_apic_register_offset_lvt_timer, create_register_value_interrupts(timer_registers) | (timer & 0xfffcef00));    
    write_lapic_register(lapic_address, local_apic_register_offset_initial_count, ticksin10ms * 10); // set timer for 10ms

    actx.lapic_timer_ticks_per_ms = ticksin10ms / 10;
}

void apic_arm_lapic_timer(uint8_t cpu_id, uint32_t ms) {
    assert(cpu_id < actx.lapic_count);
    assert(ms > 0);
    if (!actx.initialized || !actx.lapic_addresses[cpu_id].virtual_address) {
        panic("apic_arm_lapic_timer: Invalid CPU ID or LAPIC address not initialized");
    }

    uint32_t ticks = actx.lapic_timer_ticks_per_ms * ms;
    void * lapic_address = actx.lapic_addresses[cpu_id].virtual_address;
    //uint32_t remaining_ticks = read_lapic_register(lapic_address, local_apic_register_offset_curent_count);
    //uint32_t remaining_ms = remaining_ticks / actx.lapic_timer_ticks_per_ms;
    //Print ms remaining ticks and total ticks
    write_lapic_register(lapic_address, local_apic_register_offset_initial_count, ticks);
    //kprintf("APIC Timer Arm: CPU %d, remaining ms: %d, rearming for %d ms \n", cpu_id, remaining_ms, ms);
}

void apic_init(void) {
    if (actx.initialized) {
        apic_enable_for_cpu(getApicId());
        return;
    }


    acpi_madt_header_t* madt = acpi_get_headers()->madt_header;
    if (madt == 0) {
        panic("APIC initialization failed: MADT not found");
    }

    uint64_t entry = (uint64_t)madt + sizeof(acpi_madt_header_t);
    uint64_t end = (uint64_t)madt + madt->header.length;

    while (entry < end) {
        struct entry_record* record = (struct entry_record*)entry;
        create_apic_context_entry(record);
        entry += record->length;
    }

    for (uint64_t i = 0; i < actx.ioapic_count; i++) {
        ioapic_init(i);
    }
    actx.initialized = 1;
}

void apic_local_eoi(uint8_t cpu_id) {
    assert(actx.initialized);
    assert(cpu_id < actx.lapic_count);
    assert(actx.lapic_addresses[cpu_id].virtual_address != NULL);
    write_lapic_register(actx.lapic_addresses[cpu_id].virtual_address, LAPIC_EOI, 0);
}