#include <krnl/mem/mem.h>
#include <krnl/mem/vmm.h>
#include <krnl/mem/pmm.h>
#include <krnl/debug/debug.h>

void mem_init(void) {
    pmm_init();

    vmm_root * root = vmm_get_root();
    if (root == NULL) {
        panic("init_vmm: Current root is NULL");
    }
    
    status_t st;
    uint64_t physical_pages = VMM_PHYSICAL_MEMORY_SIZE / VMM_PAGE_SIZE_1GB;

    st = vmm_map_pages(root, VMM_REGION_K_IDENT, 0, physical_pages, VMM_PAGE_SIZE_1GB, VMM_WRITE_BIT);
    if (st != SUCCESS) {
        panic("init_vmm: Failed to map physical memory");
    }

    st = vmm_map_pages(root, VMM_REGION_DEVICES, 0, physical_pages, VMM_PAGE_SIZE_1GB, VMM_WRITE_BIT | VMM_CACHE_DISABLE_BIT);
    if (st != SUCCESS) {
        panic("init_vmm: Failed to map device memory");
    }
    
    vmm_remap(0xFFFFB00000000000);
    pmm_remap(0xFFFFB00000000000); //Kind of ugly dependency
    
    vmm_root * global_cr3 = vmm_duplicate_kspace();
    if (global_cr3 == NULL) {
        panic("init_vmm: Failed to duplicate kernel space for global CR3");
    }

    vmm_set_root(global_cr3);
}