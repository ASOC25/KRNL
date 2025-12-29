#include <krnl/arch/x86/vm.h>
#include <krnl/mem/vmm.h>
#include <krnl/mem/pmm.h>
#include <krnl/debug/debug.h>

#include <krnl/libraries/assert/assert.h>


status_t vmm_map_pages(vmm_root_t * root, uint64_t virtual_address_start, uint64_t physical_address_start, uint64_t pages, uint64_t page_size, uint8_t flags) {
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

status_t vmm_unmap_pages(vmm_root_t * root, uint64_t virtual_address_start, uint64_t pages, uint64_t page_size) {
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

status_t vmm_mprotect_pages(vmm_root_t * root, uint64_t virtual_address_start, uint64_t pages, uint64_t page_size, uint8_t new_flags) {
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

vmm_root_t * vmm_get_root() {

    vmm_root_t * current_pml4 = (vmm_root_t *)vm_get_current_pml4();

    return current_pml4;
}

void vmm_set_root(vmm_root_t * root) {

    vm_set_current_pml4((vm_dir *)root);

}

vmm_root_t * vmm_duplicate_kspace() {
    vmm_root_t * current_pml4 = vmm_get_root();

    vmm_root_t * duplicate = (vmm_root_t *)vm_duplicate_pml4((vm_dir *)current_pml4, VM_COPY_KERNEL_ONLY);

    return duplicate;
}

vmm_root_t * vmm_duplicate_fullspace(vmm_root_t * original) {

    vmm_root_t * duplicate = (vmm_root_t *)vm_duplicate_pml4((vm_dir *)original, VM_COPY_ALL);

    return duplicate;
}

void vmm_free_root(vmm_root_t * root) {

    vm_deallocate_vspace((vm_dir *)root);

}

uint64_t vmm_to_identity_map(uint64_t address) {

    uint64_t result = vm_to_identity_map(address);

    return result;
}

uint64_t vmm_from_identity_map(uint64_t address) {

    uint64_t result = vm_from_identity_map(address);

    return result;
}

uint64_t vmm_to_device_map(uint64_t address) {

    uint64_t result = (uint64_t)(address + VMM_REGION_K_DEVICES);

    return result;
}

uint64_t vmm_from_device_map(uint64_t address) {

    uint64_t result = (uint64_t)(address - VMM_REGION_K_DEVICES);

    return result;
}

status_t vmm_get_physical_address(vmm_root_t * root, uint64_t virtual_address, uint64_t * physical_address) {

    status_t status = vm_get_physical_address((vm_dir *)root, virtual_address, physical_address);

    return status;
}

uint8_t vmm_check_and_clean_dirty(vmm_root_t * root, uint64_t virtual_address, uint64_t pages, uint64_t page_size) {

    for (uint64_t i = 0; i < pages; i++) {
        uint64_t va = virtual_address + (i * page_size);
        uint8_t dirty = vm_check_and_clean_dirty((vm_dir *)root, va);
        if (dirty == 1) {

            return 1;
        }
    }

    return 0;
}

status_t vmm_get_page_info(vmm_root_t * root, uint64_t virtual_address, vmm_info * info) {

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