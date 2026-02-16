#ifndef _MMAP_H
#define _MMAP_H

#include <krnl/libraries/std/stdint.h>
#include <krnl/libraries/std/stddef.h>
#include <krnl/process/process.h>
#include <krnl/mem/vmarea.h>

#define PROT_READ 0x1
#define PROT_WRITE 0x2
#define PROT_EXEC 0x4
#define PROT_NONE 0x8

#define MAP_FAILED ((void *)(-1))
#define MAP_FILE      0x00
#define MAP_SHARED    0x01
#define MAP_PRIVATE   0x02
#define MAP_FIXED     0x10
#define MAP_ANON      0x20
#define MAP_ANONYMOUS 0x20
#define MAP_GROWSDOWN 0x100
#define MAP_DENYWRITE 0x800
#define MAP_EXECUTABLE 0x1000
#define MAP_LOCKED    0x2000
#define MAP_NORESERVE 0x4000
#define MAP_POPULATE  0x8000
#define MAP_NONBLOCK  0x10000
#define MAP_STACK     0x20000
#define MAP_HUGETLB   0x40000
#define MAP_SYNC      0x80000
#define MAP_FIXED_NOREPLACE 0x100000

void * vmarea_mmap(process_t * process, void * addr, uint64_t length, uint8_t prot, uint8_t flags, int fd, uint64_t offset, uint8_t force);
status_t vmarea_mprotect(process_t * process, void * address, uint64_t size, uint8_t new_prot);
status_t vmarea_munmap(process_t * process, void * address);
status_t vmarea_fork(process_t * destination, process_t * source);
status_t vmarea_addforeign(process_t * process, void * addr, uint64_t length, uint8_t prot, uint8_t flags);
status_t vmarea_try_cow(process_t * process, void * address);
void vmarea_remove_all(process_t * process);
void vmarea_sync(process_t * process);
#endif