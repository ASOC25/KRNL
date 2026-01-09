#ifndef _VMM_H
#define _VMM_H

#include <krnl/libraries/std/stdint.h>
#include <krnl/libraries/std/stddef.h>

#define VMM_PAGE_SIZE 4096
#define VMM_PAGE_SIZE_4KB                   0x1000
#define VMM_PAGE_SIZE_2MB                   0x200000
#define VMM_PAGE_SIZE_1GB                   0x40000000
#define VMM_PAGE_SIZE_DIR                   0x1000

#define VMM_WRITE_BIT                       0x1
#define VMM_USER_BIT                        0x2
#define VMM_WRITE_THROUGH_BIT               0x4 
#define VMM_CACHE_DISABLE_BIT               0x8
#define VMM_GLOBAL_BIT                      0x10
#define VMM_NX_BIT                          0x20

#define VMM_PHYSICAL_MEMORY_SIZE            0x000000F000000000

#define VMM_REGION_U_SPACE_INI              0x0000000000000000
#define VMM_REGION_U_SPACE_MMAP             0x0000600000000000
#define VMM_REGION_U_STACK                  0x00007FFFFFFFF000
#define VMM_REGION_S_STACK                  0x00006FFFFFFFF000
#define VMM_REGION_U_SPACE_END              0x0000800000000000
#define VMM_REGION_K_STACK                  0xFFFFA00000000000
#define VMM_REGION_K_IDENT                  0xFFFFB00000000000
#define VMM_REGION_K_DEVICES                0xFFFFC00000000000
#define VDSO_BASE_ADDRESS                   0xFFFFD00000000000

typedef struct vmm_root_t {
    uint64_t entries[512];
} __attribute__((aligned(VMM_PAGE_SIZE))) vmm_root_t;

typedef enum vmm_copy_range {
    VMM_COPY_ALL,
    VMM_COPY_KERNEL_ONLY
} vmm_copy_range;

typedef struct vmm_perms {
    uint8_t read_write : 1;
    uint8_t user : 1;
    uint8_t write_through : 1;
    uint8_t cache_disable : 1;
    uint8_t global : 1;
    uint8_t no_execute : 1;
} vmm_perms;

typedef struct vmm_info {
    vmm_perms permissions;
    uint64_t page_size;
} vmm_info;

void vmm_remap(uint64_t offset);

status_t vmm_map_pages(vmm_root_t * root, uint64_t virtual_address_start, uint64_t physical_address_start, uint64_t pages, uint64_t page_size, uint8_t flags);
status_t vmm_unmap_pages(vmm_root_t * root, uint64_t virtual_address_start, uint64_t pages, uint64_t page_size);
status_t vmm_mprotect_pages(vmm_root_t * root, uint64_t virtual_address_start, uint64_t pages, uint64_t page_size, uint8_t new_flags);
status_t vmm_get_physical_address(vmm_root_t * root, uint64_t virtual_address, uint64_t * physical_address);
status_t vmm_get_page_info(vmm_root_t * root, uint64_t virtual_address, vmm_info * info);

uint64_t vmm_to_identity_map(uint64_t address);
uint64_t vmm_from_identity_map(uint64_t address);
uint64_t vmm_to_device_map(uint64_t address);
uint64_t vmm_from_device_map(uint64_t address);

uint8_t vmm_check_and_clean_dirty(vmm_root_t * root, uint64_t virtual_address, uint64_t pages, uint64_t page_size);

void vmm_set_root(vmm_root_t * root);
void vmm_free_root(vmm_root_t * root);
vmm_root_t * vmm_get_root(void);

vmm_root_t * vmm_duplicate_kspace(void);
vmm_root_t * vmm_duplicate_fullspace(vmm_root_t * original);




#endif