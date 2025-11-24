#include <krnl/mem/allocator.h>
#include <krnl/libraries/std/stdint.h>
#include <krnl/libraries/std/string.h>

/* Simple allocator with a free-list for early kernel use.
   Implements kmalloc and kfree (basic):
   - Blocks have a small header with size and 'free' flag.
   - Free blocks are kept in a singly-linked free list.
   - On free we insert into the free list and attempt to coalesce with
     adjacent free blocks.

   This is intentionally simple (not_LOCKED, no best-fit tuning), but
   sufficient to demonstrate kmalloc/kfree behavior at memory level.
*/

#define KERNEL_HEAP_SIZE (1024 * 1024) /* 1 MiB */

static unsigned char kernel_heap[KERNEL_HEAP_SIZE];
static uint64_t heap_ptr = (uint64_t)kernel_heap;
static const uint64_t heap_end = (uint64_t)kernel_heap + KERNEL_HEAP_SIZE;

typedef struct block_header {
    uint64_t size;           /* payload size in bytes */
    uint8_t free;            /* 1 if free, 0 if allocated */
    uint8_t padding[7];
    struct block_header *free_next; /* next in free list (valid only if free) */
} block_header_t;

static block_header_t *free_list_head = NULL;

static inline uint64_t align_up(uint64_t v, uint64_t align) {
    return (v + (align - 1)) & ~(align - 1);
}

/* Compute pointer to payload from header */
static inline void *header_to_payload(block_header_t *h) {
    return (void *)((unsigned char *)h + sizeof(block_header_t));
}

/* Compute header pointer from payload pointer */
static inline block_header_t *payload_to_header(void *p) {
    return (block_header_t *)((unsigned char *)p - sizeof(block_header_t));
}

/* Compute physical next header (may be beyond heap_ptr) */
static inline block_header_t *physical_next(block_header_t *h) {
    unsigned char *next = (unsigned char *)h + sizeof(block_header_t) + h->size;
    if ((uint64_t)next >= heap_ptr || (uint64_t)next >= heap_end) return NULL;
    return (block_header_t *)next;
}

/* Remove a free block from the free list (by pointer). */
static void remove_from_free_list(block_header_t *b) {
    block_header_t *prev = NULL;
    block_header_t *cur = free_list_head;
    while (cur) {
        if (cur == b) {
            if (prev) prev->free_next = cur->free_next;
            else free_list_head = cur->free_next;
            cur->free_next = NULL;
            return;
        }
        prev = cur;
        cur = cur->free_next;
    }
}

/* Try to split a free block if it's large enough; returns the block to use */
static block_header_t *split_block_if_needed(block_header_t *b, uint64_t size) {
    /* If leftover would be large enough to hold a header + 8 bytes, split */
    uint64_t leftover = b->size - size;
    if (leftover >= (sizeof(block_header_t) + 8)) {
        unsigned char *new_hdr_addr = (unsigned char *)b + sizeof(block_header_t) + size;
        block_header_t *new_hdr = (block_header_t *)new_hdr_addr;
        new_hdr->size = leftover - sizeof(block_header_t);
        new_hdr->free = 1;
        new_hdr->free_next = b->free_next;
        b->size = size;
        b->free_next = NULL;
        /* replace b in free list with new_hdr (we assume caller will remove b)
           but simplest is to leave new_hdr in place of b by linking from previous;
           caller will remove b and then insert new_hdr. */
        return b;
    }
    return b;
}

void * kmalloc(uint64_t size) {
    if (size == 0) return NULL;

    const uint64_t align = 8;
    size = align_up(size, align);

    /* Search free list first (first-fit) */
    block_header_t *prev = NULL;
    block_header_t *cur = free_list_head;
    while (cur) {
        if (cur->size >= size) {
            /* Found suitable free block */
            /* Possibly split */
            block_header_t *used = split_block_if_needed(cur, size);
            /* Remove used from free list */
            if (prev) prev->free_next = cur->free_next;
            else free_list_head = cur->free_next;
            used->free = 0;
            used->free_next = NULL;
            return header_to_payload(used);
        }
        prev = cur;
        cur = cur->free_next;
    }

    /* No free block found — allocate at the bump pointer */
    uint64_t alloc_start = align_up(heap_ptr, align);
    uint64_t total = sizeof(block_header_t) + size;
    uint64_t alloc_end = alloc_start + total;
    if (alloc_end > heap_end) return NULL; /* out of memory */

    block_header_t *h = (block_header_t *)alloc_start;
    h->size = size;
    h->free = 0;
    h->free_next = NULL;
    heap_ptr = alloc_end;
    return header_to_payload(h);
}

void kfree(void *ptr) {
    if (!ptr) return;
    block_header_t *h = payload_to_header(ptr);
    /* basic sanity: ensure header is inside heap */
    if ((uint64_t)h < (uint64_t)kernel_heap || (uint64_t)h >= heap_ptr) return;

    h->free = 1;
    /* insert at head of free list */
    h->free_next = free_list_head;
    free_list_head = h;

    /* Try to coalesce with next physical block */
    block_header_t *next = physical_next(h);
    while (next && next->free) {
        /* Remove next from free list */
        remove_from_free_list(next);
        /* merge */
        h->size = h->size + sizeof(block_header_t) + next->size;
        next = physical_next(h);
    }

    /* Try to coalesce with previous block by scanning from base */
    block_header_t *cur = (block_header_t *)kernel_heap;
    while (cur) {
        block_header_t *n = physical_next(cur);
        if (n == h) {
            /* cur is previous */
            if (cur->free) {
                /* remove h from free list */
                remove_from_free_list(h);
                /* remove cur from free list */
                remove_from_free_list(cur);
                /* merge cur and h */
                cur->size = cur->size + sizeof(block_header_t) + h->size;
                /* insert merged block into free list */
                cur->free = 1;
                cur->free_next = free_list_head;
                free_list_head = cur;
            }
            break;
        }
        if (!n) break;
        cur = n;
    }
}

/* --- Inspection helpers --- */
uint64_t kmalloc_used(void) {
    return heap_ptr - (uint64_t)kernel_heap;
}

uint64_t kmalloc_capacity(void) {
    return (uint64_t)KERNEL_HEAP_SIZE;
}

void *kmalloc_base(void) {
    return (void *)kernel_heap;
}

void *kmalloc_current(void) {
    return (void *)heap_ptr;
}