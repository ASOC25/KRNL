#include <krnl/mem/pmm.h>
#include <krnl/mem/vmm.h>
#include <krnl/mem/allocator.h>
#include <krnl/debug/debug.h>
#include <krnl/libraries/std/string.h>

struct allocation {
    vmm_root_t * root;
    void * physical_address;
    void * virtual_address;
    vmm_root_t * access_root; //For farlands allocations
    void * access_address; //For farlands allocations
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

void add_allocation(vmm_root_t* root, void * physical_address, vmm_root_t * access_root, void * access_address, void * virtual_address, uint64_t size, uint8_t permisions, uint64_t guard_base) {
    if (current_alloc_buffer.buffer_base_address == NULL || current_alloc_buffer.buffer_free_allocations == 0) {
        //Initialize the allocation buffer
        current_alloc_buffer.buffer_base_address = (void*)vmm_to_identity_map((uint64_t)pmm_alloc_pages(1)); //Allocate 1 page for the allocation buffer
        current_alloc_buffer.buffer_free_allocations = PMM_PAGE_SIZE / sizeof(struct allocation);
    }

    struct allocation * new_allocation = (struct allocation *)current_alloc_buffer.buffer_base_address + ( (PMM_PAGE_SIZE / sizeof(struct allocation)) - current_alloc_buffer.buffer_free_allocations);
    new_allocation->root = root;
    new_allocation->physical_address = physical_address;
    new_allocation->access_root = access_root;
    new_allocation->access_address = access_address;
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

void remove_allocation(vmm_root_t * root, void * ptr) {
    struct allocation * current = allocations_head;

    while (current != NULL) {
        if (current->virtual_address == ptr && (current->root == root || root == NULL)) { //Null means any root
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

    panic("remove_allocation: Allocation not found for pointer %p\n", ptr);
}

uint8_t should_deallocate_pmm(vmm_root_t * root, void * physical_address) {
    struct allocation * current = allocations_head;
    while (current != NULL) {
        //Check if another process has the same address mapped
        if (current->physical_address == physical_address && current->root != root) {
            return 0; //Another process is using this physical address
        }
        current = current->next;
    }
    return 1;
}

void * kmalloc(uint64_t size) {
    void * phys_addr = pmm_alloc_pages((size + PMM_PAGE_SIZE - 1) / PMM_PAGE_SIZE);
    if (phys_addr == NULL) {
        panic("kmalloc: Failed to allocate physical memory");
    }
    void * virt_addr = (void*)((uint64_t)VMM_REGION_K_IDENT + (uint64_t)phys_addr); //Map to identity region
    memset(virt_addr, 0, (size + PMM_PAGE_SIZE - 1) / PMM_PAGE_SIZE * PMM_PAGE_SIZE);
    add_allocation(vmm_get_root(), phys_addr, NULL, 0x0, virt_addr, size, 0x3, 0x0); //RW permisions, no guard
    return virt_addr;
}

//VERY IMPORTANT: WE ASUME THAT STACKS GROW DOWNWARDS, THIS MEANS THAT THE GUARD PAGE IS AT THE LOW ADDRESSES END OF THE STACK
//Stackalloc cannot use identity mapping as we need to support guard pages
stack_t * kstackalloc(uint64_t initial_size) {
    uint64_t pages = (initial_size + PMM_PAGE_SIZE - 1) / PMM_PAGE_SIZE;
    uint64_t phys_addr = (uint64_t)pmm_alloc_pages(pages + 1); //Allocate an extra page for the guard
    //When the guard page is accessed, a page fault will occur, we will handle it by growing the stack
    if (phys_addr == 0x0) {
        panic("kstackalloc: Failed to allocate physical memory");
    }

    uint64_t virt_addr = VMM_REGION_K_STACK + phys_addr; //Map stacks to a specific region
    status_t st = vmm_map_pages(
        vmm_get_root(),
        virt_addr + PMM_PAGE_SIZE,
        phys_addr + PMM_PAGE_SIZE, //Skip the guard page
        pages,
        PMM_PAGE_SIZE,
        VMM_WRITE_BIT
    );

    if (st != SUCCESS) {
        panic("kstackalloc: Failed to map stack pages");
    }
    memset((void *)(virt_addr + PMM_PAGE_SIZE), 0, pages * PMM_PAGE_SIZE);
    add_allocation(vmm_get_root(), (void*)(phys_addr + PMM_PAGE_SIZE), NULL, 0x0, (void*)(virt_addr + PMM_PAGE_SIZE), initial_size, VMM_WRITE_BIT, virt_addr); //Guard page at the base of the stack

    stack_t *stk = kmalloc(sizeof(stack_t));
    stk->top = (void *)(virt_addr + PMM_PAGE_SIZE + pages * PMM_PAGE_SIZE);
    stk->base = (void *)(virt_addr + PMM_PAGE_SIZE);
    stk->guard_size = PMM_PAGE_SIZE;
    stk->flags = VMM_WRITE_BIT;
    return stk;
}

void kfree(void * virtual_address) {
    //Find the allocation
    struct allocation * current = allocations_head;

    while (current != NULL) {
        if (current->virtual_address == virtual_address) {
            //Always free the pages
            pmm_free_pages(current->physical_address, (current->size + PMM_PAGE_SIZE - 1) / PMM_PAGE_SIZE);
            //remove the allocation from the list
            remove_allocation(NULL, virtual_address);
            return;
        }
        current = current->next;
    }
    panic("kfree: Allocation not found for pointer %p\n", virtual_address);
}

void kstackfree(stack_t * stk) {
    //Find the allocation
    struct allocation * current = allocations_head;
    vmm_root_t * cr3 = vmm_get_root();

    while (current != NULL) {
        if (current->virtual_address == stk->base && current->root == cr3) {
            //Free the physical pages
            pmm_free_pages(current->physical_address - current->guard_base, ((current->size + PMM_PAGE_SIZE - 1) / PMM_PAGE_SIZE) + 1); //Free guard page too
            
            //Unmap the pages
            status_t st = vmm_unmap_pages(
                cr3,
                (uint64_t)(stk->base - stk->guard_size),
                ((current->size + PMM_PAGE_SIZE - 1) / PMM_PAGE_SIZE) + 1,
                PMM_PAGE_SIZE
            );

            if (st != SUCCESS) {
                panic("kstackfree: Failed to unmap stack pages");
            }

            //remove the allocation from the list
            remove_allocation(NULL, stk->base);
            kfree(stk);
            return;
        }
        current = current->next;
    }
    panic("kstackfree: Allocation not found for stack at base pointer %p\n", stk->base);
}

status_t malloc(vmm_root_t * root, uint64_t size, uint64_t vaddr, uint8_t flags, farlands_t * farlands) {
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

    farlands->address = (void *)(vaddr);
    farlands->handle = (void *)(phys_addr + VMM_REGION_FARLANDS);
    farlands->size = size;
    farlands->flags = flags;
    memset(farlands->handle, 0, (size + PMM_PAGE_SIZE - 1) / PMM_PAGE_SIZE * PMM_PAGE_SIZE);

    add_allocation(root, phys_addr, vmm_get_root(), farlands->handle, farlands->address, size, flags, 0x0);

    return SUCCESS;
}

status_t stackalloc(vmm_root_t * root, uint64_t size, uint64_t vaddr, uint8_t flags, farlands_stack_t * farstack) {
    uint64_t pages = (size + PMM_PAGE_SIZE - 1) / PMM_PAGE_SIZE;
    uint64_t phys_addr = (uint64_t)pmm_alloc_pages(pages + 1); //Allocate an extra page for the guard
    //When the guard page is accessed, a page fault will occur, we will handle it by growing the stack
    if (phys_addr == 0x0) {
        panic("kstackalloc: Failed to allocate physical memory");
    }
    uint64_t vaddr_bottom = vaddr - (pages * PMM_PAGE_SIZE) - PMM_PAGE_SIZE; //Calculate the bottom of the stack including guard
    status_t st = vmm_map_pages(
        root,
        vaddr_bottom,
        phys_addr + PMM_PAGE_SIZE, //Skip the guard page
        pages,
        PMM_PAGE_SIZE,
        flags
    );

    if (st != SUCCESS) {
        panic("kstackalloc: Failed to map stack pages");
    }

    uint64_t handle_base = (VMM_REGION_FARLANDS + phys_addr + PMM_PAGE_SIZE);
    uint64_t handle_top = (VMM_REGION_FARLANDS + phys_addr + PMM_PAGE_SIZE + pages * PMM_PAGE_SIZE);

    farstack->top = (void *)(vaddr_bottom + PMM_PAGE_SIZE + pages * PMM_PAGE_SIZE);
    farstack->base = (void *)(vaddr_bottom + PMM_PAGE_SIZE);
    farstack->handle_top = (void *)(handle_top);
    farstack->handle_base = (void *)(handle_base);
    farstack->flags = flags;
    farstack->guard_size = PMM_PAGE_SIZE;
    memset(farstack->handle_base, 0, pages * PMM_PAGE_SIZE);

    add_allocation(root, (void *)(phys_addr + PMM_PAGE_SIZE), vmm_get_root(), (void *)(handle_base), (void *)(vaddr_bottom + PMM_PAGE_SIZE), size, flags, vaddr_bottom); //Guard page at the base of the stack
    return SUCCESS;
}

void free(vmm_root_t * cr3, void * virtual_address) {
    //Find the allocation
    struct allocation * current = allocations_head;
    while (current != NULL) {
        if (current->virtual_address == virtual_address && current->root == cr3) {
            //Free the physical pages
            if (should_deallocate_pmm(cr3, current->physical_address)) {
                pmm_free_pages(current->physical_address, (current->size + PMM_PAGE_SIZE - 1) / PMM_PAGE_SIZE);
            }

            //remove the allocation from the list
            remove_allocation(cr3, virtual_address);
            return;
        }
        current = current->next;
    }
    panic("free: Allocation not found for pointer %p\n", virtual_address);
}

void stackfree(vmm_root_t * cr3, farlands_stack_t * farstack) {
    //Find the allocation
    struct allocation * current = allocations_head;

    while (current != NULL) {
        if (current->virtual_address == farstack->base && current->root == cr3) {
            //Free the physical pages
            if (should_deallocate_pmm(cr3, current->physical_address)) {
                pmm_free_pages(current->physical_address - current->guard_base, ((current->size + PMM_PAGE_SIZE - 1) / PMM_PAGE_SIZE) + 1); //Free guard page too
            }

            //Unmap the pages
            status_t st = vmm_unmap_pages(
                cr3,
                (uint64_t)(farstack->base - farstack->guard_size),
                ((current->size + PMM_PAGE_SIZE - 1) / PMM_PAGE_SIZE) + 1,
                PMM_PAGE_SIZE
            );

            if (st != SUCCESS) {
                panic("stackfree: Failed to unmap stack pages");
            }

            //remove the allocation from the list
            remove_allocation(cr3, farstack->base);
            return;
        }
        current = current->next;
    }
    panic("stackfree: Allocation not found for stack at base pointer %p\n", farstack->base);
}