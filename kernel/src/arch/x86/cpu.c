#include <krnl/arch/x86/cpu.h>
#include <krnl/arch/x86/gdt.h>
#include <krnl/arch/x86/idt.h>
#include <krnl/arch/x86/apic.h>
#include <krnl/arch/x86/hpet.h>
#include <krnl/arch/x86/syscall.h>
#include <krnl/mem/allocator.h>
#include <krnl/libraries/std/string.h>
#include <krnl/mem/vmm.h>
#include <krnl/boot/bootloaders/bootloader.h>
#include <krnl/debug/debug.h>

#define SIMD_CONTEXT_SIZE 512
#define CR0_MONITOR_COPROC (1 << 1)
#define CR0_EM (1 << 2)
#define CR0_NUMERIC_ERROR (1 << 5)
#define CR4_FXSR (1 << 9)
#define CR4_SIMD_EXCEPTION (1 << 10)

extern uint8_t getApicId(void);
extern void reload_gs_fs(void);
extern void set_cpu_gs_base(uint64_t addr);
extern uint64_t get_cpu_gs_base(void);
extern void syscall_enable(uint16_t kernel_cs, uint16_t user_cs);
typedef struct core_context {
    uint64_t core_id;
    context_info_t * cinfo;
    uint64_t ustack;
    tss_t* cpu_tss;
} __attribute__((packed)) core_context_t;

void simd_init() {
    uint64_t cr0;
    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    cr0 &= ~((uint64_t)CR0_EM);
    cr0 |= CR0_MONITOR_COPROC;
    cr0 |= CR0_NUMERIC_ERROR;
    __asm__ volatile("mov %0, %%cr0" :: "r"(cr0));

    uint64_t cr4;
    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
    cr4 |= CR4_FXSR;
    cr4 |= CR4_SIMD_EXCEPTION;
    __asm__ volatile("mov %0, %%cr4" :: "r"(cr4));
    __asm__ volatile("finit");
}

void cpu_tss_init(core_context_t * cpu_ctx) {
    cpu_ctx->cpu_tss = (tss_t*)kmalloc(sizeof(tss_t));
    stack_t * kernel_stack = kstackalloc(vmm_get_root(), KERNEL_STACK_SIZE);
    memset(kernel_stack->base, 0, KERNEL_STACK_SIZE);
    memset(cpu_ctx->cpu_tss, 0, sizeof(tss_t));
    cpu_ctx->cpu_tss->rsp[0] = (uint64_t)(kernel_stack->top);
    gdt_load_tss(cpu_ctx->cpu_tss);
}

void cpu_context_init() {
    core_context_t * ctx = (core_context_t *)kmalloc(sizeof(core_context_t));
    ctx->core_id = getApicId();
    ctx->cinfo = (context_info_t *)kmalloc(sizeof(context_info_t));
    reload_gs_fs();
    set_cpu_gs_base((uint64_t)ctx);
    cpu_tss_init(ctx);
}

void cpu_set_context_info(context_info_t* info) {
    core_context_t * ctx = (core_context_t *)get_cpu_gs_base();
    ctx->cinfo = info;
}

void callback(boot_smp_info_t *lcpu) {
    (void)lcpu;
    while (1) {
       __asm__("hlt");
    }
}

void cpu_init_id(uint64_t cpu_id, uint64_t lapic_id) {
    (void)cpu_id;
    (void)lapic_id;
    simd_init();
    gdt_init();
    idt_init();
    //ACPI IS AUTOMATICALLY INITIALIZED ON DEMAND OF ANY TABLE
    apic_init();
    cpu_context_init();
    syscall_enable(GDT_KERNEL_CODE * sizeof(gdt_entry_t), GDT_USER_CODE * sizeof(gdt_entry_t));
    hpet_init();
    //Enable preemptive scheduler timer
    apic_start_lapic_timer();
    //Enable internal system timer (HPET)
    arm_hpet_interrupt_timer(77000);
    apic_ioapic_mask(HPET_TIMER_IRQ, 1); //Unmask HPET IRQ
}

void * cpu_get_current_thread(void) {
    core_context_t * ctx = (core_context_t *)get_cpu_gs_base();
    if (ctx && ctx->cinfo) {
        return ctx->cinfo->thread;
    }
    return NULL;
}

void cpu_init() {
    //Iterate cpus other than the bsp
    uint32_t bsp_lapic_id = get_smp_bsp_lapic_id();
    uint64_t get_smp_cpu_count();
    boot_smp_info_t ** cpus = get_smp_cpus();

    for (uint64_t i = 0; i < get_smp_cpu_count(); i++) {
        boot_smp_info_t * cpu = cpus[i];
        if (cpu->lapic_id != bsp_lapic_id) {
            cpu->goto_address = (void*)(uint64_t)callback;
        } else {
            cpu_init_id(cpu->processor_id, cpu->lapic_id);
        }
    }
}