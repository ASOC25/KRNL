#include <krnl/mem/mem.h>
#include <krnl/mem/vmm.h>
#include <krnl/mem/pmm.h>
#include <krnl/debug/debug.h>

void mem_init(void) {
    pmm_init();

    vmm_root_t * root = vmm_get_root();
    if (root == NULL) {
        panic("init_vmm: Current root is NULL");
    }

    vmm_root_t * new_root = vmm_duplicate_kspace();
    if (new_root == NULL) {
        panic("init_vmm: Failed to duplicate kernel space for new root");
    }
    
    status_t st;
    uint64_t physical_pages = VMM_PHYSICAL_MEMORY_SIZE / VMM_PAGE_SIZE_1GB;

    st = vmm_map_pages(new_root, VMM_REGION_K_IDENT, 0, physical_pages, VMM_PAGE_SIZE_1GB, VMM_WRITE_BIT);
    if (st != SUCCESS) {
        panic("init_vmm: Failed to map physical memory");
    }

    st = vmm_map_pages(new_root, VMM_REGION_K_DEVICES, 0, physical_pages, VMM_PAGE_SIZE_1GB, VMM_WRITE_BIT | VMM_CACHE_DISABLE_BIT);
    if (st != SUCCESS) {
        panic("init_vmm: Failed to map device memory");
    }

    st = vmm_map_pages(new_root, VMM_REGION_K_STACK, 0, physical_pages, VMM_PAGE_SIZE_1GB, VMM_USER_BIT | VMM_WRITE_BIT);
    if (st != SUCCESS) {
        panic("init_vmm: Failed to map kernel stack memory");
    }

    vmm_remap(VMM_REGION_K_IDENT);
    pmm_remap(VMM_REGION_K_IDENT); //Kind of ugly dependency to avoid vm knowing pmm
    vmm_set_root((vmm_root_t*)vmm_to_identity_map((uint64_t)new_root));
}