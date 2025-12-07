#ifndef _VMAREA_H
#define _VMAREA_H
#include <krnl/libraries/std/stdint.h>

typedef struct vm_area {
    void * start;
    uint64_t size;
    uint64_t page_size;
    uint8_t flags;
    uint8_t prot;
    uint8_t cow;
    int fd;
    off_t offset;
    struct vm_area * next;
} vm_area_t;

#endif