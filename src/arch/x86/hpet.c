//This code belongs to Kot!
//https://github.com/kot-org/Kot/blob/main/sources/core/kernel/source/arch/amd64/hpet.c

#include <krnl/arch/x86/hpet.h>
#include <krnl/arch/x86/acpi.h>
#include <krnl/mem/vmm.h>
#include <krnl/debug/debug.h>
#include <krnl/libraries/assert/assert.h>
#include <krnl/arch/x86/apic.h>

#define HPET_TIMER_OFFSET_GENERAL_CAPABILITIES_ID               0x0
#define HPET_TIMER_OFFSET_GENERAL_CONFIGURATION                 0x10
#define HPET_TIMER_OFFSET_MAIN_COUNTER_VALUES                   0xF0
#define HPET_GENERAL_CAPABILITIES_ID_COUNTER_PERIOD             0x20
#define HPET_GENERAL_CONFIGURATION_TIMER_INTERRUPT              0x0
#define HPET_GENERAL_CONFIGURATION_LEGACY_MAPPING               0x1
#define HPET_TIMER_OFFSET_TIMER_SPACE_DATA_START                0x100
#define HPET_TIMER_OFFSET_TIMER_SPACE_SIZE                      0x20
#define HPET_TIMER_OFFSET_TIMER_CONFIG_CAPABILITY_REGISTER      0x0
#define HPET_TIMER_OFFSET_TIMER_COMPARATOR_VALUE_REGISTER       0x8
#define HPET_TIMER_OFFSET_TIMER_FSB_INTERRUPT_ROUTE_REGISTER    0x10
#define COMPARATOR_0_REGS                                      0x100


static void* hpet_base;
static uint64_t hpet_frequency;
uint8_t hpet_is_initialized = 0;
uint8_t hpet_is_enabled = 0;


static uint64_t hpet_read_register(uint64_t offset){
    return *((volatile uint64_t*)((void*)((uint64_t)hpet_base + offset)));
}

static void hpet_write_register(uint64_t offset, uint64_t value){
    *((volatile uint64_t*)((void*)((uint64_t)hpet_base + offset))) = value;
}

static void hpet_change_main_timer_interrupt_state(uint8_t is_enable){
    uint64_t general_configuration_register_data = hpet_read_register(HPET_TIMER_OFFSET_GENERAL_CONFIGURATION);
    general_configuration_register_data |= is_enable << HPET_GENERAL_CONFIGURATION_TIMER_INTERRUPT; 
    hpet_write_register(HPET_TIMER_OFFSET_GENERAL_CONFIGURATION, general_configuration_register_data);
} 

void hpet_init(){
    if (hpet_is_initialized) return;

    acpi_hpet_header_t* hpet = acpi_get_headers()->hpet_header;
    if(hpet == 0){
        panic("HPET table not found");
    }
    hpet_base = (void*)vmm_to_device_map((uint64_t)hpet->address.address);

    hpet_frequency = hpet_read_register(HPET_TIMER_OFFSET_GENERAL_CAPABILITIES_ID) >> HPET_GENERAL_CAPABILITIES_ID_COUNTER_PERIOD;
    
    hpet_change_main_timer_interrupt_state(0);
    hpet_write_register(HPET_TIMER_OFFSET_MAIN_COUNTER_VALUES, 0);
    hpet_change_main_timer_interrupt_state(1);
    hpet_is_initialized = 1;
    hpet_is_enabled = 1;

}   

uint64_t femo_to_micro(uint64_t femto){
    return femto / 1000000000;
}

uint64_t micro_to_femo(uint64_t micro){
    return micro * 1000000000;
}

uint64_t hpet_get_resolution(void){
    assert(hpet_is_initialized);
    uint64_t counter_clk_perioid = hpet_read_register(HPET_TIMER_OFFSET_GENERAL_CAPABILITIES_ID) >> HPET_GENERAL_CAPABILITIES_ID_COUNTER_PERIOD;
    return femo_to_micro(counter_clk_perioid);
}

uint64_t hpet_get_current_time(void){
    assert(hpet_is_initialized);
    uint64_t current_time = femo_to_micro(hpet_read_register(HPET_TIMER_OFFSET_MAIN_COUNTER_VALUES) * hpet_frequency);
    return current_time;
}

void hpet_sleep(uint64_t us){
    assert(hpet_is_initialized);

    uint64_t end = hpet_read_register(HPET_TIMER_OFFSET_MAIN_COUNTER_VALUES) + (micro_to_femo(us)) / hpet_frequency;

    do {
        __asm__ volatile ("pause" : : : "memory");
    } while(hpet_read_register(HPET_TIMER_OFFSET_MAIN_COUNTER_VALUES) < end);

}

//Setup the hpet to generate periodic interrupts every femtos femtoseconds
//Use interrupt vector INT_SLEEP (0x41)
void arm_hpet_interrupt_timer(uint64_t us) {
    assert(hpet_is_initialized);

    hpet_change_main_timer_interrupt_state(0);
    hpet_write_register(HPET_TIMER_OFFSET_GENERAL_CONFIGURATION, 0);
    uint64_t counter_clk_perioid = hpet_read_register(HPET_TIMER_OFFSET_GENERAL_CAPABILITIES_ID) >> HPET_GENERAL_CAPABILITIES_ID_COUNTER_PERIOD;
    uint64_t ticks_per_ms = (micro_to_femo(us) / counter_clk_perioid);
    hpet_write_register(HPET_TIMER_OFFSET_MAIN_COUNTER_VALUES, 0);
    uint64_t timer0_config = (1 << 15) | (1 << 9) | (1 << 6) | (1 << 3) | (1 << 2); //Set periodic mode, enable interrupts, set interrupt type to level triggered, set to use main counter, enable timer
    hpet_write_register(COMPARATOR_0_REGS + HPET_TIMER_OFFSET_TIMER_CONFIG_CAPABILITY_REGISTER, timer0_config);
    hpet_write_register(COMPARATOR_0_REGS + HPET_TIMER_OFFSET_TIMER_COMPARATOR_VALUE_REGISTER, ticks_per_ms);
    //Write general configuration to enable hpet and legacy replacement
    uint64_t general_configuration = (1 << 0) | (1 << 1);
    hpet_write_register(HPET_TIMER_OFFSET_GENERAL_CONFIGURATION, general_configuration);
    hpet_change_main_timer_interrupt_state(1);
}

void hpet_enable() {
    if (hpet_is_enabled) return;
    hpet_change_main_timer_interrupt_state(1);
    apic_ioapic_mask(HPET_TIMER_IRQ, 1);
    hpet_is_enabled = 1;
}

void hpet_disable() {
    if (!hpet_is_enabled) return;
    hpet_change_main_timer_interrupt_state(0);
    apic_ioapic_mask(HPET_TIMER_IRQ, 0);
    hpet_is_enabled = 0;
}