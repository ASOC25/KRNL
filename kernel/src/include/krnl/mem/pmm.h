#ifndef _PMM_H
#define _PMM_H

#include <krnl/libraries/std/stdint.h>

#define PMM_PAGE_SIZE 4096

void pmm_init(void);
void pmm_remap(uint64_t offset);
void *pmm_alloc_pages(uint64_t num_pages);
void pmm_free_pages(void *addr, uint64_t num_pages);

#endif