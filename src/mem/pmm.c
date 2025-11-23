#include "krnl/boot/bootloaders/bootloader.h"
#include "krnl/debug/debug.h"
#include <krnl/boot/bootloaders/limine/bootservices.h>
#include <krnl/mem/pmm.h>
#include <stdint.h>

#define PAGE_SIZE 4096
#define ADDRESSABLE_MEMORY ((uint64_t)8 * 1024 * 1024 * 1024)

static uint8_t page_occupation[ADDRESSABLE_MEMORY / PAGE_SIZE] = {0};

static uint64_t memmap_len = 0;

struct memory {
  uint64_t start;
  uint64_t len;
};

static struct memory memory = {0};

void pmm_init(void) {
  memmap_len = get_memory_map_entries();
  uint64_t region_len = 0;
  for (uint64_t i = 0; i < memmap_len; i++) {
    if (get_memory_map_type(i) == 0 &&
        (region_len = get_memory_map_length(i)) > memory.len) {
      memory.start = get_memory_map_base(i);
      memory.len = region_len;
    }
  }
  kprintf("entradas: %d\n", memmap_len);
  kprintf("entrada mas grande - base: %x, length : %x\n", memory.start,
          memory.len);
}

void *pmm_alloc_pages(uint64_t num_pages) {
  uint64_t pages_left = num_pages;
  uint64_t i;
  uint64_t start;

  for (i = 0; i < memory.len / PAGE_SIZE / 8 && pages_left > 0; i++) {
    if (pages_left >= 8) {
      if (page_occupation[i] == 0x00) {
        pages_left -= 8;
      } else {
        pages_left = num_pages;
      }
    } else {
      uint8_t mask = (1 << pages_left) - 1;
      if ((page_occupation[i] & mask) == 0) {
        page_occupation[i] |= mask;
        pages_left = 0;
        i--;
      } else {
        pages_left = num_pages;
      }
    }
  }
  start = i - num_pages / 8;
  while (i-- > start) {
    page_occupation[i] = 0xFF;
  }
  return (void *)(memory.start + start * PAGE_SIZE);
}

void pmm_free_pages(void *addr, uint64_t num_pages) {
  uint32_t first_page = (((uint64_t)addr) - memory.start) / PAGE_SIZE;

  while (num_pages > 0) {
    if (num_pages >= 8) {
      page_occupation[first_page++] = 0x00;
      num_pages -= 8;
    } else {
      uint8_t mask = (1 << num_pages) - 1;
      page_occupation[first_page] &= ~mask;
      num_pages = 0;
    }
  }
}
