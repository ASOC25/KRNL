#include <krnl/mem/mmap.h>
#include <krnl/process/process.h>
#include <krnl/mem/allocator.h>
#include <krnl/mem/pmm.h>
#include <krnl/vfs/vfs.h>
#include <krnl/debug/debug.h>
#include <krnl/libraries/std/string.h>
#include <krnl/libraries/assert/assert.h>

vm_area_t* vmarea_find(process_t* process, void * address) {
    vm_area_t * current = process->vm_areas;
    while (current) {
        if (address >= current->start && address < (current->start + current->size)) {
            return current;
        }
        current = current->next;
    }
    return 0;
}

vm_area_t* vmarea_collides(process_t * process, vm_area_t * vma) {
    vm_area_t * current = process->vm_areas;
    while (current) {
        if (!((vma->start >= (current->start + current->size)) || 
              ((vma->start + vma->size) <= current->start))) {
            return current;
        }
        current = current->next;
    }
    return 0;
}

vm_area_t * vmarea_create(process_t * process, void * start, uint64_t size, uint64_t area_page_size, uint8_t flags, uint8_t prot, int fd, uint64_t offset) {
    vm_area_t * new_area = (vm_area_t *)kmalloc(sizeof(vm_area_t));
    new_area->start = start;
    new_area->size = size;
    new_area->page_size = area_page_size;
    new_area->flags = flags;
    new_area->prot = prot;
    new_area->fd = fd;
    new_area->cow = 0;
    new_area->offset = offset;
    new_area->next = process->vm_areas;
    process->vm_areas = new_area;
    //kprintf("vmarea_create: Created VMA at %p of size %llu for process %d\n", start, size, process->pid);
    return new_area;
}

//This has to be called after duplicating the VMM of the process!!!!
status_t vmarea_fork(process_t * destination, process_t * source) {
    if (!destination || !source) {
        panic("vmarea_fork: destination or source is NULL");
    }
    vm_area_t * current = source->vm_areas;
    while (current) {
        vm_area_t * copy = vmarea_create(
            destination,
            current->start,
            current->size,
            current->page_size,
            current->flags,
            current->prot,
            current->fd,
            current->offset
        );
        if (current->flags & MAP_PRIVATE) {
            current->cow = 1;
            copy->cow = 1;
            uint8_t vmm_flags = VMM_USER_BIT;
            if (!(current->prot & PROT_EXEC)) vmm_flags |= VMM_NX_BIT;
            status_t st = vmm_mprotect_pages(
                (vmm_root_t *)destination->vmm,
                (uint64_t)current->start,
                (current->size + current->page_size - 1) / current->page_size,
                current->page_size,
                vmm_flags
            );
            if (st != SUCCESS) {
                panic("vmarea_fork: Failed to mprotect pages for copy-on-write");
            }
            st = vmm_mprotect_pages(
                (vmm_root_t *)source->vmm,
                (uint64_t)current->start,
                (current->size + current->page_size - 1) / current->page_size,
                current->page_size,
                vmm_flags
            );
            if (st != SUCCESS) {
                panic("vmarea_fork: Failed to mprotect pages for copy-on-write");
            }
        } else {
           //Shared mapping, nothing special to do 
        }

        //Add a new allocation for the destination process
        uint64_t physical;
        status_t st = vmm_get_physical_address((vmm_root_t *)source->vmm, (uint64_t)current->start, &physical);
        if (st != SUCCESS) {
            panic("vmarea_fork: Failed to get physical address for allocation");
        }
        uint8_t flags = VMM_USER_BIT;
        if (current->prot & PROT_WRITE) flags |= VMM_WRITE_BIT;
        if (!(current->prot & PROT_EXEC)) flags |= VMM_NX_BIT;
        add_allocation(
            (vmm_root_t *)destination->vmm,
            (void *)physical,
            current->start,
            current->size,
            flags
        );
        current = current->next;
    }

    return SUCCESS;;
}

status_t vmarea_try_cow(process_t * process, void * address) {
    vm_area_t * vma = vmarea_find(process, address);
    if (!vma) {
        return FAILURE;
    }
    if (!vma->cow) {
        return FAILURE;
    }

    uint64_t num_pages = (vma->size + vma->page_size - 1) / vma->page_size;

    // Save physical addresses of ALL pages before unmapping any of them
    uint64_t * phys_addrs = (uint64_t *)kmalloc(num_pages * sizeof(uint64_t));
    if (!phys_addrs) {
        panic("vmarea_try_cow: Failed to allocate physical address array");
    }
    for (uint64_t i = 0; i < num_pages; i++) {
        uint64_t vaddr = (uint64_t)vma->start + i * vma->page_size;
        status_t st = vmm_get_physical_address((vmm_root_t *)process->vmm, vaddr, &phys_addrs[i]);
        if (st != SUCCESS) {
            kfree(phys_addrs);
            panic("vmarea_try_cow: Failed to get physical address for page %llu", i);
        }
    }

    status_t st = vmm_unmap_pages(
        (vmm_root_t *)process->vmm,
        (uint64_t)vma->start,
        num_pages,
        vma->page_size
    );
    if (st != SUCCESS) {
        kfree(phys_addrs);
        panic("vmarea_try_cow: Failed to unmap original pages");
    }

    /* Retire the bookkeeping record for the pages just unmapped -- every VMA
       is backed by exactly one contiguous pmm_alloc_pages() block (see
       vmarea_mmap()/vmarea_fork()), tracked by exactly one allocation record
       for (process->vmm, vma->start). malloc() below adds a *new* record at
       the same (root, vaddr) for the freshly-copied pages; add_allocation()
       prepends, so without retiring this one first it would be permanently
       shadowed from every future free()/remove_allocation() lookup and its
       physical pages would never be returned to the allocator (a real bug:
       this leaked one physical block per (fork, subsequent-COW-write) pair
       for the life of the process).

       Removing the *record* has to happen now, before malloc() adds a
       second record for the same (root, vaddr) that remove_allocation()
       couldn't tell apart from this one. But actually *freeing* the old
       physical pages (should_deallocate_pmm + pmm_free_pages) must wait
       until after the copy loop below -- doing it here handed the old,
       still-needed pages straight back to the allocator, which malloc()
       then immediately re-issued as the "new" pages, and its own
       memset-to-zero wiped the source data before it could be copied
       (corrupted the VMA's whole contents instead of just leaking memory
       -- caught by a full isolation test before shipping this fix). So:
       decide now whether it'll be safe to free (no other process still
       maps this physical range), remove the bookkeeping record now, and
       only call pmm_free_pages() once the copy is actually done. */
    uint8_t old_pages_freeable = should_deallocate_pmm((vmm_root_t *)process->vmm, (void *)phys_addrs[0]);
    remove_allocation((vmm_root_t *)process->vmm, vma->start);

    void * ptr = malloc(
        (vmm_root_t *)process->vmm,
        vma->size,
        (uint64_t)vma->start,
        VMM_USER_BIT | ((vma->prot & PROT_WRITE) ? VMM_WRITE_BIT : 0) | ((!(vma->prot & PROT_EXEC)) ? VMM_NX_BIT : 0)
    );
    if (ptr == NULL) {
        kfree(phys_addrs);
        panic("vmarea_try_cow: Failed to allocate new pages for copy-on-write");
    }

    // Copy each page from its original physical address to the new physical address
    for (uint64_t i = 0; i < num_pages; i++) {
        uint64_t new_phys;
        uint64_t new_vaddr = (uint64_t)vma->start + i * vma->page_size;
        st = vmm_get_physical_address((vmm_root_t *)process->vmm, new_vaddr, &new_phys);
        if (st != SUCCESS) {
            kfree(phys_addrs);
            panic("vmarea_try_cow: Failed to get new physical address for page %llu", i);
        }
        memcpy(
            (void *)vmm_to_identity_map(new_phys),
            (void *)vmm_to_identity_map(phys_addrs[i]),
            vma->page_size
        );
    }

    /* Now that the old contents have actually been copied into the new,
       independent pages, it's safe to return the old physical block to the
       allocator (see the comment above the should_deallocate_pmm() call). */
    if (old_pages_freeable) {
        pmm_free_pages((void *)phys_addrs[0], num_pages);
    }

    kfree(phys_addrs);
    vma->cow = 0; //No longer copy-on-write
    return SUCCESS;
}

void * vmarea_find_space(process_t * process, void * hint, uint64_t size, uint64_t page_size) {
    if (hint == 0) hint = (void*)VMM_REGION_U_SPACE_MMAP;
    else {
        //Align to page size
        hint = (void *)(((uint64_t)hint + page_size - 1) & ~(page_size - 1));
    }
    //Align size to page size upwards
    size = (size + page_size - 1) & ~(page_size - 1);

    vm_area_t desired_vma = {
        .start = hint,
        .size = size,
        .flags = 0,
        .prot = 0,
        .page_size = page_size,
        .fd = -1,
        .offset = 0,
    };

    vm_area_t * collision = vmarea_collides(process, &desired_vma);
    uint8_t wrap_around = 0;
    while (collision) {
        if ((uint64_t)hint + size > VMM_REGION_U_SPACE_END) {
            if (wrap_around) {
                return MAP_FAILED; //No space found
            }
            //Wrap around to the beginning
            hint = (void *)VMM_REGION_U_SPACE_MMAP;
            wrap_around = 1;
        } else {
            hint = (void *)((uint64_t)(collision->start + collision->size));
        }
        //Align to page size
        hint = (void *)(((uint64_t)hint + page_size - 1) & ~(page_size - 1));
        desired_vma.start = hint;
        collision = vmarea_collides(process, &desired_vma);
    }

    return hint;
}

void vmarea_sync(process_t * process) {
    vm_area_t * current = process->vm_areas;
    while (current) {
        if (current->fd == -1) goto advance;
        if (current->flags & MAP_ANONYMOUS) goto advance;
        vfs_file_descriptor_t * fd = process_get_fd(process, current->fd);
        if (!fd) panic("vmarea_sync: Invalid file descriptor %d", current->fd);
        uint64_t pages = (current->size + current->page_size - 1) / current->page_size;

        if (vmm_check_and_clean_dirty((vmm_root_t *)process->vmm, (uint64_t)current->start, pages, current->page_size)) {
            size_t saved_position = fd->position;
            fd->position = current->offset;
            vfs_write(fd, (uint8_t*)current->start, current->size);
            fd->position = saved_position;
        }
advance:
        current = current->next;
    }

}

status_t vmarea_addforeign(process_t * process, void * addr, uint64_t length, uint8_t prot, uint8_t flags) {
    vmarea_create(process, addr, length, VMM_PAGE_SIZE_4KB, flags, prot, -1, 0);
    return SUCCESS;
}

status_t vmarea_remove_locked(process_t * process, void * address) {
    vm_area_t * current = process->vm_areas;
    vm_area_t * previous = 0;
    while (current) {
        if (current->start == address) {
            if (previous) {
                previous->next = current->next;
            } else {
                process->vm_areas = current->next;
            }
            kfree(current);
            return SUCCESS;
        }
        previous = current;
        current = current->next;
    }
    return FAILURE;
}

status_t vmarea_remove(process_t * process, void * address) {
    status_t st = vmarea_remove_locked(process, address);
    return st;
}

void vmarea_remove_all(process_t * process) {
    vm_area_t * current = process->vm_areas;
    while (current) {
        vm_area_t * next = current->next;
        //kprintf("vmarea_remove_all: Removing VMA at %p of size %llu for process %d\n", current->start, current->size, process->pid);
        free(process->vmm, current->start); //Free the mapped memory
        kfree(current);
        current = next;
    }
    process->vm_areas = 0;
}

status_t vmarea_mprotect(process_t * process, void * address, uint64_t size, uint8_t new_prot) {
    vm_area_t * current = process->vm_areas;
    while (current) {
        if (address >= current->start && (uint64_t)address + size <= (uint64_t)(current->start + current->size)) {
            current->prot = new_prot;
            //vmm_mprotect_pages
            status_t st = vmm_mprotect_pages(
                (vmm_root_t *)process->vmm,
                (uint64_t)address,
                (size + current->page_size - 1) / current->page_size,
                current->page_size,
                VMM_USER_BIT |
                ((new_prot & PROT_WRITE) ? VMM_WRITE_BIT : 0) |
                ((!(new_prot & PROT_EXEC)) ? VMM_NX_BIT : 0)
            );
            if (st != SUCCESS) {
                panic("vmarea_mprotect: Failed to mprotect pages");
            }
        }
        current = current->next;
    }
    return SUCCESS;
}

void * vmarea_mmap(process_t * process, void * addr, uint64_t length, uint8_t prot, uint8_t flags, int fd, uint64_t offset, uint8_t force) {
    if (process == NULL) {
        panic("vmarea_mmap: process is NULL");
    }

    void* old_addr = addr;
    if (addr == 0) {
        addr = vmarea_find_space(process, addr, length, VMM_PAGE_SIZE_4KB);
        if (addr == MAP_FAILED) {
            return MAP_FAILED;
        }
        if (force && addr != old_addr) {
            panic("vmarea_mmap: Could not find space at the forced address %p", old_addr);
        }
    } else {
        //Check for collisions
        vm_area_t desired_vma = {
            .start = addr,
            .size = length,
            .flags = flags,
            .prot = prot,
            .page_size = VMM_PAGE_SIZE_4KB,
            .fd = fd,
            .offset = offset,
        };
        vm_area_t * collision = vmarea_collides(process, &desired_vma);
        if (collision) {
            return MAP_FAILED;
        }
    }

    void * ptr = malloc(
        (vmm_root_t *)process->vmm,
        length,
        (uint64_t)addr,
        VMM_USER_BIT | ((prot & PROT_WRITE) ? VMM_WRITE_BIT : 0) | ((!(prot & PROT_EXEC)) ? VMM_NX_BIT : 0)
    );
    if (ptr == NULL) {
        panic("vmarea_mmap: Failed to allocate memory memory");
    }

    void * identity = to_kident((vmm_root_t *)process->vmm, ptr);
    if (identity == NULL) {
        panic("vmarea_mmap: Failed to get identity mapped address");
    }

    vmarea_create(process, ptr, length, VMM_PAGE_SIZE_4KB, flags, prot, fd, offset);

    //If mapping a file, read the file contents
    if (fd != -1 && !(flags & MAP_ANONYMOUS)) {
        vfs_file_descriptor_t * file_desc = process_get_fd(process, fd);
        if (!file_desc) {
            panic("vmarea_mmap: Invalid file descriptor %d", fd);
        }
        size_t saved_position = file_desc->position;
        file_desc->position = offset;
        ssize_t read_bytes = vfs_read(file_desc, (uint8_t *)identity, length);
        if (read_bytes < 0) {
            panic("vmarea_mmap: Failed to read file for mmap");
        }
        file_desc->position = saved_position;
    }
    //kprintf("vmarea_mmap: Mapped %llu bytes at %p for process %d\n", length, ptr, process->pid);
    return ptr;
}

status_t vmarea_munmap(process_t * process, void * address) {
    vm_area_t * vma = vmarea_find(process, address);
    if (!vma) {
        return FAILURE;
    }

    //Unmap with kfree_userland
    free(process->vmm, vma->start);

    //Remove vm area
    status_t st = vmarea_remove_locked(process, vma->start);
    if (st != SUCCESS) {
        return st;
    }

    return SUCCESS;
}
