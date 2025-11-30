//This code belongs to Kot!
//https://github.com/kot-org/Kot/blob/main/sources/core/kernel/source/arch/amd64/hpet.c

#include <krnl/arch/x86/hpet.h>
#include <krnl/arch/x86/acpi.h>
#include <krnl/mem/vmm.h>
#include <krnl/debug/debug.h>

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


static void* hpet_base;
static uint64_t hpet_frequency;
uint8_t hpet_is_initialized = 0;


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

}   

uint64_t femo_to_micro(uint64_t femto){
    return femto / 1000000000;
}

uint64_t micro_to_femo(uint64_t micro){
    return micro * 1000000000;
}

uint64_t hpet_get_current_time(void){
    return femo_to_micro(hpet_read_register(HPET_TIMER_OFFSET_MAIN_COUNTER_VALUES) * hpet_frequency);
}

void hpet_sleep(uint64_t us){
    uint64_t end = hpet_read_register(HPET_TIMER_OFFSET_MAIN_COUNTER_VALUES) + (micro_to_femo(us)) / hpet_frequency;
    while(hpet_read_register(HPET_TIMER_OFFSET_MAIN_COUNTER_VALUES) <= end){
        __asm__ volatile ("pause" : : : "memory");
    }
}