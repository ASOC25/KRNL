#include <krnl/drivers/ahci/ahci.h>
#include <krnl/drivers/pci/pci.h>
#include <krnl/devices/devices.h>
#include <krnl/mem/pmm.h>
#include <krnl/mem/vmm.h>
#include <krnl/arch/x86/idt.h>
#include <krnl/arch/x86/cpu.h>
#include <krnl/debug/debug.h>
#include <krnl/libraries/std/string.h>
#include <krnl/libraries/std/stdint.h>
#include <krnl/libraries/std/stddef.h>

extern uint8_t getApicId(void);

/* ------------------------------------------------------------------ */
/* AHCI 1.3.1 register / structure layout                              */
/* ------------------------------------------------------------------ */

#define AHCI_MAX_PORTS 32

#define HBA_GHC_AE   (1u << 31)
#define HBA_GHC_IE   (1u << 1)

#define HBA_PxCMD_ST   (1u << 0)
#define HBA_PxCMD_FRE  (1u << 4)
#define HBA_PxCMD_FR   (1u << 14)
#define HBA_PxCMD_CR   (1u << 15)

#define HBA_PxIS_DHRS  (1u << 0)
#define HBA_PxIS_TFES  (1u << 30)

#define HBA_PxTFD_ERR  (1u << 0)
#define HBA_PxTFD_DRQ  (1u << 3)
#define HBA_PxTFD_BSY  (1u << 7)

#define HBA_PORT_DET_PRESENT 3
#define HBA_PORT_IPM_ACTIVE  1

#define SATA_SIG_ATA   0x00000101u
#define SATA_SIG_ATAPI 0xEB140101u
#define SATA_SIG_SEMB  0xC33C0101u
#define SATA_SIG_PM    0x96690101u

#define ATA_CMD_READ_DMA_EXT  0x25
#define ATA_CMD_WRITE_DMA_EXT 0x35
#define ATA_CMD_IDENTIFY      0xEC

#define FIS_TYPE_REG_H2D 0x27

#define AHCI_MSI_VECTOR 0x50
#define AHCI_SECTOR_SIZE 512u
#define AHCI_BOUNCE_SECTORS (PMM_PAGE_SIZE / AHCI_SECTOR_SIZE)

typedef struct hba_port {
    uint32_t clb;
    uint32_t clbu;
    uint32_t fb;
    uint32_t fbu;
    uint32_t is;
    uint32_t ie;
    uint32_t cmd;
    uint32_t rsv0;
    uint32_t tfd;
    uint32_t sig;
    uint32_t ssts;
    uint32_t sctl;
    uint32_t serr;
    uint32_t sact;
    uint32_t ci;
    uint32_t sntf;
    uint32_t fbs;
    uint32_t rsv1[11];
    uint32_t vendor[4];
} __attribute__((packed)) hba_port_t;

typedef struct hba_mem {
    uint32_t cap;
    uint32_t ghc;
    uint32_t is;
    uint32_t pi;
    uint32_t vs;
    uint32_t ccc_ctl;
    uint32_t ccc_pts;
    uint32_t em_loc;
    uint32_t em_ctl;
    uint32_t cap2;
    uint32_t bohc;
    uint8_t  reserved[0xA0 - 0x2C];
    uint8_t  vendor[0x100 - 0xA0];
    hba_port_t ports[AHCI_MAX_PORTS];
} __attribute__((packed)) hba_mem_t;

typedef struct hba_cmd_header {
    uint8_t  cfl:5;
    uint8_t  a:1;
    uint8_t  w:1;
    uint8_t  p:1;

    uint8_t  r:1;
    uint8_t  b:1;
    uint8_t  c:1;
    uint8_t  rsv0:1;
    uint8_t  pmp:4;

    uint16_t prdtl;

    volatile uint32_t prdbc;

    uint32_t ctba;
    uint32_t ctbau;

    uint32_t rsv1[4];
} __attribute__((packed)) hba_cmd_header_t;

typedef struct hba_prdt_entry {
    uint32_t dba;
    uint32_t dbau;
    uint32_t rsv0;

    uint32_t dbc:22;
    uint32_t rsv1:9;
    uint32_t i:1;
} __attribute__((packed)) hba_prdt_entry_t;

#define AHCI_PRDT_ENTRIES 1

typedef struct hba_cmd_tbl {
    uint8_t cfis[64];
    uint8_t acmd[16];
    uint8_t rsv[48];
    hba_prdt_entry_t prdt_entry[AHCI_PRDT_ENTRIES];
} __attribute__((packed)) hba_cmd_tbl_t;

typedef struct fis_reg_h2d {
    uint8_t fis_type;

    uint8_t pmport:4;
    uint8_t rsv0:3;
    uint8_t c:1;

    uint8_t command;
    uint8_t featurel;

    uint8_t lba0;
    uint8_t lba1;
    uint8_t lba2;
    uint8_t device;

    uint8_t lba3;
    uint8_t lba4;
    uint8_t lba5;
    uint8_t featureh;

    uint8_t countl;
    uint8_t counth;
    uint8_t icc;
    uint8_t control;

    uint8_t rsv1[4];
} __attribute__((packed)) fis_reg_h2d_t;

/* ------------------------------------------------------------------ */
/* Driver state                                                        */
/* ------------------------------------------------------------------ */

struct ahci_port_ctx {
    volatile hba_port_t *regs;
    uint64_t clb_phys;
    void    *clb_virt;
    uint64_t fb_phys;
    void    *fb_virt;
    uint64_t ctba_phys;
    void    *ctba_virt;
    uint64_t scratch_phys;
    uint64_t sector_count;
    volatile uint8_t command_done;
    volatile uint8_t command_error;
    uint8_t present;
};

static volatile hba_mem_t *hba = NULL;
static struct ahci_port_ctx ahci_ports[AHCI_MAX_PORTS] = {0};
/* Forced on permanently: the MSI-driven wait in ahci_wait_command() below
   parks the calling thread in a `sti; hlt` loop, which lets the scheduler
   preempt it (kernel-mode context save/restore) once per iteration. A large
   file read issues one such wait per filesystem block (1 KB each), so
   reading something the size of libc.so re-enters that save/restore path
   over a thousand times in a row. The kernel-mode context resume mechanism
   has a known, never-fully-root-caused small state drift per cycle (see the
   idle-thread RSP drift fix in process/scheduler.c) that a handful of
   cycles never surfaces, but ~1000+ back-to-back cycles reliably corrupts
   the resumed thread's register/stack state -- observed as a wild jump into
   libc.so right after its first large mmap+read from a real AHCI disk.
   Polling never sleeps via hlt, so the waiting thread is never suspended
   and resumed through that path. Revisit once the drift's root cause is
   found; until then, do not flip this back to interrupt-driven waits. */
static uint8_t ahci_use_polling = 1;
static uint8_t ahci_has_boot_drive = 0;
static device_minor_t ahci_boot_minor = 0;

/* The LAPIC scheduler timer is armed (unmasked, one-shot) as early as
   cpu_init(), long before any thread -- let alone the idle thread -- exists.
   Nothing enables interrupts (sti) before that point during a normal boot,
   so the pending timer interrupt just sits latched. Command waits issued
   before ahci_mark_scheduler_ready() (i.e. everything up to and including
   process_init()'s ext2/AHCI reads while loading /init.elf) must therefore
   poll instead of sti;hlt -- enabling interrupts that early lets the latched
   scheduler-timer interrupt fire with no runnable thread yet, which panics
   in scheduler_get_next_thread(). */
static uint8_t ahci_scheduler_ready = 0;

void ahci_mark_scheduler_ready(void) {
    ahci_scheduler_ready = 1;
}

/* ------------------------------------------------------------------ */
/* Port control                                                        */
/* ------------------------------------------------------------------ */

static void ahci_port_stop(volatile hba_port_t *port) {
    port->cmd &= ~(HBA_PxCMD_ST | HBA_PxCMD_FRE);
    for (uint32_t spin = 0; spin < 1000000; spin++) {
        if (!(port->cmd & (HBA_PxCMD_FR | HBA_PxCMD_CR))) break;
    }
}

static void ahci_port_start(volatile hba_port_t *port) {
    for (uint32_t spin = 0; spin < 1000000; spin++) {
        if (!(port->cmd & HBA_PxCMD_CR)) break;
    }
    port->cmd |= HBA_PxCMD_FRE;
    port->cmd |= HBA_PxCMD_ST;
}

static int ahci_port_is_sata_drive(volatile hba_port_t *port) {
    uint32_t ssts = port->ssts;
    uint8_t det = (uint8_t)(ssts & 0x0F);
    uint8_t ipm = (uint8_t)((ssts >> 8) & 0x0F);
    if (det != HBA_PORT_DET_PRESENT || ipm != HBA_PORT_IPM_ACTIVE) return 0;
    return port->sig == SATA_SIG_ATA;
}

static void ahci_port_init(int idx) {
    volatile hba_port_t *port = &hba->ports[idx];
    struct ahci_port_ctx *pc = &ahci_ports[idx];

    ahci_port_stop(port);

    /* Command list (1KB) + received-FIS area (256B) + slot 0's command
       table (256B) comfortably share a single 4KB page. */
    void *ctrl_phys = pmm_alloc_pages(1);
    void *ctrl_virt = (void *)vmm_to_identity_map((uint64_t)ctrl_phys);
    memset(ctrl_virt, 0, PMM_PAGE_SIZE);

    void *scratch_phys = pmm_alloc_pages(1);

    pc->regs          = port;
    pc->clb_phys       = (uint64_t)ctrl_phys;
    pc->clb_virt       = ctrl_virt;
    pc->fb_phys        = (uint64_t)ctrl_phys + 1024;
    pc->fb_virt        = (uint8_t *)ctrl_virt + 1024;
    pc->ctba_phys      = (uint64_t)ctrl_phys + 1280;
    pc->ctba_virt      = (uint8_t *)ctrl_virt + 1280;
    pc->scratch_phys   = (uint64_t)scratch_phys;
    pc->command_done   = 0;
    pc->command_error  = 0;
    pc->present        = 1;

    port->clb  = (uint32_t)(pc->clb_phys & 0xFFFFFFFFu);
    port->clbu = (uint32_t)(pc->clb_phys >> 32);
    port->fb   = (uint32_t)(pc->fb_phys & 0xFFFFFFFFu);
    port->fbu  = (uint32_t)(pc->fb_phys >> 32);
    port->serr = 0xFFFFFFFFu;
    port->is   = 0xFFFFFFFFu;
    port->ie   = HBA_PxIS_DHRS | HBA_PxIS_TFES;

    ahci_port_start(port);
}

/* ------------------------------------------------------------------ */
/* Command issue                                                       */
/* ------------------------------------------------------------------ */

static status_t ahci_wait_command(struct ahci_port_ctx *pc) {
    if (ahci_use_polling || !ahci_scheduler_ready) {
        uint32_t spin = 0;
        while (pc->regs->ci & 1u) {
            if (++spin > 50000000u)
                panic("ahci: polling wait for command completion timed out");
        }
        uint32_t is = pc->regs->is;
        status_t result = (is & HBA_PxIS_TFES) ? FAILURE : SUCCESS;
        pc->regs->is = 0xFFFFFFFFu;
        return result;
    }

    /* Preserve the caller's interrupt-flag state across the wait: during
       early boot (before boot.c's final `sti`) interrupts are off and should
       stay off afterwards; once the kernel is fully up, syscalls run with
       interrupts already enabled and must not come back from a disk I/O
       call with interrupts permanently disabled. */
    uint64_t saved_flags;
    __asm__ volatile("pushfq; pop %0" : "=r"(saved_flags));

    uint32_t spins = 0;
    while (!pc->command_done) {
        __asm__ volatile("sti; hlt");
        if (++spins > 1000000u)
            panic("ahci: timed out waiting for MSI command-completion interrupt");
    }
    if (!(saved_flags & (1ull << 9))) {
        __asm__ volatile("cli");
    }
    status_t result = pc->command_error ? FAILURE : SUCCESS;
    pc->command_done  = 0;
    pc->command_error = 0;
    return result;
}

static status_t ahci_issue_command(struct ahci_port_ctx *pc, uint8_t ata_cmd,
                                    uint64_t lba, uint16_t sector_count,
                                    uint64_t buffer_phys, uint32_t byte_count,
                                    int is_write) {
    volatile hba_port_t *port = pc->regs;

    uint32_t spin = 0;
    while (port->tfd & (HBA_PxTFD_BSY | HBA_PxTFD_DRQ)) {
        if (++spin > 1000000u) panic("ahci: port stuck busy, cannot issue command");
    }

    hba_cmd_header_t *hdr = (hba_cmd_header_t *)pc->clb_virt;
    memset(hdr, 0, sizeof(hba_cmd_header_t));
    hdr->cfl   = sizeof(fis_reg_h2d_t) / 4;
    hdr->w     = is_write ? 1 : 0;
    hdr->prdtl = AHCI_PRDT_ENTRIES;
    hdr->ctba  = (uint32_t)(pc->ctba_phys & 0xFFFFFFFFu);
    hdr->ctbau = (uint32_t)(pc->ctba_phys >> 32);

    hba_cmd_tbl_t *tbl = (hba_cmd_tbl_t *)pc->ctba_virt;
    memset(tbl, 0, sizeof(hba_cmd_tbl_t));
    tbl->prdt_entry[0].dba  = (uint32_t)(buffer_phys & 0xFFFFFFFFu);
    tbl->prdt_entry[0].dbau = (uint32_t)(buffer_phys >> 32);
    tbl->prdt_entry[0].dbc  = byte_count - 1;
    tbl->prdt_entry[0].i    = 1;

    fis_reg_h2d_t *fis = (fis_reg_h2d_t *)tbl->cfis;
    fis->fis_type = FIS_TYPE_REG_H2D;
    fis->c        = 1;
    fis->command  = ata_cmd;
    fis->device   = 0x40; /* LBA mode */
    fis->lba0 = (uint8_t)(lba);
    fis->lba1 = (uint8_t)(lba >> 8);
    fis->lba2 = (uint8_t)(lba >> 16);
    fis->lba3 = (uint8_t)(lba >> 24);
    fis->lba4 = (uint8_t)(lba >> 32);
    fis->lba5 = (uint8_t)(lba >> 40);
    fis->countl = (uint8_t)(sector_count);
    fis->counth = (uint8_t)(sector_count >> 8);

    pc->command_done  = 0;
    pc->command_error = 0;
    port->ci = 1u; /* issue slot 0 */

    return ahci_wait_command(pc);
}

static status_t ahci_identify(struct ahci_port_ctx *pc) {
    status_t st = ahci_issue_command(pc, ATA_CMD_IDENTIFY, 0, 1,
                                      pc->scratch_phys, AHCI_SECTOR_SIZE, 0);
    if (st != SUCCESS) return st;

    uint16_t *words = (uint16_t *)vmm_to_identity_map(pc->scratch_phys);
    pc->sector_count = ((uint64_t)words[100])
                     | ((uint64_t)words[101] << 16)
                     | ((uint64_t)words[102] << 32)
                     | ((uint64_t)words[103] << 48);
    return SUCCESS;
}

/* ------------------------------------------------------------------ */
/* Interrupt handler                                                    */
/* ------------------------------------------------------------------ */

static void ahci_interrupt_handler(cpu_context_t *ctx, uint8_t cpu_id) {
    (void)ctx; (void)cpu_id;

    uint32_t pending = hba->is;
    for (int i = 0; i < AHCI_MAX_PORTS; i++) {
        if (!(pending & (1u << i))) continue;
        if (!ahci_ports[i].present) continue;

        volatile hba_port_t *port = ahci_ports[i].regs;
        uint32_t port_is = port->is;
        if (port_is & HBA_PxIS_TFES) ahci_ports[i].command_error = 1;
        port->is = port_is;
        ahci_ports[i].command_done = 1;
    }
    hba->is = pending;
}

/* ------------------------------------------------------------------ */
/* device_driver read/write: byte-addressable, sector-granular RMW      */
/* ------------------------------------------------------------------ */

static int64_t ahci_transfer(struct ahci_port_ctx *pc, uint64_t offset,
                              uint64_t size, uint8_t *buffer, int is_write) {
    uint8_t *scratch_virt = (uint8_t *)vmm_to_identity_map(pc->scratch_phys);
    uint64_t done = 0;

    while (done < size) {
        uint64_t abs_off       = offset + done;
        uint64_t start_lba     = abs_off / AHCI_SECTOR_SIZE;
        uint64_t sector_off    = abs_off % AHCI_SECTOR_SIZE;
        uint64_t remaining     = size - done;

        uint64_t span          = sector_off + remaining;
        uint32_t sectors       = (uint32_t)((span + AHCI_SECTOR_SIZE - 1) / AHCI_SECTOR_SIZE);
        if (sectors > AHCI_BOUNCE_SECTORS) sectors = AHCI_BOUNCE_SECTORS;

        uint64_t chunk_bytes   = (uint64_t)sectors * AHCI_SECTOR_SIZE - sector_off;
        uint64_t xfer          = remaining < chunk_bytes ? remaining : chunk_bytes;
        uint32_t chunk_size    = sectors * AHCI_SECTOR_SIZE;

        if (is_write) {
            int partial = (sector_off != 0) || (xfer != chunk_size);
            if (partial) {
                if (ahci_issue_command(pc, ATA_CMD_READ_DMA_EXT, start_lba,
                                        (uint16_t)sectors, pc->scratch_phys,
                                        chunk_size, 0) != SUCCESS)
                    return done ? (int64_t)done : -1;
            }
            memcpy(scratch_virt + sector_off, buffer + done, xfer);
            if (ahci_issue_command(pc, ATA_CMD_WRITE_DMA_EXT, start_lba,
                                    (uint16_t)sectors, pc->scratch_phys,
                                    chunk_size, 1) != SUCCESS)
                return done ? (int64_t)done : -1;
        } else {
            if (ahci_issue_command(pc, ATA_CMD_READ_DMA_EXT, start_lba,
                                    (uint16_t)sectors, pc->scratch_phys,
                                    chunk_size, 0) != SUCCESS)
                return done ? (int64_t)done : -1;
            memcpy(buffer + done, scratch_virt + sector_off, xfer);
        }

        done += xfer;
    }
    return (int64_t)done;
}

static int64_t ahci_read(device_addr_t id, uint64_t offset, uint64_t size, uint8_t *buffer) {
    return ahci_transfer(&ahci_ports[id], offset, size, buffer, 0);
}

static int64_t ahci_write(device_addr_t id, uint64_t offset, uint64_t size, const uint8_t *buffer) {
    return ahci_transfer(&ahci_ports[id], offset, size, (uint8_t *)buffer, 1);
}

static int64_t ahci_dev_initialize(device_addr_t id) { (void)id; return SUCCESS; }
static int64_t ahci_dev_shutdown(device_addr_t id)   { (void)id; return SUCCESS; }
static int64_t ahci_dev_ioctl(device_addr_t id, uint64_t command,
                               uint64_t input_buffer, uint64_t output_buffer) {
    (void)id; (void)command; (void)input_buffer; (void)output_buffer;
    return SUCCESS;
}

/* ------------------------------------------------------------------ */
/* PCI discovery + MSI setup                                           */
/* ------------------------------------------------------------------ */

static void ahci_setup_msi(pci_device_t *dev) {
    uint8_t msi_off = pci_find_capability(dev, PCI_CAP_ID_MSI);
    if (!msi_off) {
        kprintf("ahci: controller has no MSI capability, falling back to polling\n");
        ahci_use_polling = 1;
        return;
    }

    uint16_t msg_ctrl = pci_config_read16(dev->bus, dev->device, dev->function,
                                           (uint8_t)(msi_off + 2));
    uint8_t is_64bit = (uint8_t)((msg_ctrl >> 7) & 1);
    uint8_t apic_id  = getApicId();
    uint32_t msi_addr = 0xFEE00000u | ((uint32_t)apic_id << 12);

    pci_config_write32(dev->bus, dev->device, dev->function,
                        (uint8_t)(msi_off + 4), msi_addr);

    uint8_t data_off;
    if (is_64bit) {
        pci_config_write32(dev->bus, dev->device, dev->function,
                            (uint8_t)(msi_off + 8), 0);
        data_off = (uint8_t)(msi_off + 12);
    } else {
        data_off = (uint8_t)(msi_off + 8);
    }
    pci_config_write16(dev->bus, dev->device, dev->function, data_off, AHCI_MSI_VECTOR);

    /* Force single-message mode (MME = 0) and set the enable bit. */
    msg_ctrl = (uint16_t)((msg_ctrl & ~0x0070u) | 0x0001u);
    pci_config_write16(dev->bus, dev->device, dev->function,
                        (uint8_t)(msi_off + 2), msg_ctrl);

    register_dynamic_interrupt(AHCI_MSI_VECTOR, ahci_interrupt_handler);
}

status_t ahci_init_pnp(void) {
    pci_device_t dev;
    if (pci_find_device(0x01, 0x06, 0x01, &dev) != SUCCESS) {
        return NOT_FOUND;
    }

    pci_enable_command_bits(&dev, PCI_COMMAND_MEMORY_SPACE | PCI_COMMAND_BUS_MASTER);

    uint32_t abar = pci_bar_address(&dev, 5);
    hba = (volatile hba_mem_t *)vmm_to_device_map(abar);

    hba->ghc |= HBA_GHC_AE;
    ahci_setup_msi(&dev);
    if (!ahci_use_polling) {
        hba->ghc |= HBA_GHC_IE;
    }

    struct device_driver ops = {
        .read       = ahci_read,
        .write      = ahci_write,
        .ioctl      = ahci_dev_ioctl,
        .initialize = ahci_dev_initialize,
        .shutdown   = ahci_dev_shutdown,
    };
    if (devices_new_driver(AHCI_DRIVER_MAJOR, ops) != SUCCESS)
        panic("ahci_init_pnp: failed to register AHCI driver");

    uint32_t pi = hba->pi;
    int found_any = 0;
    for (int i = 0; i < AHCI_MAX_PORTS; i++) {
        if (!(pi & (1u << i))) continue;
        if (!ahci_port_is_sata_drive(&hba->ports[i])) continue;

        ahci_port_init(i);
        if (ahci_identify(&ahci_ports[i]) != SUCCESS) {
            ahci_ports[i].present = 0;
            continue;
        }

        device_minor_t minor = devices_new_device(AHCI_DRIVER_MAJOR, i);
        if (minor < 0) {
            ahci_ports[i].present = 0;
            continue;
        }

        kprintf("ahci: port %d: drive present, %llu sectors\n", i, ahci_ports[i].sector_count);
        if (!found_any) {
            ahci_boot_minor    = minor;
            ahci_has_boot_drive = 1;
        }
        found_any = 1;
    }

    return found_any ? SUCCESS : NOT_FOUND;
}

uint8_t ahci_get_boot_drive(device_minor_t *minor) {
    if (!ahci_has_boot_drive) return 0;
    *minor = ahci_boot_minor;
    return 1;
}
