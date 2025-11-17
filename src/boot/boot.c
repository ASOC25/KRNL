
#include <krnl/boot/bootloaders/bootloader.h>
#include <krnl/drivers/serial/serial.h>
#include <krnl/devices/devices.h>
#include <krnl/libraries/std/string.h>
#include <krnl/arch/x86/cpu.h>
#include <krnl/debug/debug.h>

void boot_startup() {
    //Init the bootloader
    init_bootloader();
    //Init SIMD support
    arch_init_simd();
    //Optionally init the framebuffer
    
    //Init device subsystem
    devices_init();
    //Init the early debugger over dcon or serial
    serial_init_pnp();
    debug_init(3, 0); //Major 3 is debug console [¡¡¡¡¡¡¡¡¡THIS MAY CHANGE!!!!!!!!]

    //Init physical memory manager

    //Init virtual memory manager

    //Create a reasonable GDT

    //Init the programmable interrupt timer (PIT)

    //Init the interrupt subsystem (IDT + PIC or APIC)

    //Init the CPU subsystem (detect features, multicore, etc)

    //Init the VDSO subsystem

    //Initialize ACPI subsystem

    //Initialize the disk drivers

    //Register other devices (fifo, serial, tty, ps2, pci)

    //Spawn a tty over your preferred device (usually serial for debugging)

    //Init the advanced debugger over the tty

    //Register filesystem drivers (fifo, ext2, tty) via VFS

    //Probe filesystems on disks

    //Start the core subsystem (manages processes, scheduling, uspace, etc)


    kprintf("ASOC KERNEL BOOTED SUCCESSFULLY!\n");
    kprintf("Using bootloader: %s version: %s\n", get_bootloader_name(), get_bootloader_version());
    while (1);
}
