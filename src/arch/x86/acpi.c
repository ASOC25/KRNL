#include <krnl/arch/x86/acpi.h>
#include <krnl/mem/vmm.h>
#include <krnl/boot/bootloaders/bootloader.h>
#include <krnl/libraries/std/string.h>
#include <krnl/debug/debug.h>
#include <krnl/arch/x86/io.h>

acpi_headers_t headers = {0};

const char acpi_signatures[][4] = {
    BERT_SIGNATURE,
    BOOT_SIGNATURE,
    BGRT_SIGNATURE,
    CPEP_SIGNATURE,
    CSRT_SIGNATURE,
    DBG2_SIGNATURE,
    DBGP_SIGNATURE,
    DSDT_SIGNATURE,
    DMAR_SIGNATURE,
    DRTM_SIGNATURE,
    ECDT_SIGNATURE,
    EINJ_SIGNATURE,
    ERST_SIGNATURE,
    ETDT_SIGNATURE,
    FACS_SIGNATURE,
    FADT_SIGNATURE,
    FPDT_SIGNATURE,
    GTDT_SIGNATURE,
    HEST_SIGNATURE,
    HPET_SIGNATURE,
    IBFT_SIGNATURE,
    IORT_SIGNATURE,
    IVRS_SIGNATURE,
    LPIT_SIGNATURE,
    MADT_SIGNATURE,
    MCFG_SIGNATURE,
    MCHI_SIGNATURE,
    MPST_SIGNATURE,
    MSCT_SIGNATURE,
    MSDM_SIGNATURE,
    NFIT_SIGNATURE,
    OEMx_SIGNATURE,
    PCCT_SIGNATURE,
    PMTT_SIGNATURE,
    PSDT_SIGNATURE,
    RASF_SIGNATURE,
    SBST_SIGNATURE,
    SLIC_SIGNATURE,
    SLIT_SIGNATURE,
    SPCR_SIGNATURE,
    SPMI_SIGNATURE,
    SRAT_SIGNATURE,
    SSDT_SIGNATURE,
    STAO_SIGNATURE,
    TCPA_SIGNATURE,
    TPM2_SIGNATURE,
    UEFI_SIGNATURE,
    WAET_SIGNATURE,
    WDAT_SIGNATURE,
    WDRT_SIGNATURE,
    WPBT_SIGNATURE,
    XENV_SIGNATURE,
    XSDT_SIGNATURE
};
uint8_t acpi_signature_count = sizeof(acpi_signatures) / 4;

uint8_t acpi_sdt_checksum(struct acpi_sdt_header* table_header)
{
    uint8_t sum = 0;
 
    for (uint32_t i = 0; i < table_header->length; i++)
    {
        sum += ((uint8_t *) table_header)[i];
    }
 
    return (sum == 0);
}

struct acpi_sdt_header* find_rsdt(struct rsdt* rsdt, const char* signature, uint64_t sig_len) {
    uint32_t entries = (rsdt->header.length - sizeof(struct acpi_sdt_header)) / sizeof(uint32_t);

    for (uint32_t i = 0; i < entries; i++) {
        uint32_t pto = rsdt->pointer_other_sdt[i];
        struct acpi_sdt_header* header = (struct acpi_sdt_header*)vmm_to_identity_map((uint64_t)pto); //TODO: delete this inmediately

        if (!memcmp(header->signature, signature, sig_len))
            return header;
    }

    return 0;
}

struct acpi_sdt_header* find_xsdt(struct xsdt* xsdt, const char* signature, uint64_t sig_len) {
    uint32_t entries = (xsdt->header.length - sizeof(struct acpi_sdt_header)) / sizeof(uint64_t);

    for (uint32_t i = 0; i < entries; i++) {
        uint32_t pto = xsdt->pointer_other_sdt[i];
        struct acpi_sdt_header* header =  (struct acpi_sdt_header*)vmm_to_identity_map((uint64_t)pto); //TODO: delete this inmediately

        if (!memcmp(header->signature, signature, sig_len))
            return header;
    }

    return 0;
}

void* init_acpi_vt(void* rsdp_address, const char* target) {
    char signature[9];
    char oemid[7];

    struct rsdp2_descriptor* rsdp = (struct rsdp2_descriptor*)rsdp_address;

    if (memcmp(rsdp->first_part.signature, "RSD PTR ", 8))
        panic("RSDP signature mismatch");

    strncpy(oemid, rsdp->first_part.oem_id, 6);
    strncpy(signature, rsdp->first_part.signature, 8);

    struct xsdt* xsdt = (struct xsdt*)vmm_to_identity_map((uint64_t)(rsdp->xsdt_address));
    struct acpi_sdt_header* result = find_xsdt(xsdt, target, 4);
    if (result == 0) {
        return 0;
    }

    if (!acpi_sdt_checksum(result))
        panic("XSDT checksum mismatch");

    return (void*)result;
}

void* init_acpi_vz(void* rsdp_address, const char * target) {
    char signature[9];
    char oemid[7];

    struct rsdp_descriptor* rsdp = (struct rsdp_descriptor*)rsdp_address;

    if (memcmp(rsdp->signature, "RSD PTR ", 8))
        panic("RSDP signature mismatch");

    strncpy(oemid, rsdp->oem_id, 6);
    strncpy(signature, rsdp->signature, 8);

    struct rsdt* rsdt = (struct rsdt*)vmm_to_identity_map((uint64_t)(rsdp->rsdt_address));
    struct acpi_sdt_header* result = find_rsdt(rsdt, target, 4);

    if (!acpi_sdt_checksum(result))
        panic("RSDT checksum mismatch");

    return (void*)result;
}

uint8_t is_enabled(acpi_fadt_header_t* fadt_header) {
    //Check if ACPI is enabled
    if (fadt_header->smi_command_port != 0) return 0;
    if (fadt_header->acpi_enable != 0 || fadt_header->acpi_disable != 0) return 0;
    if (!(fadt_header->pm1a_event_block & 1)) return 0;

    return 1;
}

acpi_mcfg_header_t * acpi_get_mcfg() {
    char signature[9];
    char oemid[7];
    char oemtableid[9];

    void* rsdp_address = (void*)get_rsdp_address();
    if (rsdp_address == 0) {
        panic("RSDP not found");
    }
    struct rsdp_descriptor* prev_rsdp = (struct rsdp_descriptor*)rsdp_address;
    acpi_mcfg_header_t* mcfg_header = 0x0;

    if (prev_rsdp->revision == 0) {
        mcfg_header = (acpi_mcfg_header_t*)init_acpi_vz(rsdp_address, "MCFG");
    } else if (prev_rsdp->revision == 2) {
        mcfg_header = (acpi_mcfg_header_t*)init_acpi_vt(rsdp_address, "MCFG");
    } else {
        panic("RSDP revision not supported");
    }

    if (mcfg_header == 0) {
        panic("MCFG table not found");
    }

    strncpy(signature, mcfg_header->header.signature, 4);
    strncpy(oemtableid, mcfg_header->header.oem_table_id, 8);
    strncpy(oemid, mcfg_header->header.oem_id, 6);

    return mcfg_header;
}

acpi_madt_header_t * acpi_get_madt() {
    char signature[9];
    char oemid[7];
    char oemtableid[9];

    void* rsdp_address = (void*)get_rsdp_address();
    if (rsdp_address == 0) {
        panic("RSDP not found");
    }
    struct rsdp_descriptor* prev_rsdp = (struct rsdp_descriptor*)rsdp_address;
    acpi_madt_header_t* madt_header = 0x0;

    if (prev_rsdp->revision == 0) {
        madt_header = (acpi_madt_header_t*)init_acpi_vz(rsdp_address, "APIC");
    } else if (prev_rsdp->revision == 2) {
        madt_header = (acpi_madt_header_t*)init_acpi_vt(rsdp_address, "APIC");
    } else {
        panic("RSDP revision not supported");
    }   

    if (madt_header == 0) {
        panic("MADT table not found");
    }

    strncpy(signature, madt_header->header.signature, 4);
    strncpy(oemtableid, madt_header->header.oem_table_id, 8);
    strncpy(oemid, madt_header->header.oem_id, 6);

    return madt_header;
}

acpi_fadt_header_t * acpi_get_fadt() {
    char signature[9];
    char oemid[7];
    char oemtableid[9];

    void* rsdp_address = (void*)get_rsdp_address();
    if (rsdp_address == 0) {
        panic("RSDP not found");
    }
    struct rsdp_descriptor* prev_rsdp = (struct rsdp_descriptor*)rsdp_address;
    acpi_fadt_header_t* fadt_header = 0x0;

    if (prev_rsdp->revision == 0) {
        fadt_header = (acpi_fadt_header_t*)init_acpi_vz(rsdp_address, "FACP");
    } else if (prev_rsdp->revision == 2) {
        fadt_header = (acpi_fadt_header_t*)init_acpi_vt(rsdp_address, "FACP");
    } else {
        panic("RSDP revision not supported");
    }   

    if (fadt_header == 0) {
        panic("FADT table not found");
    }

    strncpy(signature, fadt_header->header.signature, 4);
    strncpy(oemtableid, fadt_header->header.oem_table_id, 8);
    strncpy(oemid, fadt_header->header.oem_id, 6);

    return fadt_header;
}

acpi_hpet_header_t * acpi_get_hpet() {
    char signature[9];
    char oemid[7];
    char oemtableid[9];

    void* rsdp_address = (void*)get_rsdp_address();
    if (rsdp_address == 0) {
        panic("RSDP not found");
    }
    struct rsdp_descriptor* prev_rsdp = (struct rsdp_descriptor*)rsdp_address;
    acpi_hpet_header_t* hpet_header = 0x0;

    if (prev_rsdp->revision == 0) {
        hpet_header = (acpi_hpet_header_t*)init_acpi_vz(rsdp_address, "HPET");
    } else if (prev_rsdp->revision == 2) {
        hpet_header = (acpi_hpet_header_t*)init_acpi_vt(rsdp_address, "HPET");
    } else {
        panic("RSDP revision not supported");
    }   

    if (hpet_header == 0) {
        panic("HPET table not found");
    }

    strncpy(signature, hpet_header->header.signature, 4);
    strncpy(oemtableid, hpet_header->header.oem_table_id, 8);
    strncpy(oemid, hpet_header->header.oem_id, 6);

    return hpet_header;
}

void acpi_init() {
    acpi_fadt_header_t* fadt_header = acpi_get_fadt();
    if (is_enabled(fadt_header)) {return;}
    if (inw(fadt_header->pm1a_control_block) & 1) {return;}
    outb(fadt_header->smi_command_port, fadt_header->acpi_enable);
    while (((inw(fadt_header->pm1a_control_block) & 1) == 0));
}

acpi_headers_t * acpi_get_headers() {
    if (headers.valid)
        return &headers;

    acpi_init();
    headers.mcfg_header = acpi_get_mcfg();
    headers.madt_header = acpi_get_madt();
    headers.hpet_header = acpi_get_hpet();
    headers.valid = 1;
    return &headers;
}