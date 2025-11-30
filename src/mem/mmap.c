#include <krnl/mem/mmap.h>
#include <krnl/process/process.h>
#include <krnl/mem/allocator.h>
#include <krnl/vfs/vfs.h>
#include <krnl/debug/debug.h>

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

void vmarea_create(process_t * process, void * start, uint64_t size, uint64_t area_page_size, uint8_t flags, uint8_t prot, int fd, uint64_t offset) {
    vm_area_t * new_area = (vm_area_t *)kmalloc(sizeof(vm_area_t));
    new_area->start = start;
    new_area->size = size;
    new_area->page_size = area_page_size;
    new_area->flags = flags;
    new_area->prot = prot;
    new_area->fd = fd;
    new_area->offset = offset;
    new_area->next = process->vm_areas;
    process->vm_areas = new_area;
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

status_t vmarea_remove(process_t * process, void * address) {
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

void vmarea_remove_all(process_t * process) {
    vm_area_t * current = process->vm_areas;
    while (current) {
        vm_area_t * next = current->next;
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
    return FAILURE;
}

void * vmarea_mmap(process_t * process, void * addr, uint64_t length, uint8_t prot, uint8_t flags, int fd, uint64_t offset) {
    if (process == NULL) {
        panic("vmarea_mmap: process is NULL");
    }
    if (addr == 0) {
        addr = vmarea_find_space(process, addr, length, VMM_PAGE_SIZE_4KB);
        if (addr == MAP_FAILED) {
            return MAP_FAILED;
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

    farlands_t farlands;
    status_t st = malloc(
        (vmm_root_t *)process->vmm,
        length,
        (uint64_t)addr,
        VMM_USER_BIT | ((prot & PROT_WRITE) ? VMM_WRITE_BIT : 0) | ((!(prot & PROT_EXEC)) ? VMM_NX_BIT : 0),
        &farlands
    );
    if (st != SUCCESS) {
        panic("vmarea_mmap: Failed to allocate farlands memory");
    }

    vmarea_create(process, addr, length, VMM_PAGE_SIZE_4KB, flags, prot, fd, offset);

    //If mapping a file, read the file contents
    if (fd != -1 && !(flags & MAP_ANONYMOUS)) {
        vfs_file_descriptor_t * file_desc = process_get_fd(process, fd);
        if (!file_desc) {
            panic("vmarea_mmap: Invalid file descriptor %d", fd);
        }
        size_t saved_position = file_desc->position;
        file_desc->position = offset;
        ssize_t read_bytes = vfs_read(file_desc, (uint8_t *)farlands.handle, length);
        if (read_bytes < 0) {
            panic("vmarea_mmap: Failed to read file for mmap");
        }
        file_desc->position = saved_position;
    }

    return addr;
    
}

status_t vmarea_munmap(process_t * process, void * address) {
    vm_area_t * vma = vmarea_find(process, address);
    if (!vma) {
        return FAILURE;
    }

    //Unmap with kfree_userland
    free(process->vmm, address);

    //Remove vm area
    status_t st = vmarea_remove(process, address);
    if (st != SUCCESS) {
        return st;
    }

    return SUCCESS;
}