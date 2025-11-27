
#include <krnl/boot/bootloaders/bootloader.h>
#include <krnl/drivers/serial/serial.h>
#include <krnl/drivers/ramdisk/ramdisk.h>
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
#include <krnl/vfs/vfs.h>

void boot_startup() {
    __asm__("cli");

    //Init the bootloader
    init_bootloader();
    //Optionally init the framebuffer
    
    //Init device subsystem
    devices_init();
    //Init the early debugger over dcon or serial
    serial_init_pnp();
    ramdisk_init_pnp();
    debug_init(3, 0); //Major 3 is debug console [¡¡¡¡¡¡¡¡¡THIS MAY CHANGE!!!!!!!!]
    //Init memory management subsystem
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

    kprintf("ASOC KERNEL BOOTED SUCCESSFULLY!\n");
    kprintf("Using bootloader: %s version: %s\n", get_bootloader_name(), get_bootloader_version());
    run_all_tests();

    uint8_t buffer[512];
    int64_t read_bytes = devices_read(4, 0, 0, 512, buffer); //Read first 512 bytes from ramdisk major 4
    if (read_bytes != 512) {
        panic("Failed to read from ramdisk");
    }

    //Print all hex bytes read
    kprintf("First 512 bytes of ramdisk:\n");
    for (int i = 0; i < 512; i++) {
        kprintf("%02x ", buffer[i]);
        if ((i + 1) % 16 == 0) {
            kprintf("\n");
        }
    }

    x1fs_init();

    vfs_new_mount(4, 0, "/");
    vfs_file_descriptor_t *fd = vfs_open("/a.txt", 0);
    if (fd == NULL) {
        panic("Failed to open /a.txt");
    }

    uint8_t file_buffer[6];
    ssize_t bytes_read_file = vfs_read(fd, file_buffer, 6);
    if (bytes_read_file < 0) {
        panic("Failed to read from /a.txt");
    }
    kprintf("Read %d bytes from /a.txt: ", (int)bytes_read_file);
    for (ssize_t i = 0; i < bytes_read_file; i++)
        kprintf("%c", file_buffer[i]);
    kprintf("\n");
    vfs_close(fd);
    process_init();
    __asm__("sti");
    while (1);
}
