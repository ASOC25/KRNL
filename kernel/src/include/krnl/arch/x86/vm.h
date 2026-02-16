#ifndef _X86_VM_H
#define _X86_VM_H

#define MAXPHYADDR 40
#define VM_PAGE_SIZE 4096

#include <krnl/libraries/std/stdint.h>
#include <krnl/libraries/std/stddef.h>

#define VM_PAGE_SIZE_4KB                   0x1000
#define VM_PAGE_SIZE_2MB                   0x200000
#define VM_PAGE_SIZE_1GB                   0x40000000
#define VM_PAGE_SIZE_DIR                   0x1000

#define VM_WRITE_BIT                       0x1
#define VM_USER_BIT                        0x2
#define VM_WRITE_THROUGH_BIT               0x4 
#define VM_CACHE_DISABLE_BIT               0x8
#define VM_GLOBAL_BIT                      0x10
#define VM_NX_BIT                          0x20

//WARNING: WE ARE FIXING MAXPHYADDR = 40

typedef struct page_permissions {
    uint8_t read_write : 1;
    uint8_t user : 1;
    uint8_t write_through : 1;
    uint8_t cache_disable : 1;
    uint8_t global : 1;
    uint8_t no_execute : 1;
} vm_perms;

struct directory_entry {
    uint64_t P                  :1; //0
    uint64_t RW                 :1; //1
    uint64_t US                 :1; //2
    uint64_t PWT                :1; //3
    uint64_t PCD                :1; //4
    uint64_t A                  :1; //5
    uint64_t IGNORED1           :1; //6 
    uint64_t PS                 :1; //7 reserved
    uint64_t IGNORED2           :3; //8-10
    uint64_t R                  :1; //11 ignored
    uint64_t PDPP               :28; //12-39
    uint64_t RESERVED           :12; //40-51
    uint64_t IGNORED3           :11; //52-62
    uint64_t XD                 :1; //63
} __attribute__((packed));

struct huge_entry {
    uint64_t P                  :1; //0
    uint64_t RW                 :1; //1
    uint64_t US                 :1; //2
    uint64_t PWT                :1; //3
    uint64_t PCD                :1; //4
    uint64_t A                  :1; //5

    uint64_t D                  :1; //6
    uint64_t PS                 :1; //7
    uint64_t G                  :1; //8
    uint64_t IGNORED1           :2; //9-10
    uint64_t R                  :1; //11
    uint64_t PAT                :1; //12
    uint64_t RESERVED           :17;//13-29
    uint64_t PDPP               :10;//30-39

    uint64_t RESERVED2          :12;//40-51
    uint64_t IGNORED2           :7; //52-58
    uint64_t PK                 :4; //59-62
    uint64_t XD                 :1; //63
} __attribute__((packed));

struct big_entry {
    uint64_t P                  :1; //0
    uint64_t RW                 :1; //1
    uint64_t US                 :1; //2
    uint64_t PWT                :1; //3
    uint64_t PCD                :1; //4
    uint64_t A                  :1; //5
    uint64_t D                  :1; //6
    uint64_t PS                 :1; //7
    uint64_t G                  :1; //8
    uint64_t IGNORED1           :2; //9-10
    uint64_t R                  :1; //11
    uint64_t PAT                :1; //12
    uint64_t RESERVED           :8; //13-20
    uint64_t PDPP               :19;//21-39
    uint64_t RESERVED2          :12;//40-51
    uint64_t IGNORED2           :7; //52-58
    uint64_t PK                 :4; //59-62
    uint64_t XD                 :1; //63
} __attribute__((packed));

struct regular_entry {
    uint64_t P                  :1; //0
    uint64_t RW                 :1; //1
    uint64_t US                 :1; //2
    uint64_t PWT                :1; //3
    uint64_t PCD                :1; //4
    uint64_t A                  :1; //5
    uint64_t D                  :1; //6
    uint64_t PAT                :1; //7
    uint64_t G                  :1; //8
    uint64_t IGNORED1           :2; //9-10
    uint64_t R                  :1; //11
    uint64_t PDPP               :28;//12-39
    uint64_t RESERVED           :12;//40-51
    uint64_t IGNORED2           :7; //52-58
    uint64_t PK                 :4; //59-62
    uint64_t XD                 :1; //63
} __attribute__((packed));

typedef union {
    struct directory_entry directory;
    struct huge_entry huge;
    struct big_entry big;
    struct regular_entry regular;
} __attribute__((packed)) vm_entry;
struct page_directory {
    uint64_t entries[512];
} __attribute__((aligned(VM_PAGE_SIZE)));

typedef struct page_directory vm_dir;

struct page_map_index{
    uint64_t PML4_index;
    uint64_t PDP_index;
    uint64_t PD_index;
    uint64_t PT_index;
};

struct page_info {
    struct page_map_index indices;
    vm_perms permissions;
    uint64_t page_size;
    uint64_t physical_address;
};

typedef enum vm_copy_range {
    VM_COPY_ALL,
    VM_COPY_KERNEL_ONLY
} vm_copy_range;

void vm_set_offset(uint64_t offset);
uint64_t vm_to_identity_map(uint64_t address);
uint64_t vm_from_identity_map(uint64_t address);

vm_dir * vm_get_current_pml4();
vm_dir * vm_duplicate_pml4(vm_dir * pml4, vm_copy_range range);
void vm_set_current_pml4(vm_dir * pml4);
void vm_flush_tlb_entry(uint64_t address);
status_t vm_deallocate_vspace(vm_dir * root);
uint8_t vm_check_and_clean_dirty(vm_dir * root, uint64_t virtual_address);

status_t vm_map_address(vm_dir * root, uint64_t virtual_address, uint64_t physical_address, uint64_t page_size, uint8_t flags);
status_t vm_unmap_address(vm_dir * root, uint64_t virtual_address);
status_t vm_mprotect_address(vm_dir * root, uint64_t virtual_address, uint8_t new_flags);

status_t vm_get_physical_address(vm_dir* root, uint64_t virtual_address, uint64_t* physical_address);
status_t vm_get_page_info(vm_dir * root, uint64_t virtual_address, struct page_info* info);
#endif