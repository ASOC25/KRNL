#include <krnl/arch/x86/vm.h>
#include <krnl/mem/vmm.h>
#include <krnl/mem/pmm.h>
#include <krnl/debug/debug.h>

status_t vmm_map_pages(vmm_root * root, uint64_t virtual_address_start, uint64_t physical_address_start, uint64_t pages, uint64_t page_size, uint8_t flags) {
    if (root == 0) {
        panic("vmm_map_pages: root page directory is NULL");
    }
    if (virtual_address_start % page_size != 0 || physical_address_start % page_size != 0) {
        panic("vmm_map_pages: Addresses must be aligned to page size");
    }

    for (uint64_t i = 0; i < pages; i++) {
        uint64_t virtual_address = virtual_address_start + (i * page_size);
        uint64_t physical_address = physical_address_start + (i * page_size);
        status_t status = vm_map_address((vm_dir *)root, virtual_address, physical_address, page_size, flags);
        if (status != SUCCESS) {
            panic("vmm_map_pages: Failed to map page %llu", i);
        }
    }

    return SUCCESS;
}

status_t vmm_unmap_pages(vmm_root * root, uint64_t virtual_address_start, uint64_t pages, uint64_t page_size) {
    if (root == 0) {
        panic("vmm_unmap_pages: root page directory is NULL");
    }
    if (virtual_address_start % page_size != 0) {
        panic("vmm_unmap_pages: Virtual address must be aligned to page size");
    }

    for (uint64_t i = 0; i < pages; i++) {
        uint64_t virtual_address = virtual_address_start + (i * page_size);
        status_t status = vm_unmap_address((vm_dir *)root, virtual_address);
        if (status != SUCCESS) {
            panic("vmm_unmap_pages: Failed to unmap page %llu", i);
        }
    }

    return SUCCESS;
}

status_t vmm_mprotect_pages(vmm_root * root, uint64_t virtual_address_start, uint64_t pages, uint64_t page_size, uint8_t new_flags) {
    if (root == 0) {
        panic("vmm_mprotect_pages: root page directory is NULL");
    }
    if (virtual_address_start % page_size != 0) {
        panic("vmm_mprotect_pages: Virtual address must be aligned to page size");
    }

    for (uint64_t i = 0; i < pages; i++) {
        uint64_t virtual_address = virtual_address_start + (i * page_size);
        status_t status = vm_mprotect_address((vm_dir *)root, virtual_address, new_flags);
        if (status != SUCCESS) {
            panic("vmm_mprotect_pages: Failed to mprotect page %llu", i);
        }
    }

    return SUCCESS;
}

void vmm_remap(uint64_t offset) {
    vm_set_offset(offset);
}

vmm_root * vmm_get_root() {
    return (vmm_root *)vm_get_current_pml4();
}

void vmm_set_root(vmm_root * root) {
    vm_set_current_pml4((vm_dir *)root);
}

vmm_root * vmm_duplicate_kspace() {
    vmm_root * current_pml4 = vmm_get_root();
    return (vmm_root *)vm_duplicate_pml4((vm_dir *)current_pml4, VM_COPY_KERNEL_ONLY);
}

vmm_root * vmm_duplicate_fullspace(vmm_root * original) {
    return (vmm_root *)vm_duplicate_pml4((vm_dir *)original, VM_COPY_ALL);
}

void vmm_free_root(vmm_root * root) {
    vm_flush_tlb_entry((uint64_t)root);
}

uint64_t vmm_to_identity_map(uint64_t address) {
    return vm_to_identity_map(address);
}

uint64_t vmm_from_identity_map(uint64_t address) {
    return vm_from_identity_map(address);
}

uint64_t vmm_to_device_map(uint64_t address) {
    return (uint64_t)(address + VMM_REGION_DEVICES);
}

uint64_t vmm_from_device_map(uint64_t address) {
    return (uint64_t)(address - VMM_REGION_DEVICES);
}

status_t vmm_get_physical_address(vmm_root * root, uint64_t virtual_address, uint64_t * physical_address) {
    return vm_get_physical_address((vm_dir *)root, virtual_address, physical_address);
}

status_t vmm_get_page_info(vmm_root * root, uint64_t virtual_address, vmm_info * info) {
    struct page_info inf;
    status_t st = vm_get_page_info((vm_dir *)root, virtual_address, &inf);
    if (st != SUCCESS) {
        return st;
    }

    info->page_size = inf.page_size;
    info->permissions.read_write = inf.permissions.read_write;
    info->permissions.user = inf.permissions.user;
    info->permissions.write_through = inf.permissions.write_through;
    info->permissions.cache_disable = inf.permissions.cache_disable;
    info->permissions.global = inf.permissions.global;
    info->permissions.no_execute = inf.permissions.no_execute;
    
    return SUCCESS;
}