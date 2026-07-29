#ifndef _PCI_H
#define _PCI_H

#include <krnl/libraries/std/stddef.h>
#include <krnl/libraries/std/stdint.h>

typedef struct pci_device {
    uint8_t bus;
    uint8_t device;
    uint8_t function;
    uint16_t vendor_id;
    uint16_t device_id;
    uint8_t class_code;
    uint8_t subclass;
    uint8_t prog_if;
    uint8_t header_type;
} pci_device_t;

uint8_t  pci_config_read8 (uint8_t bus, uint8_t device, uint8_t function, uint8_t offset);
uint16_t pci_config_read16(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset);
uint32_t pci_config_read32(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset);
void pci_config_write8 (uint8_t bus, uint8_t device, uint8_t function, uint8_t offset, uint8_t value);
void pci_config_write16(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset, uint16_t value);
void pci_config_write32(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset, uint32_t value);

/* Brute-force scans every bus/device/function looking for a device matching
   class_code/subclass/prog_if. Returns SUCCESS + fills *out, or NOT_FOUND. */
status_t pci_find_device(uint8_t class_code, uint8_t subclass, uint8_t prog_if, pci_device_t *out);

/* Reads/decodes BAR `bar` (0-5). Returns the base address with the low
   flag bits masked off. Does not handle 64-bit BAR pairs. */
uint32_t pci_bar_address(pci_device_t *dev, uint8_t bar);

/* Command register (offset 0x04) read-modify-write helper. */
#define PCI_COMMAND_MEMORY_SPACE 0x0002
#define PCI_COMMAND_BUS_MASTER   0x0004
void pci_enable_command_bits(pci_device_t *dev, uint16_t bits);

/* Walks the capabilities linked list for a capability with the given id.
   Returns the config-space offset of the capability, or 0 if not present. */
#define PCI_CAP_ID_MSI 0x05
uint8_t pci_find_capability(pci_device_t *dev, uint8_t cap_id);

#endif
