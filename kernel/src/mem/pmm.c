#include <krnl/mem/pmm.h>
#include <krnl/boot/bootloaders/bootloader.h>
#include <krnl/debug/debug.h>

struct memory_region {
    uint64_t start;
    uint64_t pages;
    uint8_t *locks;
    uint64_t next_free_hint;
};

struct memory_region valid_memory_regions[128];
uint64_t memory_region_count = 0;

void pmm_remap(uint64_t offset) {
    //Move all memory regions by offset
    for (uint64_t i = 0; i < memory_region_count; i++) {
        valid_memory_regions[i].locks = (uint8_t *)((uint64_t)valid_memory_regions[i].locks + offset);
    }
}

void pmm_init(void) {
    uint64_t entries = get_memory_map_entries();
    if (entries > 128) {
        panic("PMM: Too many memory map entries (%d), max is 128\n", entries);
    }

    for (uint64_t i = 0; i < entries; i++) {
        if (get_memory_map_type(i) != BOOTLOADER_MEMMAP_USABLE) {
            continue;
        }

        uint64_t region_start = get_memory_map_base(i);
        uint64_t region_size = get_memory_map_length(i);
        uint64_t num_pages = region_size / PMM_PAGE_SIZE;
        if (num_pages == 0) {
            continue; //Region too small
        }
        uint64_t lock_array_size = (num_pages + 7) / 8; // Round up to nearest byte
        
        //The region first n pages are reserved for the lock array, then the rest is usable
        //Make sure to align the usable memory start to a page boundary
        uint64_t lock_array_start = region_start;
        uint64_t usable_memory_start = (lock_array_start + lock_array_size + PMM_PAGE_SIZE - 1) & ~(PMM_PAGE_SIZE - 1);
        uint64_t usable_memory_size = (region_start + region_size) - usable_memory_start;
        if (usable_memory_size < PMM_PAGE_SIZE) {
            continue; //Not enough usable memory
        }

        valid_memory_regions[memory_region_count].start = usable_memory_start;
        valid_memory_regions[memory_region_count].pages = usable_memory_size / PMM_PAGE_SIZE;
        valid_memory_regions[memory_region_count].locks = (uint8_t *)lock_array_start;
        valid_memory_regions[memory_region_count].next_free_hint = 0;

        //Zero out the lock array
        for (uint64_t j = 0; j < lock_array_size; j++) {
            valid_memory_regions[memory_region_count].locks[j] = 0;
        }

        memory_region_count++;
    }
}

void *pmm_alloc_pages(uint64_t num_pages) {
    for (uint64_t i = 0; i < memory_region_count; i++) {
        struct memory_region *region = &valid_memory_regions[i];

        uint64_t total = region->pages;
        uint64_t start = region->next_free_hint;
        uint64_t page = start;
        uint64_t consecutive = 0;
        uint64_t first_page = 0;

        uint64_t attempts = 0;

        while (attempts < total) {
            uint64_t byte_index = page >> 3;
            uint8_t  bit_index  = page & 7;
            uint8_t  mask       = 1u << bit_index;

            // free?
            if (!(region->locks[byte_index] & mask)) {
                if (consecutive == 0) {
                    first_page = page;
                }
                consecutive++;

                if (consecutive == num_pages) {
                    // mark used
                    for (uint64_t p = first_page; p < first_page + num_pages; p++) {
                        region->locks[p >> 3] |= (1u << (p & 7));
                    }

                    // next searches begin right after this allocation
                    region->next_free_hint = (first_page + num_pages) % total;

                    return (void*)(region->start + first_page * PMM_PAGE_SIZE);
                }
            } else {
                consecutive = 0;
            }

            // advance circularly
            page = (page + 1 == total) ? 0 : page + 1;
            attempts++;
        }
    }

    panic("PMM: Failed to allocate %d pages\n", num_pages);
    return NULL;
}

void pmm_free_pages(void *addr, uint64_t num_pages) {
    uint64_t target = (uint64_t)addr;

    // Find the region this address belongs to
    for (uint64_t i = 0; i < memory_region_count; i++) {
        struct memory_region *region = &valid_memory_regions[i];

        uint64_t region_start = region->start;
        uint64_t region_end   = region_start + region->pages * PMM_PAGE_SIZE;

        if (target < region_start || target >= region_end)
            continue;

        // Calculate starting page index inside region
        uint64_t start_page = (target - region_start) / PMM_PAGE_SIZE;

        // Free the pages (clear bits)
        for (uint64_t p = start_page; p < start_page + num_pages; p++) {
            uint64_t byte_index = p >> 3;
            uint8_t  bit_index  = p & 7;
            region->locks[byte_index] &= ~(1u << bit_index);
        }

        // Optional optimization:
        // If these freed pages are before the current next hint,
        // move the next-fit cursor back to them.
        if (start_page < region->next_free_hint)
            region->next_free_hint = start_page;

        return;
    }

    // Optional: panic or log if address not found
    panic("PMM: Attempted to free non-PMM memory at address %p\n", addr);
}
