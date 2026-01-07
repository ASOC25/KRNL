#include <krnl/arch/x86/vm.h>
#include <krnl/mem/pmm.h>
#include <krnl/debug/debug.h>
#include <krnl/libraries/std/string.h>
#include <krnl/libraries/std/stddef.h>

#define VM_WRITE_BIT_SET(flags)            ((flags) & VM_WRITE_BIT)
#define VM_USER_BIT_SET(flags)             ((flags) & VM_USER_BIT)
#define VM_CACHE_BIT_SET(flags)            ((flags) & VM_CACHE_DISABLE_BIT)
#define VM_NX_BIT_SET(flags)               ((flags) & VM_NX_BIT)
#define VM_WRITE_THROUGH_BIT_SET(flags)    ((flags) & VM_WRITE_THROUGH_BIT)
#define VM_GLOBAL_BIT_SET(flags)           ((flags) & VM_GLOBAL_BIT)
#define VM_CACHE_DISABLE_BIT_SET(flags)    ((flags) & VM_CACHE_DISABLE_BIT)

#define IS_PRESENT(entry)                   ((entry)->directory.P)
#define IS_WRITEABLE(entry)                 ((entry)->directory.RW)
#define IS_USER_ACCESS(entry)               ((entry)->directory.US)
#define IS_EXECUTABLE(entry)                (!((entry)->directory.XD))

#define GET_PDPP_HUGE(entry)                ((uint64_t)(((uint64_t)(entry)->huge.PDPP) << 30))
#define GET_PDPP_BIG(entry)                 ((uint64_t)(((uint64_t)(entry)->big.PDPP) << 21))
#define GET_PDPP_REGULAR(entry)             ((uint64_t)(((uint64_t)(entry)->regular.PDPP) << 12))
#define GET_PDPP_DIR(entry)                 ((uint64_t)(((uint64_t)(entry)->directory.PDPP) << 12))
#define GET_ENTRY(root, index)              ((vm_entry*)(&((root)->entries[(index)])))

#define TO_IDENTITY_MAP(addr) (uint64_t)(((uint64_t)addr) + (uint64_t)physical_memory_offset)
#define FROM_IDENTITY_MAP(addr) (uint64_t)(((uint64_t)addr) - (uint64_t)physical_memory_offset)

uint64_t physical_memory_offset = 0;

void address_to_map(uint64_t address, struct page_map_index* map) {
    address >>= 12;
    map->PT_index = address & 0x1ff;
    address >>= 9;
    map->PD_index = address & 0x1ff;
    address >>= 9;
    map->PDP_index = address & 0x1ff;
    address >>= 9;    
    map->PML4_index = address & 0x1ff;
}

void map_to_address(struct page_map_index* map, uint64_t* address) {
    *address = map->PML4_index;
    *address <<= 9;
    *address |= map->PDP_index;
    *address <<= 9;
    *address |= map->PD_index;
    *address <<= 9;
    *address |= map->PT_index;
    *address <<= 12;
}

uint64_t get_pdpp(vm_entry * entry, uint64_t size)
{
    switch (size)
    {
        case VM_PAGE_SIZE_1GB:
            return TO_IDENTITY_MAP(GET_PDPP_HUGE(entry));
        case VM_PAGE_SIZE_2MB:
            return TO_IDENTITY_MAP(GET_PDPP_BIG(entry));
        case VM_PAGE_SIZE_4KB:
            return TO_IDENTITY_MAP(GET_PDPP_REGULAR(entry));
        default:
            return TO_IDENTITY_MAP(GET_PDPP_DIR(entry));
    }
}

void * allocate_phys_page() {
    void * buffer = pmm_alloc_pages(1);
    if (buffer == NULL)
    {
        return NULL;
    }
    memset((void*)TO_IDENTITY_MAP(buffer), 0, VM_PAGE_SIZE_4KB);
    return buffer;
}

void init_entry(vm_entry * entry, uint64_t size, uint64_t page_ppn, vm_perms perms)
{   

    //Check that page_ppn is aligned to the size, else panic
    if (size == VM_PAGE_SIZE_1GB && (page_ppn & 0x3fffffff) != 0)
    {
        panic("Page ppn is not aligned to 1GiB page size");
    }

    if (size == VM_PAGE_SIZE_2MB && (page_ppn & 0x1fffff) != 0)
    {
        panic("Page ppn is not aligned to 2MiB page size");
    }

    if (size == VM_PAGE_SIZE_4KB && (page_ppn & 0xfff) != 0)
    {
        panic("Page ppn is not aligned to 4KiB page size");
    }

    entry->directory.P = 1;          //0
    entry->directory.RW = perms.read_write; //1
    entry->directory.US = perms.user; //2
    entry->directory.PWT = perms.write_through; //3
    entry->directory.PCD = perms.cache_disable; //4
    entry->directory.A = 0;          //5
    entry->directory.IGNORED1 = 0;          //6
    entry->directory.PS = 0;         //7
    entry->directory.IGNORED2 = 0;   //8-10
    entry->directory.R = 0;          //11
    entry->directory.PDPP = 0;         //12-39 Provisionally set to 0
    entry->directory.RESERVED = 0;   //40-51
    entry->directory.IGNORED3 = 0;   //52-62
    entry->directory.XD = perms.no_execute; //63

    switch (size)
    {
        case VM_PAGE_SIZE_1GB:
            entry->huge.PS = 1;
            entry->huge.PDPP = page_ppn >> 30;
            break;
        case VM_PAGE_SIZE_2MB:
            entry->big.PS = 1;
            entry->big.PDPP = page_ppn >> 21;
            break;
        case VM_PAGE_SIZE_4KB:
            entry->regular.PDPP = page_ppn >> 12;
            break;
        default:
            entry->directory.PDPP = page_ppn >> 12;
            break;
    }
}

void vm_duplicate(vm_dir* root, vm_dir* new_root, uint8_t level, int override, uint8_t root_on_phys)
{
    for (int i = override; i < 512; i++)
    {
        vm_entry* entry = GET_ENTRY(root, i);
        vm_entry* new_entry = GET_ENTRY(new_root, i);
        if (IS_PRESENT(entry))
        {
            new_entry->directory.P = entry->directory.P;
            new_entry->directory.RW = entry->directory.RW;
            new_entry->directory.US = entry->directory.US;
            new_entry->directory.PWT = entry->directory.PWT;
            new_entry->directory.PCD = entry->directory.PCD;
            new_entry->directory.A = entry->directory.A;
            new_entry->directory.IGNORED1 = entry->directory.IGNORED1;
            new_entry->directory.PS = entry->directory.PS;
            new_entry->directory.IGNORED2 = entry->directory.IGNORED2;
            new_entry->directory.R = entry->directory.R;
            new_entry->directory.PDPP = 0; //CHANGED LATED
            new_entry->directory.RESERVED = entry->directory.RESERVED;
            new_entry->directory.IGNORED3 = entry->directory.IGNORED3;
            new_entry->directory.XD = entry->directory.XD;

            if (level == 0 || entry->directory.PS)
            {
                new_entry->directory.PDPP = entry->directory.PDPP;
            } else {
                new_entry->directory.PDPP = ((uint64_t)allocate_phys_page()) >> 12;

                uint64_t source_pdpp = (root_on_phys) ? GET_PDPP_DIR(entry) : get_pdpp(entry, VM_PAGE_SIZE_DIR);

                vm_duplicate(
                    (struct page_directory*)source_pdpp,
                    (struct page_directory*)get_pdpp(new_entry, VM_PAGE_SIZE_DIR),
                    level - 1,
                    0,
                    root_on_phys
                );
            }
        }
    }
}

void vm_set_offset(uint64_t offset) {
    physical_memory_offset = offset;
}

uint64_t vm_to_identity_map(uint64_t address) {
    return TO_IDENTITY_MAP(address);
}

uint64_t vm_from_identity_map(uint64_t address) {
    return FROM_IDENTITY_MAP(address);
}

status_t vm_get_physical_address(vm_dir* root, uint64_t virtual_address, uint64_t* physical_address) {
    struct page_map_index indices;
    address_to_map((uint64_t)virtual_address, &indices);

    vm_dir *pdptable, *pdtable, *pttable;
    vm_entry *pml4entry, *pdptentry, *pdentry, *ptentry;

    pml4entry = GET_ENTRY(root, indices.PML4_index);
    if (!IS_PRESENT(pml4entry)) {
        panic("vm_get_physical_address: PML4 entry not present");
    }

    pdptable = (vm_dir *)(get_pdpp(pml4entry, VM_PAGE_SIZE_DIR));
    pdptentry = GET_ENTRY(pdptable, indices.PDP_index);
    if (!IS_PRESENT(pdptentry)) {
        panic("vm_get_physical_address: PDPT entry not present");
    } else if (pdptentry->huge.PS) {
        *physical_address = GET_PDPP_HUGE(pdptentry) + (virtual_address & 0x3fffffff);
        return SUCCESS;
    }

    pdtable = (vm_dir *)(get_pdpp(pdptentry, VM_PAGE_SIZE_DIR));
    pdentry = GET_ENTRY(pdtable, indices.PD_index);
    if (!IS_PRESENT(pdentry)) {
        panic("vm_get_physical_address: PD entry not present");
    } else if (pdentry->big.PS) {
        *physical_address = GET_PDPP_BIG(pdentry) + (virtual_address & 0x1fffff);
        return SUCCESS;
    }

    pttable = (vm_dir *)(get_pdpp(pdentry, VM_PAGE_SIZE_DIR));
    ptentry = GET_ENTRY(pttable, indices.PT_index);
    if (!IS_PRESENT(ptentry)) {
        panic("vm_get_physical_address: PT entry not present");
    }

    *physical_address = GET_PDPP_REGULAR(ptentry) + (virtual_address & 0xfff);
    return SUCCESS;
}

status_t vm_get_page_info(vm_dir * root, uint64_t virtual_address, struct page_info* info) {
    uint64_t physical_address;
    status_t status = vm_get_physical_address(root, virtual_address, &physical_address);
    if (status != SUCCESS) {
        return status;
    }

    struct page_map_index indices;
    address_to_map((uint64_t)virtual_address, &indices);
    info->indices = indices;

    vm_dir *pdptable, *pdtable, *pttable;
    vm_entry *pml4entry, *pdptentry, *pdentry, *ptentry;

    pml4entry = GET_ENTRY(root, indices.PML4_index);
    if (!IS_PRESENT(pml4entry)) {
        panic("vm_get_page_info: PML4 entry not present");
    }

    if (!IS_PRESENT(pml4entry)) {
        panic("vm_get_page_info: PML4 entry not present");
    } else if (pml4entry->huge.PS) {
        panic("vm_get_page_info: Huge pages not supported at PML4 level");
    }

    pdptable = (vm_dir *)(get_pdpp(pml4entry, VM_PAGE_SIZE_DIR));
    pdptentry = GET_ENTRY(pdptable, indices.PDP_index);
    if (!IS_PRESENT(pdptentry)) {
        panic("vm_get_page_info: PDPT entry not present");
    } else if (pdptentry->huge.PS) {
        info->page_size = VM_PAGE_SIZE_1GB;
        info->permissions.read_write = IS_WRITEABLE(pdptentry);
        info->permissions.user = IS_USER_ACCESS(pdptentry);
        info->permissions.no_execute = IS_EXECUTABLE(pdptentry) ? 0 : 1;
        info->physical_address = GET_PDPP_HUGE(pdptentry) + (virtual_address & 0x3fffffff);
        return SUCCESS;
    }

    pdtable = (vm_dir *)(get_pdpp(pdptentry, VM_PAGE_SIZE_DIR));
    pdentry = GET_ENTRY(pdtable, indices.PD_index);
    if (!IS_PRESENT(pdentry)) {
        panic("vm_get_page_info: PD entry not present");
    } else if (pdentry->big.PS) {
        info->page_size = VM_PAGE_SIZE_2MB;
        info->permissions.read_write = IS_WRITEABLE(pdentry);
        info->permissions.user = IS_USER_ACCESS(pdentry);
        info->permissions.no_execute = IS_EXECUTABLE(pdentry) ? 0 : 1;
        info->physical_address = GET_PDPP_BIG(pdentry) + (virtual_address & 0x1fffff);
        return SUCCESS;
    }

    pttable = (vm_dir *)(get_pdpp(pdentry, VM_PAGE_SIZE_DIR));
    ptentry = GET_ENTRY(pttable, indices.PT_index);
    if (!IS_PRESENT(ptentry)) {
        panic("vm_get_page_info: PT entry not present");
    }

    info->page_size = VM_PAGE_SIZE_4KB;
    info->permissions.read_write = IS_WRITEABLE(ptentry);
    info->permissions.user = IS_USER_ACCESS(ptentry);
    info->permissions.no_execute = IS_EXECUTABLE(ptentry) ? 0 : 1;
    info->physical_address = GET_PDPP_REGULAR(ptentry) + (virtual_address & 0xfff);
    return SUCCESS;
}

vm_dir * vm_get_current_pml4() {
    vm_dir* cr3;
    __asm__("mov %%cr3, %0" : "=r"(cr3));
    return (vm_dir*)TO_IDENTITY_MAP((uint64_t)cr3);
}

vm_dir * vm_duplicate_pml4(vm_dir * pml4, vm_copy_range range) {
    vm_dir* new_pml4 = (vm_dir*)TO_IDENTITY_MAP((uint64_t)allocate_phys_page());
    if (new_pml4 == NULL) {
        panic("vm_duplicate_pml4: Failed to allocate memory for new PML4");
    }

    switch (range) {
        case VM_COPY_ALL:
            vm_duplicate(pml4, new_pml4, 3, 0, 0);
            break;
        case VM_COPY_KERNEL_ONLY:
            vm_duplicate(pml4, new_pml4, 3, 256, 0);
            break;
        default:
            panic("vm_duplicate_pml4: Invalid copy range");
    }

    return new_pml4;
}

void vm_set_current_pml4(vm_dir * pml4) {
    vm_dir * physical_pml4 = (vm_dir*)FROM_IDENTITY_MAP((uint64_t)pml4);
    __asm__("mov %0, %%cr3" : : "r"(physical_pml4) : "memory");
}

void vm_flush_tlb_entry(uint64_t address)
{
    __asm__("invlpg (%0)" : : "r"(address) : "memory");
}


//This fun
uint8_t vm_check_and_clean_dirty(vm_dir * root, uint64_t virtual_address) {
    struct page_map_index indices;
    address_to_map((uint64_t)virtual_address, &indices);

    vm_dir *pdptable, *pdtable, *pttable;
    vm_entry *pml4entry, *pdptentry, *pdentry, *ptentry;

    pml4entry = GET_ENTRY(root, indices.PML4_index);
    if (!IS_PRESENT(pml4entry)) {
        panic("vm_is_dirty: PML4 entry not present");
    }

    pdptable = (vm_dir *)(get_pdpp(pml4entry, VM_PAGE_SIZE_DIR));
    pdptentry = GET_ENTRY(pdptable, indices.PDP_index);
    if (!IS_PRESENT(pdptentry)) {
        panic("vm_is_dirty: PDPT entry not present");
    } else if (pdptentry->huge.PS) {
        return pdptentry->huge.D ? SUCCESS : FAILURE;
    }

    pdtable = (vm_dir *)(get_pdpp(pdptentry, VM_PAGE_SIZE_DIR));
    pdentry = GET_ENTRY(pdtable, indices.PD_index);
    if (!IS_PRESENT(pdentry)) {
        panic("vm_is_dirty: PD entry not present");
    } else if (pdentry->big.PS) {
        return pdentry->big.D ? SUCCESS : FAILURE;
    }

    pttable = (vm_dir *)(get_pdpp(pdentry, VM_PAGE_SIZE_DIR));
    ptentry = GET_ENTRY(pttable, indices.PT_index);
    if (!IS_PRESENT(ptentry)) {
        panic("vm_is_dirty: PT entry not present");
    }
    uint8_t was_dirty = ptentry->regular.D;
    ptentry->regular.D = 0;
    vm_flush_tlb_entry(virtual_address);
    return was_dirty;
}

status_t vm_map_address(vm_dir * root, uint64_t virtual_address, uint64_t physical_address, uint64_t page_size, uint8_t flags) {
    if (root == 0) {
        panic("vm_map_address: root page directory is NULL");
    }
    if (virtual_address % page_size != 0 || physical_address % page_size != 0) {
        panic("vm_map_address: Addresses must be aligned to page size");
    }
    if (page_size != VM_PAGE_SIZE_1GB && page_size != VM_PAGE_SIZE_2MB && page_size != VM_PAGE_SIZE_4KB) {
        panic("vm_map_address: Invalid page size");
    }

    struct page_map_index indices;
    address_to_map(virtual_address, &indices);

    vm_dir *pdptable, *pdtable, *pttable;
    vm_entry *pml4entry, *pdptentry, *pdentry, *ptentry;

    vm_perms perms;
    perms.read_write = 1;
    perms.user = 1;
    perms.write_through = 0;
    perms.cache_disable = 0;
    perms.global = 0;
    perms.no_execute = 0;

    vm_perms page_perms;
    page_perms.read_write = VM_WRITE_BIT_SET(flags) ? 1 : 0;
    page_perms.user = VM_USER_BIT_SET(flags) ? 1 : 0;
    page_perms.write_through = VM_WRITE_THROUGH_BIT_SET(flags) ? 1 : 0;
    page_perms.cache_disable = VM_CACHE_DISABLE_BIT_SET(flags) ? 1 : 0;
    page_perms.global = VM_GLOBAL_BIT_SET(flags) ? 1 : 0;
    page_perms.no_execute = VM_NX_BIT_SET(flags) ? 1 : 0;

    pml4entry = GET_ENTRY(root, indices.PML4_index);
    if (!IS_PRESENT(pml4entry)) {
        pdptable = (vm_dir *)allocate_phys_page();
        if (pdptable == NULL) {
            panic("vm_map_page: Failed to allocate memory for PDPT");
        }
        init_entry(pml4entry, VM_PAGE_SIZE_DIR, (uint64_t)pdptable, perms);
    }

    pdptable = (vm_dir *)(get_pdpp(pml4entry, VM_PAGE_SIZE_DIR));
    pdptentry = GET_ENTRY(pdptable, indices.PDP_index);
    if (!IS_PRESENT(pdptentry)) {
        if (page_size == VM_PAGE_SIZE_1GB) {
            init_entry(pdptentry, VM_PAGE_SIZE_1GB, physical_address, page_perms);
            vm_flush_tlb_entry(virtual_address);
            goto check_mapping;
        } else {
            pdtable = (vm_dir *)allocate_phys_page();
            if (pdtable == NULL) {
                panic("vm_map_page: Failed to allocate memory for PD");
            }
            init_entry(pdptentry, VM_PAGE_SIZE_DIR, (uint64_t)pdtable, perms);
        }
    } else if (pdptentry->huge.PS) {
        panic("vm_map_page: Mapping conflict at PDPT level");
    }

    pdtable = (vm_dir *)(get_pdpp(pdptentry, VM_PAGE_SIZE_DIR));
    pdentry = GET_ENTRY(pdtable, indices.PD_index);
    if (!IS_PRESENT(pdentry)) {
        if (page_size == VM_PAGE_SIZE_2MB) {
            init_entry(pdentry, VM_PAGE_SIZE_2MB, physical_address, page_perms);
            vm_flush_tlb_entry(virtual_address);
            goto check_mapping;
        } else {
            pttable = (vm_dir *)allocate_phys_page();
            if (pttable == NULL) {
                panic("vm_map_page: Failed to allocate memory for PT");
            }
            init_entry(pdentry, VM_PAGE_SIZE_DIR, (uint64_t)pttable, perms);
        }
    } else if (pdentry->big.PS) {
        panic("vm_map_page: Mapping conflict at PD level");
    }

    pttable = (vm_dir *)(get_pdpp(pdentry, VM_PAGE_SIZE_DIR));
    ptentry = GET_ENTRY(pttable, indices.PT_index);
    if (IS_PRESENT(ptentry)) {
        panic("vm_map_page: Mapping conflict at PT level");
    }

    init_entry(ptentry, VM_PAGE_SIZE_4KB, physical_address, page_perms);
    vm_flush_tlb_entry(virtual_address);

check_mapping:
    uint64_t resulting_paddr;
    //Invalidate TLB and verify mapping
    vm_flush_tlb_entry(virtual_address);
    if (vm_get_physical_address(root, virtual_address, &resulting_paddr) != SUCCESS) {
        panic("vm_map_page: Verification of mapping failed");
    }
    if (resulting_paddr != physical_address) {
        panic("vm_map_page: Verification of mapping failed");
    }

    return SUCCESS;
}

status_t vm_unmap_address(vm_dir * root, uint64_t virtual_address) {
    struct page_map_index indices;
    address_to_map(virtual_address, &indices);
    
    vm_dir *pdptable, *pdtable, *pttable;
    vm_entry *pml4entry, *pdptentry, *pdentry, *ptentry;

    pml4entry = GET_ENTRY(root, indices.PML4_index);
    if (!IS_PRESENT(pml4entry)) {
        panic("vm_unmap_address: PML4 entry not present");
    }

    pdptable = (vm_dir *)(get_pdpp(pml4entry, VM_PAGE_SIZE_DIR));
    pdptentry = GET_ENTRY(pdptable, indices.PDP_index);
    if (!IS_PRESENT(pdptentry)) {
        panic("vm_unmap_address: PDPT entry not present");
    } else if (pdptentry->huge.PS) {
        panic("vm_unmap_address: Attempted to unmap 1GiB page, which is not supported");
    }

    pdtable = (vm_dir *)(get_pdpp(pdptentry, VM_PAGE_SIZE_DIR));
    pdentry = GET_ENTRY(pdtable, indices.PD_index);
    if (!IS_PRESENT(pdentry)) {
        panic("vm_unmap_address: PD entry not present");
    } else if (pdentry->big.PS) {
        panic("vm_unmap_address: Attempted to unmap 2MiB page, which is not supported");
    }

    pttable = (vm_dir *)(get_pdpp(pdentry, VM_PAGE_SIZE_DIR));
    ptentry = GET_ENTRY(pttable, indices.PT_index);
    if (!IS_PRESENT(ptentry)) {
        panic("vm_unmap_address: PT entry not present");
    }

    ptentry->directory.P = 0;
    vm_flush_tlb_entry(virtual_address);

    return SUCCESS;
}

status_t vm_mprotect_address(vm_dir * root, uint64_t virtual_address, uint8_t new_flags) {
    
    struct page_map_index indices;
    address_to_map(virtual_address, &indices);

    vm_dir *pdptable, *pdtable, *pttable;
    vm_entry *pml4entry, *pdptentry, *pdentry, *ptentry;

    pml4entry = GET_ENTRY(root, indices.PML4_index);
    if (!IS_PRESENT(pml4entry)) {
        panic("vm_mprotect_address: PML4 entry not present");
    }

    pdptable = (vm_dir *)(get_pdpp(pml4entry, VM_PAGE_SIZE_DIR));
    pdptentry = GET_ENTRY(pdptable, indices.PDP_index);
    if (!IS_PRESENT(pdptentry)) {
        panic("vm_mprotect_address: PDPT entry not present");
    } else if (pdptentry->huge.PS) {
        panic("vm_mprotect_address: Attempted to mprotect 1GiB page, which is not supported");
    }

    pdtable = (vm_dir *)(get_pdpp(pdptentry, VM_PAGE_SIZE_DIR));
    pdentry = GET_ENTRY(pdtable, indices.PD_index);
    if (!IS_PRESENT(pdentry)) {
        panic("vm_mprotect_address: PD entry not present");
    } else if (pdentry->big.PS) {
        panic("vm_mprotect_address: Attempted to mprotect 2MiB page, which is not supported");
    }

    pttable = (vm_dir *)(get_pdpp(pdentry, VM_PAGE_SIZE_DIR));
    ptentry = GET_ENTRY(pttable, indices.PT_index);
    if (!IS_PRESENT(ptentry)) {
        panic("vm_mprotect_address: PT entry not present"); 
    }

    vm_perms page_perms;
    page_perms.read_write = VM_WRITE_BIT_SET(new_flags) ? 1 : 0;
    page_perms.user = VM_USER_BIT_SET(new_flags) ? 1 : 0;
    page_perms.write_through = VM_WRITE_THROUGH_BIT_SET(new_flags) ? 1 : 0;
    page_perms.cache_disable = VM_CACHE_DISABLE_BIT_SET(new_flags) ? 1 : 0;
    page_perms.global = VM_GLOBAL_BIT_SET(new_flags) ? 1 : 0;
    page_perms.no_execute = VM_NX_BIT_SET(new_flags) ? 1 : 0;

    ptentry->directory.RW = page_perms.read_write;
    ptentry->directory.US = page_perms.user;
    ptentry->directory.PWT = page_perms.write_through;
    ptentry->directory.PCD = page_perms.cache_disable;
    ptentry->directory.XD = page_perms.no_execute;

    vm_flush_tlb_entry(virtual_address);

    return SUCCESS;
}

status_t vm_deallocate_vspace(vm_dir * root) {
    (void)root;
    //panic("vm_deallocate_vspace: Not yet implemented");
    return SUCCESS;
    //Remove all mappings and free all page tables
}