
#include <krnl/boot/bootloaders/bootloader.h>
#include <krnl/drivers/serial/serial.h>
#include <krnl/drivers/ramdisk/ramdisk.h>
#include <krnl/drivers/framebuffer/framebuffer.h>
#include <krnl/devices/devices.h>
#include <krnl/libraries/std/string.h>
#include <krnl/libraries/std/stddef.h>
#include <krnl/libraries/std/stdint.h>
#include <krnl/arch/x86/cpu.h>
#include <krnl/mem/allocator.h>
#include <krnl/debug/debug.h>
#include <krnl/tests/tests.h>
#include <krnl/mem/mem.h>
#include <krnl/process/process.h>
#include <krnl/fs/x1fs/x1fs.h>
#include <krnl/fs/tty/tty.h>
#include <krnl/vfs/vfs.h>
#include <krnl/arch/x86/apic.h>
#include <krnl/process/scheduler.h>
#include <krnl/arch/x86/hpet.h>

extern uint8_t getApicId(void);

void boot_startup() {
    //Init the bootloader
    init_bootloader();
    //Optionally init the framebuffer
    
    //Init device subsystem
    devices_init();
    //Init the early debugger over dcon or serial
    serial_init_pnp();
    ramdisk_init_pnp();
    framebuffer_init_pnp();
    //Init memory management subsystem
    ktrace("This is a fucking test...\n");
    mem_init();
    //Init the CPU subsystem
    cpu_init();
    //Initialize the disk drivers

    //Register other devices (fifo, serial, tty, ps2, pci)

    //Spawn a tty over your preferred device (usually serial for debugging)

    //Init the advanced debugger over the tty

    //Register filesystem drivers (fifo, ext2, tty) via VFS

    //Probe filesystems on disks

    //Start the core subsystem (manages processes, scheduling, uspace, etc)

    x1fs_init();
    tty_init();
    vfs_new_mount(4, 0, "/");
    vfs_new_mount(3, 0, "/dev/tty0"); //Placeholders!!!!

    kprintf("ASOC KERNEL BOOTED SUCCESSFULLY!\n");
    kprintf("Using bootloader: %s version: %s\n", get_bootloader_name(), get_bootloader_version());
    //run_all_tests();

    process_init("/init.elf", "/dev/tty0");
    __asm__("sti");
    while (1);
}
