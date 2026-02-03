#include <krnl/globals.h>
#include <krnl/mem/pmm.h>
#include <krnl/mem/vmm.h>
#include <krnl/mem/allocator.h>
#include <krnl/debug/debug.h>
#include <krnl/libraries/std/string.h>
#include <krnl/libraries/assert/assert.h>

#define STACKTRACE_SIZE 3
#define DEALLOCATION_BUFFER_SIZE 0x1000

struct deallocation {
    void* allocation_stacktrace[STACKTRACE_SIZE];
    void* deallocation_stacktrace[STACKTRACE_SIZE];
    uint64_t address;
};

struct allocation {
    vmm_root_t * root;
    void * physical_address;
    void * virtual_address;
    uint64_t size;
    uint8_t permisions;

    void* allocation_stacktrace[STACKTRACE_SIZE];

    struct allocation * next;
    struct allocation * prev;
};

struct alloc_buffer {
    void * buffer_base_address;
    uint64_t buffer_free_allocations;
};

struct allocation * allocations_head = NULL;
struct deallocation deallocations[DEALLOCATION_BUFFER_SIZE];
uint64_t deallocation_count = 0;
struct alloc_buffer current_alloc_buffer = {0};

void fill_stacktrace(void ** st) {
    st[0] = __builtin_return_address(0);
    st[1] = __builtin_return_address(1);
    st[2] = __builtin_return_address(2);
}

//A reallocation may have been performed, if the deallocation exists for the given pointer, remove it from the deallocation list
void remove_deallocation_if_realloc(void * ptr) {
    for (uint64_t i = 0; i < deallocation_count; i++) {
        struct deallocation * dealloc = &deallocations[i];
        if (dealloc->address == (uint64_t)ptr) {
            //Remove this deallocation by shifting all later deallocations back
            for (uint64_t j = i; j < deallocation_count - 1; j++) {
                deallocations[j] = deallocations[j + 1];
            }
            deallocation_count--;
            return;
        }
    }
}

void add_allocation(vmm_root_t* root, void * physical_address, void * virtual_address, uint64_t size, uint8_t permisions) {
    if (current_alloc_buffer.buffer_base_address == NULL || current_alloc_buffer.buffer_free_allocations == 0) {
        //Initialize the allocation buffer
        current_alloc_buffer.buffer_base_address = (void*)vmm_to_identity_map((uint64_t)pmm_alloc_pages(1)); //Allocate 1 page for the allocation buffer
        current_alloc_buffer.buffer_free_allocations = PMM_PAGE_SIZE / sizeof(struct allocation);
    }

    struct allocation * new_allocation = (struct allocation *)current_alloc_buffer.buffer_base_address + ( (PMM_PAGE_SIZE / sizeof(struct allocation)) - current_alloc_buffer.buffer_free_allocations);
    new_allocation->root = root;
    new_allocation->physical_address = physical_address;
    new_allocation->virtual_address = virtual_address;
    new_allocation->size = size;
    new_allocation->permisions = permisions;
    new_allocation->next = allocations_head;
    new_allocation->prev = NULL;

    fill_stacktrace(new_allocation->allocation_stacktrace);

    if (allocations_head != NULL) {
        allocations_head->prev = new_allocation;
    }

    allocations_head = new_allocation;
    current_alloc_buffer.buffer_free_allocations--;

    remove_deallocation_if_realloc(virtual_address);
}

//If the deallocation count exceeds DEALLOCATION_BUFFER_SIZE start writing over old deallocations
void add_deallocation(void * address, void ** out_stacktrace) {
    if (deallocation_count >= DEALLOCATION_BUFFER_SIZE) {
        deallocation_count = 0;
    }

    struct deallocation * dealloc = &deallocations[deallocation_count++];
    dealloc->address = (uint64_t)address;

    fill_stacktrace(dealloc->deallocation_stacktrace);

    dealloc->allocation_stacktrace[0] = out_stacktrace[0];
    dealloc->allocation_stacktrace[1] = out_stacktrace[1];
    dealloc->allocation_stacktrace[2] = out_stacktrace[2];
}

void dump_deallocations() {
    kprintf("Deallocation dump (most recent first):\n");
    for (int64_t i = deallocation_count - 1; i >= 0; i--) {
        struct deallocation * dealloc = &deallocations[i];
        if (dealloc->address == 0) {
            continue;
        }
        kprintf("Deallocated address: %p\n", (void *)dealloc->address);
        kprintf(" Allocation stacktrace:\n");
        for (int j = 0; j < STACKTRACE_SIZE; j++) {
            if (dealloc->allocation_stacktrace[j] == NULL) {
                break;
            }
            kprintf("  [%d] %p\n", j, dealloc->allocation_stacktrace[j]);
        }
        kprintf(" Deallocation stacktrace:\n");
        for (int j = 0; j < STACKTRACE_SIZE; j++) {
            if (dealloc->deallocation_stacktrace[j] == NULL) {
                break;
            }
            kprintf("  [%d] %p\n", j, dealloc->deallocation_stacktrace[j]);
        }
    }
}

//Search specific deallocation and dump it
void dump_deallocation(void * adddr) {
    kprintf("Deallocation dump for address %p:\n", adddr);
    for (uint64_t i = 0; i < deallocation_count; i++) {
        struct deallocation * dealloc = &deallocations[i];
        if (dealloc->address == (uint64_t)adddr) {
            kprintf("Deallocated address: %p\n", (void *)dealloc->address);
            kprintf(" Allocation stacktrace:\n");
            for (int j = 0; j < STACKTRACE_SIZE; j++) {
                if (dealloc->allocation_stacktrace[j] == NULL) {
                    break;
                }
            }
            kprintf(" Deallocation stacktrace:\n");
            for (int j = 0; j < STACKTRACE_SIZE; j++) {
                if (dealloc->deallocation_stacktrace[j] == NULL) {
                    break;
                }
            }
        }
    }
}

void allocmatch() {
    kprintf("Allocation matches !!!\n");
}

//Detect double free attempts and panic
void detect_double_lok(void * ptr) {
    struct deallocation * current = deallocations;
    for (uint64_t i = 0; i < DEALLOCATION_BUFFER_SIZE; i++) {
        if (current->address == (uint64_t)ptr) {
            dump_deallocations();
            panic("Double free detected for pointer %p\n", ptr);
        }
        current++;
    }
}

void remove_allocation(vmm_root_t * root, void * ptr) {
    struct allocation * current = allocations_head;

    while (current != NULL) {
        if (current->virtual_address == ptr && (current->root == root || root == NULL)) { //Null means any root

            if (current->prev != NULL) {
                current->prev->next = current->next;
            } else {
                allocations_head = current->next; // Update head if needed
            }

            if (current->next != NULL) {
                current->next->prev = current->prev;
            }

            // Found the allocation to remove
            add_deallocation(ptr, current->allocation_stacktrace);
            
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
        //kprintf("Checking allocation at paddr %p (root %p) against: %p/r:%p\n", current->physical_address, current->root, physical_address, root);
        //Check if another process has the same address mapped
        if (current->physical_address == physical_address && current->root != root) {
            //kprintf("It's a match !\n");
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

    add_allocation(vmm_get_root(), phys_addr, virt_addr, size, 0x3); //RW permisions
    //kprintf("kmalloc: Allocated %d bytes at virtual address %p (physical %p)\n", size, virt_addr, phys_addr);
    return virt_addr;
}

//VERY IMPORTANT: WE ASUME THAT STACKS GROW DOWNWARDS, ALSO KERNEL STACKS CANNOT GROW
stack_t * kstackalloc(vmm_root_t * root, uint64_t size) {
    void * phys_addr = pmm_alloc_pages((size + PMM_PAGE_SIZE - 1) / PMM_PAGE_SIZE);
    if (phys_addr == 0x0) {
        panic("kstackalloc: Failed to allocate physical memory");
    }

    void * virt_addr = (void*)((uint64_t)VMM_REGION_K_STACK + (uint64_t)phys_addr); //Map to identity region

    uint64_t top_address = (uint64_t)(virt_addr + size);
    if (top_address % 0x10) {
        top_address -= top_address % 0x10;
    }
    top_address -= 0x8;

    uint64_t base_address = (uint64_t)virt_addr & ~0xFFF; //Align to page size
    memset((void *)virt_addr, 0, (size + PMM_PAGE_SIZE - 1) / PMM_PAGE_SIZE * PMM_PAGE_SIZE);
    add_allocation(root, phys_addr, (void*)base_address, size, 0x3); //RW permisions
    stack_t *stk = kmalloc(sizeof(stack_t));
    stk->top = (void *)(top_address);
    stk->base = (void *)(base_address);
    stk->flags = VMM_WRITE_BIT;
    return stk;
}

void kfree(void * virtual_address) {
    //kprintf("kfree: Freeing pointer %p\n", virtual_address);
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

    detect_double_lok(virtual_address);
    dump_deallocations();
    panic("kfree: Allocation not found for pointer %p\n", virtual_address);
}

void kstackfree(stack_t * stk) {
    //Find the allocation
    struct allocation * current = allocations_head;

    while (current != NULL) {
        if (current->virtual_address == stk->base) {
            //Always free the pages
            pmm_free_pages(current->physical_address, (current->size + PMM_PAGE_SIZE - 1) / PMM_PAGE_SIZE);
            //remove the allocation from the list
            remove_allocation(NULL, stk->base);
            kfree(stk);
            return;
        }
        current = current->next;
    }
    panic("kstackfree: Allocation not found for pointer %p\n", stk->base);
}

void * malloc(vmm_root_t * root, uint64_t size, uint64_t vaddr, uint8_t flags) {
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
    
    void * identity = (void *)vmm_to_identity_map((uint64_t)phys_addr);
    //kprintf("malloc: vaddr: %llx paddr: %llx size: %llx\n", vaddr, (uint64_t)phys_addr, size);

    memset(identity, 0, (size + PMM_PAGE_SIZE - 1) / PMM_PAGE_SIZE * PMM_PAGE_SIZE);

    add_allocation(root, phys_addr,(void*)vaddr, size, flags);
    return (void*)vaddr;
}

stack_t * stackalloc(vmm_root_t * root, uint64_t size, uint64_t vaddr, uint8_t flags) {
    
    uint64_t pages = (size + PMM_PAGE_SIZE - 1) / PMM_PAGE_SIZE;
    void * phys_addr = pmm_alloc_pages(pages);
    if (phys_addr == NULL) {
        panic("stackalloc: Failed to allocate physical memory");
    }

    status_t st = vmm_map_pages(
        root,
        vaddr,
        (uint64_t)phys_addr,
        pages,
        PMM_PAGE_SIZE,
        flags
    );
    if (st != SUCCESS) {
        panic("stackalloc: Failed to map user pages");
    }

    stack_t * stk = kmalloc(sizeof(stack_t));
    if (!stk) {
        panic("stackalloc: Failed to allocate stack structure");
    }
    stk->base = (void *)vaddr;
    uint64_t top_address = vaddr + size;
    if (top_address % 0x10) {
        top_address -= top_address % 0x10;
    }
    top_address -= 0x8;

    if (st != SUCCESS) {
        panic("stackalloc: Failed to map user pages");
    }

    void * identity = (void *)vmm_to_identity_map((uint64_t)phys_addr);
    memset(identity, 0, pages * PMM_PAGE_SIZE);
    stk->top = (void *)top_address;
    stk->flags = flags;
    add_allocation(root, phys_addr, (void*)vaddr, size, flags);
    return stk;
}

void free(vmm_root_t * cr3, void * virtual_address) {
    //kprintf("Called free on vaddr %p in root %p\n", virtual_address, cr3);
    //Find the allocation
    struct allocation * current = allocations_head;
    while (current != NULL) {
        if (current->virtual_address == virtual_address && current->root == cr3) {
            //Unmap the pages from VMM
            vmm_unmap_pages(
                cr3,
                (uint64_t)virtual_address,
                (current->size + PMM_PAGE_SIZE - 1) / PMM_PAGE_SIZE,
                PMM_PAGE_SIZE
            );
            //Free the physical pages
            if (should_deallocate_pmm(cr3, current->physical_address)) {
                //kprintf("Releasing physical pages at %p\n", current->physical_address);
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

void stackfree(vmm_root_t * cr3, stack_t * stack) {
    //Find the allocation
    struct allocation * current = allocations_head;
    while (current != NULL) {
        if (current->virtual_address == stack->base && current->root == cr3) {
            //Unmap the stack pages from VMM
            vmm_unmap_pages(
                cr3,
                (uint64_t)stack->base,
                (current->size + PMM_PAGE_SIZE - 1) / PMM_PAGE_SIZE,
                PMM_PAGE_SIZE
            );
            //Free the physical pages
            if (should_deallocate_pmm(cr3, current->physical_address)) {
                pmm_free_pages(current->physical_address, (current->size + PMM_PAGE_SIZE - 1) / PMM_PAGE_SIZE); 
            }
            //remove the allocation from the list
            remove_allocation(cr3, stack->base);
            return;
        }
        current = current->next;
    }
    panic("stackfree: Allocation not found for pointer %p\n", stack->base);
}

stack_t * copy_kstack(vmm_root_t * dest_root, stack_t * source) {
    uint64_t stack_size = ((uint64_t)source->top - (uint64_t)source->base);
    uint64_t pages = (stack_size + PMM_PAGE_SIZE - 1) / PMM_PAGE_SIZE;
    void * phys_addr = pmm_alloc_pages(pages);
    if (phys_addr == NULL) {
        panic("copy_stack: Failed to allocate physical memory");
    }
    memcpy((void*)vmm_to_identity_map((uint64_t)phys_addr), source->base, pages * PMM_PAGE_SIZE);
    //Unmap the old stack if it exists and map it again to the new physical address
    status_t st = vmm_unmap_pages(
        dest_root,
        (uint64_t)source->base,
        pages,
        PMM_PAGE_SIZE
    );
    if (st != SUCCESS) {
        panic("copy_stack: Failed to unmap old stack pages");
    }

    st = vmm_map_pages(
        dest_root,
        (uint64_t)source->base,
        (uint64_t)phys_addr,
        pages,
        PMM_PAGE_SIZE,
        source->flags
    );
    if (st != SUCCESS) {
        panic("copy_stack: Failed to map new stack pages");
    }

    //add allocation
    add_allocation(dest_root, phys_addr, (void*)source->base, stack_size, source->flags);
    return source;
}

stack_t * copy_stack(vmm_root_t * dest_root, stack_t * source) {
    uint64_t stack_size = ((uint64_t)source->top - (uint64_t)source->base);
    uint64_t pages = (stack_size + PMM_PAGE_SIZE - 1) / PMM_PAGE_SIZE;
    void * phys_addr = pmm_alloc_pages(pages);
    if (phys_addr == NULL) {
        panic("copy_stack: Failed to allocate physical memory");
    }
    memcpy((void*)vmm_to_identity_map((uint64_t)phys_addr), source->base, pages * PMM_PAGE_SIZE);
    //Unmap the old stack if it exists and map it again to the new physical address
    status_t st = vmm_unmap_pages(
        dest_root,
        (uint64_t)source->base,
        pages,
        PMM_PAGE_SIZE
    );
    if (st != SUCCESS) {
        panic("copy_stack: Failed to unmap old stack pages");
    }

    st = vmm_map_pages(
        dest_root,
        (uint64_t)source->base,
        (uint64_t)phys_addr,
        pages,
        PMM_PAGE_SIZE,
        source->flags
    );
    if (st != SUCCESS) {
        panic("copy_stack: Failed to map new stack pages");
    }

    //add allocation
    add_allocation(dest_root, phys_addr, (void*)source->base, stack_size, source->flags);
    return source;
}

void * to_kident(vmm_root_t * cr3, void * vaddr) {
    //Get physical address of vaddr in cr3
    uint64_t physical;
    status_t st = vmm_get_physical_address(
        cr3,
        (uint64_t)vaddr,
        &physical
    );
    if (st != SUCCESS) {
        panic("to_kident: Failed to get physical address");
    }
    return (void *)vmm_to_identity_map(physical);
}