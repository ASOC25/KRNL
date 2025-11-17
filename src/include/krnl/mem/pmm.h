#ifndef _PMM_H
#define _PMM_H

#include <krnl/libraries/std/stdint.h>

void pmm_init(void);
void *pmm_alloc_pages(uint64_t num_pages);
void pmm_free_pages(void *addr, uint64_t num_pages);

#endif