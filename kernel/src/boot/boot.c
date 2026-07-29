
#include <krnl/boot/bootloaders/bootloader.h>
#include <krnl/drivers/serial/serial.h>
#include <krnl/drivers/ramdisk/ramdisk.h>
#include <krnl/drivers/ps2/ps2.h>
#include <krnl/drivers/framebuffer/framebuffer.h>
#include <krnl/drivers/pci/pci.h>
#include <krnl/drivers/ahci/ahci.h>
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
#include <krnl/fs/ext2/ext2.h>
#include <krnl/fs/tty/tty.h>
#include <krnl/fs/memdev/memdev.h>
#include <krnl/vfs/vfs.h>
#include <krnl/arch/x86/apic.h>
#include <krnl/process/scheduler.h>
#include <krnl/arch/x86/hpet.h>
#include <krnl/arch/x86/apic.h>

extern uint8_t getApicId(void);

void boot_startup() {
    //Init the bootloader
    init_bootloader();
    //Optionally init the framebuffer
    
    //Init device subsystem
    devices_init();
    //Init the early debugger over dcon or serial
    mem_init();
    //Init the CPU subsystem
    cpu_init();
    //Initialize the disk drivers
    serial_init_pnp();
    ramdisk_init_pnp();
    framebuffer_init_pnp();
    ps2_init_pnp();
    ahci_init_pnp();
    ktrace("This is a fucking test...\n");

    //Enable serial interrupts
    //Register other devices (fifo, serial, tty, ps2, pci)

    //Spawn a tty over your preferred device (usually serial for debugging)

    //Init the advanced debugger over the tty

    //Register filesystem drivers (fifo, ext2, tty) via VFS

    //Probe filesystems on disks

    //Start the core subsystem (manages processes, scheduling, uspace, etc)
    ext2_init();
    x1fs_init();
    tty_init();
    memdev_init();

    device_major_t root_major = RAMDISK_DRIVER_MAJOR;
    device_minor_t root_minor = 0;
    device_minor_t ahci_minor;
    if (ahci_get_boot_drive(&ahci_minor)) {
        root_major = AHCI_DRIVER_MAJOR;
        root_minor = ahci_minor;
    }
    vfs_mount_t * root_mount = vfs_new_mount(root_major, root_minor, "/");
    vfs_new_mount(3, 0, "/dev/tty0"); //Placeholders!!!!
    vfs_new_mount(MEMDEV_DRIVER_MAJOR, MEMDEV_MINOR_NULL, "/dev/null");
    vfs_new_mount(MEMDEV_DRIVER_MAJOR, MEMDEV_MINOR_ZERO, "/dev/zero");
    vfs_new_mount(MEMDEV_DRIVER_MAJOR, MEMDEV_MINOR_RANDOM, "/dev/random");
    vfs_new_mount(MEMDEV_DRIVER_MAJOR, MEMDEV_MINOR_URANDOM, "/dev/urandom");
    vfs_path_t root_path;
    root_path.mount = root_mount;
    strcpy(root_path.internal_path, "/");
    vfs_path_t cwd_path;
    cwd_path.mount = root_mount;
    strcpy(cwd_path.internal_path, "/");
    kprintf("ASOC KERNEL BOOTED SUCCESSFULLY!\n");
    kprintf("Using bootloader: %s version: %s\n", get_bootloader_name(), get_bootloader_version());
    //run_all_tests();

    process_init("/init.elf", "/dev/tty0", cwd_path, root_path);
    scheduler_create_idle_thread();
    ahci_mark_scheduler_ready();
    __asm__("sti");
    while (1);
}
