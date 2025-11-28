#include <krnl/mem/pmm.h>
#include <krnl/mem/vmm.h>
#include <krnl/mem/allocator.h>
#include <krnl/debug/debug.h>

struct allocation {
    void * phyisical_address;
    void * virtual_address;
    //uint64_t page_size; //All pages allocated here are 0x1000 bytes
    uint64_t size;
    uint8_t permisions;
    uint64_t guard_base; //Only used for stacks that grow

    struct allocation * next;
    struct allocation * prev;
};

struct alloc_buffer {
    void * buffer_base_address;
    uint64_t buffer_free_allocations;
};

struct allocation * allocations_head = NULL;
struct alloc_buffer current_alloc_buffer = {0};

void add_allocation(void * phyisical_address, void * virtual_address, uint64_t size, uint8_t permisions, uint64_t guard_base) {
    if (current_alloc_buffer.buffer_base_address == NULL || current_alloc_buffer.buffer_free_allocations == 0) {
        //Initialize the allocation buffer
        current_alloc_buffer.buffer_base_address = vmm_to_identity_map(pmm_alloc_pages(1)); //Allocate 1 page for the allocation buffer
        current_alloc_buffer.buffer_free_allocations = PMM_PAGE_SIZE / sizeof(struct allocation);
    }

    struct allocation * new_allocation = (struct allocation *)current_alloc_buffer.buffer_base_address + ( (PMM_PAGE_SIZE / sizeof(struct allocation)) - current_alloc_buffer.buffer_free_allocations);
    new_allocation->phyisical_address = phyisical_address;
    new_allocation->virtual_address = virtual_address;
    new_allocation->size = size;
    new_allocation->permisions = permisions;
    new_allocation->guard_base = guard_base;
    new_allocation->next = allocations_head;
    new_allocation->prev = NULL;

    if (allocations_head != NULL) {
        allocations_head->prev = new_allocation;
    }

    allocations_head = new_allocation;
    current_alloc_buffer.buffer_free_allocations--;
}

void remove_allocation(void * ptr) {
    struct allocation * current = allocations_head;

    while (current != NULL) {
        if (current->virtual_address == ptr) {
            // Found the allocation to remove
            if (current->prev != NULL) {
                current->prev->next = current->next;
            } else {
                allocations_head = current->next; // Update head if needed
            }

            if (current->next != NULL) {
                current->next->prev = current->prev;
            }

            // Note: We do not free the allocation structure itself for simplicity
            return;
        }
        current = current->next;
    }

    panic("Allocation not found for pointer %p\n", ptr);
}

void * __kmalloc(uint64_t size, uint64_t region) {
    void * phys_addr = pmm_alloc_pages((size + PMM_PAGE_SIZE - 1) / PMM_PAGE_SIZE);
    if (phys_addr == NULL) {
        panic("kmalloc: Failed to allocate physical memory");
    }
    void * virt_addr = region + (uint64_t)phys_addr; //Map to specific region
    add_allocation(phys_addr, virt_addr, size, 0x3, 0x0); //RW permisions, no guard
    return virt_addr;
}

void kfree(void *ptr) {
    //Find the allocation
    struct allocation * current = allocations_head;
    while (current != NULL) {
        if (current->virtual_address == ptr) {
            //Free the physical pages
            pmm_free_pages(current->phyisical_address, (current->size + PMM_PAGE_SIZE - 1) / PMM_PAGE_SIZE);
            //Remove the allocation from the list
            remove_allocation(ptr);
            return;
        }
        current = current->next;
    }
    panic("Allocation not found for pointer %p\n", ptr);
}

//VERY IMPORTANT: WE ASUME THAT STACKS GROW DOWNWARDS, THIS MEANS THAT THE GUARD PAGE IS AT THE LOW ADDRESSES END OF THE STACK
struct stack * __kstackalloc(vmm_root* root, uint64_t initial_size, uint64_t region, uint8_t flags) {
    uint64_t pages = (initial_size + PMM_PAGE_SIZE - 1) / PMM_PAGE_SIZE;
    uint64_t phys_addr = (uint64_t)pmm_alloc_pages(pages + 1); //Allocate an extra page for the guard
    //When the guard page is accessed, a page fault will occur, we will handle it by growing the stack
    if (phys_addr == 0x0) {
        panic("kstackalloc: Failed to allocate physical memory");
    }

    uint64_t virt_addr = region + phys_addr; //Map stacks to a specific region
    status_t st = vmm_map_pages(
        root,
        virt_addr + PMM_PAGE_SIZE,
        phys_addr + PMM_PAGE_SIZE, //Skip the guard page
        pages,
        PMM_PAGE_SIZE,
        flags
    );

    if (st != SUCCESS) {
        panic("kstackalloc: Failed to map stack pages");
    }

    add_allocation(phys_addr + PMM_PAGE_SIZE, virt_addr + PMM_PAGE_SIZE, initial_size, flags, virt_addr); //Guard page at the base of the stack

    struct stack *stk = kmalloc(sizeof(struct stack));
    stk->top = (void *)(virt_addr + PMM_PAGE_SIZE + pages * PMM_PAGE_SIZE);
    stk->base = (void *)(virt_addr + PMM_PAGE_SIZE);
    return stk;
}

void kstackfree(struct stack *ptr) {
    //Find the allocation
    struct allocation * current = allocations_head;
    while (current != NULL) {
        if (current->virtual_address + current->size == ptr->base) {
            //Free the physical pages including the guard page
            uint64_t total_pages = ((current->size + PMM_PAGE_SIZE - 1) / PMM_PAGE_SIZE) + 1;
            pmm_free_pages(current->phyisical_address - PMM_PAGE_SIZE, total_pages);
            //Unmap the pages
            status_t st = vmm_unmap_pages(
                vmm_get_root(),
                current->virtual_address - PMM_PAGE_SIZE,
                total_pages,
                PMM_PAGE_SIZE
            );

            if (st != SUCCESS) {
                panic("kstackfree: Failed to unmap stack pages");
            }

            //Remove the allocation from the list
            remove_allocation(current->virtual_address);
            return;
        }
        current = current->next;
    }
    panic("Stack allocation not found for pointer %p\n", ptr);
}

void * kmalloc(uint64_t size) {
    return __kmalloc(size, VMM_REGION_K_IDENT);
}

status_t kmalloc_farlands(vmm_root * root, uint64_t size, uint64_t vaddr, uint8_t flags, farlands_t * farlands) {
    void * phys_addr = pmm_alloc_pages((size + PMM_PAGE_SIZE - 1) / PMM_PAGE_SIZE);
    if (phys_addr == NULL) {
        panic("kmalloc: Failed to allocate physical memory");
    }

    status_t st = vmm_map_pages(
        root,
        vaddr,
        (uint64_t)phys_addr,
        (size + PMM_PAGE_SIZE - 1) / PMM_PAGE_SIZE,
        PMM_PAGE_SIZE,
        flags
    );

    if (st != SUCCESS) {
        panic("kmalloc_user_at: Failed to map user pages");
    }

    st = vmm_map_pages(
        vmm_get_root(),
        vaddr + VMM_REGION_FARLANDS,
        (uint64_t)phys_addr,
        (size + PMM_PAGE_SIZE - 1) / PMM_PAGE_SIZE,
        PMM_PAGE_SIZE,
        VMM_WRITE_BIT
    );

    if (st != SUCCESS) {
        panic("kmalloc_user_at: Failed to map farlands pages");
    }

    farlands->farland_address = (void *)(vaddr);
    farlands->access_address = (void *)(vaddr + VMM_REGION_FARLANDS);

    return SUCCESS;
}

struct stack * kstackalloc(uint64_t initial_size) {
    return __kstackalloc(vmm_get_root(), initial_size, VMM_REGION_K_STACK, VMM_WRITE_BIT);
}

struct stack * kstackalloc_user(vmm_root * root, uint64_t initial_size) {
    return __kstackalloc(root, initial_size, VMM_REGION_U_SPACE_INI, VMM_WRITE_BIT | VMM_USER_BIT);
}