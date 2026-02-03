#ifndef _LOADER_H
#define _LOADER_H

#include <krnl/libraries/std/elf.h>
#include <krnl/mem/vmm.h>
#include <krnl/vfs/vfs.h>
#include <krnl/process/process.h>
#include <krnl/debug/debug.h>

#define SIGNAL_TRAMPOLINE_ADDRESS ((void*)0x80000000)
#define DYNAMIC_LINKER_BASE_ADDRESS ((void*)0x40000000)

#define AT_NULL   0	/* end of vector */
#define AT_IGNORE 1	/* entry should be ignored */
#define AT_EXECFD 2	/* file descriptor of program */
#define AT_PHDR   3	/* program headers for program */
#define AT_PHENT  4	/* size of program header entry */
#define AT_PHNUM  5	/* number of program headers */
#define AT_PAGESZ 6	/* system page size */
#define AT_BASE   7	/* base address of interpreter */
#define AT_FLAGS  8	/* flags */
#define AT_ENTRY  9	/* entry point of program */
#define AT_NOTELF 10	/* program is not ELF */
#define AT_UID    11	/* real uid */
#define AT_EUID   12	/* effective uid */
#define AT_GID    13	/* real gid */
#define AT_EGID   14	/* effective gid */
#define AT_PLATFORM 15  /* string identifying CPU for optimizations */
#define AT_HWCAP  16    /* arch dependent hints at CPU capabilities */
#define AT_CLKTCK 17	/* frequency at which times() increments */

/* AT_* values 18 through 22 are reserved */
#define AT_SECURE 23   /* secure mode boolean */
#define AT_BASE_PLATFORM 24	/* string identifying real platform, may * differ from AT_PLATFORM. */
#define AT_RANDOM 25	/* address of 16 random bytes */
#define AT_HWCAP2 26	/* extension of AT_HWCAP */
#define AT_RSEQ_FEATURE_SIZE	27	/* rseq supported feature size */
#define AT_RSEQ_ALIGN		28	/* rseq allocation alignment */
#define AT_HWCAP3 29	/* extension of AT_HWCAP */
#define AT_HWCAP4 30	/* extension of AT_HWCAP */
#define AT_EXECFN  31	/* filename of program */
#define AT_SYSINFO_EHDR 33	/* sysinfo page */
#define AT_MINSIGSTKSZ	51	/* minimal stack size for signal delivery */

struct proc_ld {
    void* at_phdr;
    char* ld_path;
};

struct auxv{
    uint64_t a_type;
    void* a_val;
};

typedef struct loaded_elf {
    Elf64_Ehdr * ehdr;
    struct auxv * auxv;
    uint64_t auxv_size;
    struct proc_ld * ld;
    uint64_t ld_size;
} loaded_elf_t;

loaded_elf_t* elf_load_elf(process_t * process, const char * filename, thread_t* thread);
void * loader_create_args(void * stack, uint64_t max_size, char ** argv, char ** envp, struct auxv* auxv);
status_t allocate_signal_trampoline(process_t* process);
#endif