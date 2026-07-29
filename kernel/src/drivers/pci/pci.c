#include <krnl/drivers/pci/pci.h>
#include <krnl/arch/x86/io.h>

#define PCI_CONFIG_ADDRESS 0xCF8
#define PCI_CONFIG_DATA    0xCFC

static uint32_t pci_config_address(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset) {
    return (uint32_t)(1u << 31)
         | ((uint32_t)bus << 16)
         | ((uint32_t)(device & 0x1F) << 11)
         | ((uint32_t)(function & 0x07) << 8)
         | ((uint32_t)offset & 0xFC);
}

uint32_t pci_config_read32(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset) {
    outl(PCI_CONFIG_ADDRESS, pci_config_address(bus, device, function, offset));
    return inl(PCI_CONFIG_DATA);
}

void pci_config_write32(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset, uint32_t value) {
    outl(PCI_CONFIG_ADDRESS, pci_config_address(bus, device, function, offset));
    outl(PCI_CONFIG_DATA, value);
}

uint16_t pci_config_read16(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset) {
    uint32_t dword = pci_config_read32(bus, device, function, offset & 0xFC);
    return (uint16_t)(dword >> ((offset & 2) * 8));
}

void pci_config_write16(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset, uint16_t value) {
    uint8_t aligned = offset & 0xFC;
    uint32_t shift = (offset & 2) * 8;
    uint32_t dword = pci_config_read32(bus, device, function, aligned);
    dword = (dword & ~(0xFFFFu << shift)) | ((uint32_t)value << shift);
    pci_config_write32(bus, device, function, aligned, dword);
}

uint8_t pci_config_read8(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset) {
    uint32_t dword = pci_config_read32(bus, device, function, offset & 0xFC);
    return (uint8_t)(dword >> ((offset & 3) * 8));
}

void pci_config_write8(uint8_t bus, uint8_t device, uint8_t function, uint8_t offset, uint8_t value) {
    uint8_t aligned = offset & 0xFC;
    uint32_t shift = (offset & 3) * 8;
    uint32_t dword = pci_config_read32(bus, device, function, aligned);
    dword = (dword & ~(0xFFu << shift)) | ((uint32_t)value << shift);
    pci_config_write32(bus, device, function, aligned, dword);
}

status_t pci_find_device(uint8_t class_code, uint8_t subclass, uint8_t prog_if, pci_device_t *out) {
    for (uint16_t bus = 0; bus < 256; bus++) {
        for (uint8_t device = 0; device < 32; device++) {
            uint16_t vendor = pci_config_read16((uint8_t)bus, device, 0, 0x00);
            if (vendor == 0xFFFF) continue;

            uint8_t header_type = pci_config_read8((uint8_t)bus, device, 0, 0x0E);
            uint8_t max_func = (header_type & 0x80) ? 8 : 1;

            for (uint8_t function = 0; function < max_func; function++) {
                uint16_t fn_vendor = pci_config_read16((uint8_t)bus, device, function, 0x00);
                if (fn_vendor == 0xFFFF) continue;

                uint8_t fn_class    = pci_config_read8((uint8_t)bus, device, function, 0x0B);
                uint8_t fn_subclass = pci_config_read8((uint8_t)bus, device, function, 0x0A);
                uint8_t fn_prog_if  = pci_config_read8((uint8_t)bus, device, function, 0x09);

                if (fn_class == class_code && fn_subclass == subclass && fn_prog_if == prog_if) {
                    out->bus         = (uint8_t)bus;
                    out->device      = device;
                    out->function    = function;
                    out->vendor_id   = fn_vendor;
                    out->device_id   = pci_config_read16((uint8_t)bus, device, function, 0x02);
                    out->class_code  = fn_class;
                    out->subclass    = fn_subclass;
                    out->prog_if     = fn_prog_if;
                    out->header_type = pci_config_read8((uint8_t)bus, device, function, 0x0E);
                    return SUCCESS;
                }
            }
        }
    }
    return NOT_FOUND;
}

uint32_t pci_bar_address(pci_device_t *dev, uint8_t bar) {
    uint32_t raw = pci_config_read32(dev->bus, dev->device, dev->function, (uint8_t)(0x10 + bar * 4));
    if (raw & 0x1) {
        /* I/O space BAR */
        return raw & 0xFFFFFFFC;
    }
    /* Memory space BAR */
    return raw & 0xFFFFFFF0;
}

void pci_enable_command_bits(pci_device_t *dev, uint16_t bits) {
    uint16_t cmd = pci_config_read16(dev->bus, dev->device, dev->function, 0x04);
    pci_config_write16(dev->bus, dev->device, dev->function, 0x04, (uint16_t)(cmd | bits));
}

uint8_t pci_find_capability(pci_device_t *dev, uint8_t cap_id) {
    uint16_t status = pci_config_read16(dev->bus, dev->device, dev->function, 0x06);
    if (!(status & 0x10)) return 0; /* no capabilities list */

    uint8_t ptr = pci_config_read8(dev->bus, dev->device, dev->function, 0x34) & 0xFC;
    for (int guard = 0; ptr != 0 && guard < 48; guard++) {
        uint8_t id   = pci_config_read8(dev->bus, dev->device, dev->function, ptr);
        uint8_t next = pci_config_read8(dev->bus, dev->device, dev->function, (uint8_t)(ptr + 1));
        if (id == cap_id) return ptr;
        ptr = next & 0xFC;
    }
    return 0;
}
