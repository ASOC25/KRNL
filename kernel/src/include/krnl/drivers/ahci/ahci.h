#ifndef _AHCI_H
#define _AHCI_H

#include <krnl/devices/devices.h>
#include <krnl/libraries/std/stddef.h>

/* 3=serial, 4=ramdisk, 5=framebuffer, 6=ps2 mouse, 7=ps2 keyboard are taken. */
#define AHCI_DRIVER_MAJOR 8

/* Enumerates the PCI bus for an AHCI controller (class 0x01, subclass 0x06,
   prog-if 0x01), arms an MSI interrupt for it, and registers one AHCI
   device_driver device per SATA drive found. Returns NOT_FOUND (without
   registering anything) if no AHCI controller is present, so a plain
   x1fs/ramdisk boot with no AHCI-attached image is unaffected. */
status_t ahci_init_pnp(void);

/* True (and *minor filled) if ahci_init_pnp() found at least one drive.
   major is always AHCI_DRIVER_MAJOR. Used by boot.c to pick which device
   to mount root from. */
uint8_t ahci_get_boot_drive(device_minor_t *minor);

/* Call once the idle thread exists (right after scheduler_create_idle_thread()
   in boot.c). Before this, AHCI command waits poll hardware registers instead
   of sti;hlt, since enabling interrupts before any thread exists would let the
   already-armed scheduler timer fire with nothing runnable to switch to. */
void ahci_mark_scheduler_ready(void);

#endif
