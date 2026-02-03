#include <krnl/process/loader.h>
#include <krnl/libraries/std/string.h>
#include <krnl/libraries/std/stddef.h>
#include <krnl/libraries/std/stdint.h>
#include <krnl/mem/allocator.h>
#include <krnl/libraries/std/elf.h>
#include <krnl/mem/mmap.h>
#include <krnl/vfs/vfs.h>
#include <krnl/process/process.h>

const char * elf_class[] = {
    "Invalid class",
    "ELF32",
    "ELF64"
};

const char * elf_data[] = {
    "Invalid data",
    "2's complement, little endian",
    "2's complement, big endian"
};

const char * elf_osabi[] = {
    "UNIX System V",
    "HP-UX",
    "NetBSD",
    "Linux",
    "GNU Hurd",
    "Solaris",
    "AIX",
    "IRIX",
    "FreeBSD",
    "Tru64",
    "Novell Modesto",
    "OpenBSD",
    "OpenVMS",
    "NonStop Kernel",
    "AROS",
    "Fenix OS",
    "CloudABI",
    "Stratus Technologies OpenVOS"
};

const char * elf_type[] = {
    "NONE",
    "REL",
    "EXEC",
    "DYN",
    "CORE"
};

const char * elf_machine[] = {
    "No machine",
    "AT&T WE 32100",
    "SPARC",
    "Intel 80386",
    "Motorola 68000",
    "Motorola 88000",
    "Reserved for future use (was EM_486)",
    "Intel 80860",
    "MIPS I Architecture",
    "IBM System/370 Processor",
    "MIPS RS3000 Little-endian",
    "Reserved for future use",
    "Reserved for future use",
    "Reserved for future use",
    "Reserved for future use",
    "Hewlett-Packard PA-RISC",
    "Reserved for future use",
    "Fujitsu VPP500",
    "Enhanced instruction set SPARC",
    "Intel 80960",
    "PowerPC",
    "64-bit PowerPC",
    "IBM System/390 Processor",
    "Reserved for future use",
    "Reserved for future use",
    "Reserved for future use",
    "Reserved for future use",
    "Reserved for future use",
    "Reserved for future use",
    "Reserved for future use",
    "Reserved for future use",
    "Reserved for future use",
    "Reserved for future use",
    "Reserved for future use",
    "Reserved for future use",
    "Reserved for future use",
    "NEC V800",
    "Fujitsu FR20",
    "TRW RH-32",
    "Motorola RCE",
    "Advanced RISC Machines ARM",
    "Digital Alpha",
    "Hitachi SH",
    "SPARC Version 9",
    "Siemens TriCore embedded processor",
    "Argonaut RISC Core, Argonaut Technologies Inc.",
    "Hitachi H8/300",
    "Hitachi H8/300H",
    "Hitachi H8S",
    "Hitachi H8/500",
    "Intel IA-64 processor architecture",
    "Stanford MIPS-X",
    "Motorola ColdFire",
    "Motorola M68HC12",
    "Fujitsu MMA Multimedia Accelerator",
    "Siemens PCP",
    "Sony nCPU embedded RISC processor",
    "Denso NDR1 microprocessor",
    "Motorola Star*Core processor",
    "Toyota ME16 processor",
    "STMicroelectronics ST100 processor",
    "Advanced Logic Corp. TinyJ embedded processor family",
    "AMD x86-64 architecture",
    "Sony DSP Processor",
    "Digital Equipment Corp. PDP-10",
    "Digital Equipment Corp. PDP-11",
    "Siemens FX66 microcontroller",
    "STMicroelectronics ST9+ 8/16 bit microcontroller",
    "STMicroelectronics ST7 8-bit microcontroller",
    "Motorola MC68HC16 Microcontroller",
    "Motorola MC68HC11 Microcontroller",
    "Motorola MC68HC08 Microcontroller",
    "Motorola MC68HC05 Microcontroller",
    "Silicon Graphics SVx",
    "STMicroelectronics ST19 8-bit microcontroller",
    "Digital VAX",
    "Axis Communications 32-bit embedded processor",
    "Infineon Technologies 32-bit embedded processor",
    "Element 14 64-bit DSP Processor",
    "LSI Logic 16-bit DSP Processor",
    "Donald Knuth's educational 64-bit processor",
    "Harvard University machine-independent object files",
    "SiTera Prism",
    "Atmel AVR 8-bit microcontroller",
    "Fujitsu FR30",
    "Mitsubishi D10V",
    "Mitsubishi D30V",
    "NEC v850",
    "Mitsubishi M32R",
    "Matsushita MN10300",
    "Matsushita MN10200",
    "picoJava",
    "OpenRISC 32-bit embedded processor",
    "ARC Cores Tangent-A5",
    "Tensilica Xtensa Architecture",
    "Alphamosaic VideoCore processor",
    "Thompson Multimedia General Purpose Processor",
    "National Semiconductor 32000 series",
    "Tenor Network TPC processor",
    "Trebia SNP 1000 processor",
    "STMicroelectronics (www.st.com) ST200 microcontroller"
};

const char * elf_version[] = {
    "Invalid version",
    "Current version"
};

void * loader_create_args(void * stack, uint64_t max_size, char ** argv, char ** envp, struct auxv* auxv) {
    // Create the stack with the following layout:
    // HIGHEST ADDRESS (stack)
    // +------------------+
    // | envp strings     |
    // | argv strings     |
    // +------------------+ (stack - pointer_table_size)
    // | auxv entries     |
    // |------------------+
    // | envp pointers    |
    // | argv pointers    |
    // | argc             |
    // +------------------+ (stack - total_size)
    // LOWEST ADDRESS

    int argc = 0; if (argv != NULL) {while (argv[argc] != NULL) argc++;} else argc = 0;
    int envc = 0; if (envp != NULL) {while (envp[envc] != NULL) envc++;} else envc = 0;
    int auxc = 0; if (auxv != NULL) {while (auxv[auxc].a_type != AT_NULL) auxc++;} else auxc = 0;

    uint64_t argv_pointers[argc];
    uint64_t envp_pointers[envc];
    uint64_t ptr_buffer[0x4000];
    uint64_t * ptr = ptr_buffer;
    uint64_t original_addr = (uint64_t)ptr;
    kprintf("Argc at address: %p, value: %d\n", (void *)ptr, argc);
    *ptr++ = argc;
    for (int i = 0; i < argc; i++) {argv_pointers[i] = (uint64_t) ptr; *ptr = 0x1234; ptr++;}
    *ptr++ = 0x0;
    for (int i = 0; i < envc; i++) {envp_pointers[i] = (uint64_t) ptr; *ptr = 0x5678; ptr++;}
    *ptr++ = 0x0;
    for (int i = 0; i < auxc; i++) {*(struct auxv*)ptr = auxv[i]; ptr += 2;}
    ((struct auxv*)ptr)->a_type = AT_NULL;
    ((struct auxv*)ptr)->a_val  = NULL;
    ptr += 2;

    uint64_t total_size_ascii = 0;
    for (int i = 0; i < argc; i++) {
        total_size_ascii += strlen(argv[i]) + 1; // +1 for null terminator
    }
    for (int i = 0; i < envc; i++) {
        total_size_ascii += strlen(envp[i]) + 1; // +1 for null terminator
    }

    // Make sure the the end of the stack (ptr+total_size_ascii) will be aligned to 16 bytes by increasing ptr
    uint64_t stack_bottom = (uint64_t)ptr + (uint64_t)total_size_ascii;
    char * ascii_ptr = (char *)ptr;
    while (stack_bottom % 16 != 0) {
        ascii_ptr++;
        stack_bottom++;
    }
    ascii_ptr += 8; 

    for (int i = 0; i < argc; i++) {
        memcpy(ascii_ptr, argv[i], strlen(argv[i]) + 1);
        *(uint64_t*)argv_pointers[i] = (uint64_t)(ascii_ptr - original_addr);
        ascii_ptr += (strlen(argv[i]) + 1);
    }
    
    for (int i = 0; i < envc; i++) {
        memcpy(ascii_ptr, envp[i], strlen(envp[i]) + 1);
        *(uint64_t*)envp_pointers[i] = (uint64_t)(ascii_ptr - original_addr);
        ascii_ptr += (strlen(envp[i]) + 1);
    }

    uint64_t size = (uint64_t)ascii_ptr - original_addr;
    //Copy ptr to the stack at the end of the stack
    if (size > max_size) {
        panic("Stack size exceeds maximum allowed size");
    }
    if (stack == NULL) {
        panic("Stack pointer is NULL");
    }

    // Copy the stack to the provided stack pointer

    memcpy(stack - size, (void *)(original_addr), size);

    //Correct the pointers in the stack
    char * stack_ptr = (char *)(stack - size);
    char  ** argv_ptr = (char **)(stack_ptr + sizeof(uint64_t));
    for (int i = 0; i < argc; i++) {
        if (argv_ptr[i] != NULL) {
            argv_ptr[i] += ((uint64_t)(stack - size));
        } else {
            argv_ptr[i] = NULL;
        }
    }
    char  ** envp_ptr = (char **)(stack_ptr + sizeof(uint64_t) + (argc + 1) * sizeof(uint64_t));
    for (int i = 0; i < envc; i++) {
        if (envp_ptr[i] != NULL) {
            envp_ptr[i] += ((uint64_t)(stack - size));
        } else {
            envp_ptr[i] = NULL;
        }
    }
    
    return stack - size;
}

status_t parse_elf_file(uint8_t * buffer) {
    Elf64_Ehdr * header = (Elf64_Ehdr *) buffer;
    if (memcmp(header->e_ident, ELFMAG, SELFMAG) != 0) {
        panic("elf_load_elf: Invalid ELF magic number");
        return FAILURE;
    }

    return SUCCESS;
}

status_t allocate_signal_trampoline(process_t* process) {
    void * addr = vmarea_mmap(process, SIGNAL_TRAMPOLINE_ADDRESS, 0x1000, PROT_READ | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0, 1);
    if (addr == NULL || addr != SIGNAL_TRAMPOLINE_ADDRESS) {
        panic("allocate_signal_trampoline: Failed to allocate signal trampoline");
        return FAILURE;
    }

    //Copy the signal trampoline code
    extern uint8_t signal_trampoline_start[];
    extern uint8_t signal_trampoline_end[];
    size_t code_size = (size_t)(signal_trampoline_end - signal_trampoline_start);
    void * identity = to_kident((vmm_root_t *)process->vmm, addr);
    if (identity == NULL) {
        panic("allocate_signal_trampoline: Failed to get identity mapped address for signal trampoline");
        return FAILURE;
    }
    memcpy(identity, signal_trampoline_start, code_size);
    return SUCCESS;
}

status_t allocate_segment(process_t* process, uint8_t * elf_datab, Elf64_Phdr * program_header, void* base) {
    //kprintf("Starting ALLOCATE SEGMENT\n");
    if (program_header->p_type != PT_LOAD) panic("allocate_segment: Not a loadable segment");

    uint64_t vaddr_offset = program_header->p_vaddr & 0xfff;
    uint64_t vaddr = (program_header->p_vaddr & ~0xfff) + (uint64_t)base;
    if (vaddr >= VMM_REGION_U_SPACE_END) {
        panic("allocate_segment: Virtual address out of user space range");
    }

    uint64_t total_size = vaddr_offset + program_header->p_memsz;
    uint64_t total_pages = (total_size + 0x1000 - 1) / 0x1000;

    uint8_t perms = PROT_READ;
    if (program_header->p_flags & PF_W) perms |= PROT_WRITE;
    if ((program_header->p_flags & PF_X)) perms |= PROT_EXEC;

    void * ptr = vmarea_mmap(process, (void*)vaddr, total_pages * 0x1000, perms, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0, 1);   
    if (ptr == NULL || ptr != (void *)vaddr) {
        panic("allocate_segment: Failed to allocate memory for segment");
        return FAILURE;
    }

    //kprintf("allocate_segment: Mapped segment at vaddr: %p, size: %llu bytes (%llu pages), perms: 0x%x\n", (void *)vaddr, total_pages * 0x1000, total_pages, perms);

    void * identity = to_kident((vmm_root_t *)process->vmm, ptr);
    if (identity == NULL) {
        panic("allocate_segment: Failed to get identity mapped address");
        return FAILURE;
    }
    //kprintf("Identity mapped address: %p\n", identity);
    //Zero the buffer
    memset((uint8_t *)identity, 0, total_pages * 0x1000);
    //Copy file data
    memcpy((uint8_t *)identity + vaddr_offset, elf_datab + program_header->p_offset, program_header->p_filesz);
    //kprintf("Copied %llu bytes to segment at offset %llu\n", program_header->p_filesz, vaddr_offset);
    //kprintf("Finished ALLOCATE SEGMENT\n");
    return SUCCESS;
}

loaded_elf_t* elf_load_elf(process_t * process, const char * filename, thread_t* thread) {
    vfs_file_descriptor_t fd;
    status_t st = vfs_open(filename, 0, &fd);
    if(st != SUCCESS || !fd.valid) {
        return NULL;
    }

    vfs_stat_t stat_buf;
    if (vfs_fstat(&fd, &stat_buf) != SUCCESS) {
        //panic("elf_load_elf: Failed to stat ELF file %s\n", filename);
        vfs_close(&fd);
        return NULL;
    }

    size_t file_size = stat_buf.st_size;
    uint8_t * elf_datab = (uint8_t *)kmalloc(file_size);
    if (!elf_datab) {
        panic("elf_load_elf: Failed to allocate memory for ELF file %s\n", filename);
        vfs_close(&fd);
        return NULL;
    }

    ssize_t bytes_read = vfs_read(&fd, elf_datab, file_size);
    if ((size_t)bytes_read != file_size) {
        //panic("elf_load_elf: Failed to read complete ELF file %s\n", filename);
        kfree(elf_datab);
        vfs_close(&fd);
        return NULL;
    }

    vfs_close(&fd);
    if (parse_elf_file(elf_datab) != SUCCESS) {
        kfree(elf_datab);
        return NULL;
    }

    Elf64_Ehdr * elf_header = (Elf64_Ehdr *)elf_datab;
    if (elf_header->e_ident[EI_CLASS] != ELFCLASS64) {
        //panic("elf_load_elf: Only ELF64 files are supported\n");
        kfree(elf_datab);
        return NULL;
    }

    //kprintf("ELF Header:\n");
    //kprintf("  Magic: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n", elf_header->e_ident[0], elf_header->e_ident[1], elf_header->e_ident[2], elf_header->e_ident[3], elf_header->e_ident[4], elf_header->e_ident[5], elf_header->e_ident[6], elf_header->e_ident[7], elf_header->e_ident[8], elf_header->e_ident[9], elf_header->e_ident[10], elf_header->e_ident[11], elf_header->e_ident[12], elf_header->e_ident[13], elf_header->e_ident[14], elf_header->e_ident[15]);
    //kprintf("  Class: %s\n", elf_class[elf_header->e_ident[EI_CLASS]]);
    //kprintf("  Data: %s\n", elf_data[elf_header->e_ident[EI_DATA]]);
    //kprintf("  Version: %s\n", elf_version[elf_header->e_ident[EI_VERSION]]);
    //kprintf("  OS/ABI: %s\n", elf_osabi[elf_header->e_ident[EI_OSABI]]);
    //kprintf("  ABI Version: %d\n", elf_header->e_ident[EI_ABIVERSION]);
    //kprintf("  Type: %s\n", elf_type[elf_header->e_type]);
    //kprintf("  Machine: %s\n", elf_machine[elf_header->e_machine]);
    //kprintf("  Version: 0x%x\n", elf_header->e_version);
    //kprintf("  Entry point address: 0x%x\n", elf_header->e_entry);
    //kprintf("  Start of program headers: %d (bytes into file)\n", elf_header->e_phoff);
    //kprintf("  Start of section headers: %d (bytes into file)\n", elf_header->e_shoff);
    //kprintf("  Flags: 0x%x\n", elf_header->e_flags);
    //kprintf("  Size of this header: %d (bytes)\n", elf_header->e_ehsize);
    //kprintf("  Size of program headers: %d (bytes)\n", elf_header->e_phentsize);
    //kprintf("  Number of program headers: %d\n", elf_header->e_phnum);
    //kprintf("  Size of section headers: %d (bytes)\n", elf_header->e_shentsize);
    //kprintf("  Number of section headers: %d\n", elf_header->e_shnum);
    //kprintf("  Section header string table index: %d\n", elf_header->e_shstrndx);

    if (elf_header->e_type == ET_DYN) {
        kfree(elf_datab);
        //panic("Dynamic ELF not supported\n");
        return NULL;
    }

    if (elf_header->e_type != ET_EXEC) {
        kfree(elf_datab);
        //panic("Invalid ELF type\n");
        return NULL;
    }

    vmarea_remove_all(process);
    
    //Iterate all threads and remove them
    for (int i = 0; i < MAX_THREADS_PER_PROCESS; i++) {
        thread_t * t = process->threads[i];
        if (t && t != thread) {
            process_destroy_thread(process, t);
            process->threads[i] = NULL;
        }
    }

    Elf64_Phdr * program_header = (Elf64_Phdr *) (elf_datab + elf_header->e_phoff);
    struct proc_ld pld = {0};

    for (int i = 0; i < elf_header->e_phnum; i++) {
        if (program_header[i].p_type == PT_LOAD) {
            if (allocate_segment(process, elf_datab, &program_header[i], 0) != SUCCESS) {
                panic("elf_load_elf: Failed to allocate segment\n");
                kfree(elf_datab);
                return NULL;
            }
        } else if (program_header[i].p_type == PT_PHDR) {
            pld.at_phdr = (void*)(program_header[i].p_vaddr);
        } else if (program_header[i].p_type == PT_INTERP) {
            if (pld.ld_path) {
                panic("Multiple INTERP segments found\n");
                return NULL;
            }

            if (program_header[i].p_filesz > 0x1000) {
                panic("INTERP path too long\n");
                return NULL;
            }

            pld.ld_path = kmalloc(program_header[i].p_filesz);
            memcpy(pld.ld_path, elf_datab + program_header[i].p_offset, program_header[i].p_filesz);
        }
    }

    //Load signal trampoline
    if (allocate_signal_trampoline(process) != SUCCESS) {
        panic("elf_load_elf: Failed to allocate signal trampoline\n");
        kfree(elf_datab);
        return NULL;
    }

    struct auxv * vectors = kmalloc(sizeof(struct auxv) * 7);
    if (!vectors) {
        panic("elf_load_elf: Failed to allocate memory for auxiliary vectors\n");
        kfree(elf_datab);
        return NULL;
    }
    memset(vectors, 0, sizeof(struct auxv) * 7);
    vectors[0].a_type = AT_ENTRY;
    vectors[0].a_val = (void*)elf_header->e_entry;
    vectors[1].a_type = AT_PHDR;
    vectors[1].a_val = (void*)pld.at_phdr;
    vectors[2].a_type = AT_PHENT;
    vectors[2].a_val = (void*)(uint64_t)elf_header->e_phentsize;
    vectors[3].a_type = AT_PHNUM;
    vectors[3].a_val = (void*)(uint64_t)elf_header->e_phnum;
    vectors[4].a_type = AT_BASE;
    vectors[4].a_val = (void*)(uint64_t)DYNAMIC_LINKER_BASE_ADDRESS;
    vectors[5].a_type = AT_PAGESZ;
    vectors[5].a_val = (void*)(uint64_t)0x1000;
    vectors[6].a_type = AT_SYSINFO_EHDR;
    vectors[6].a_val = (void*)(uint64_t)VDSO_BASE_ADDRESS;
    vectors[7].a_type = AT_NULL;
    vectors[7].a_val = 0;

    if (pld.ld_path) {
        panic("Dynamic linker not supported yet\n");
        return NULL;
    } else {

        loaded_elf_t * ld = kmalloc(sizeof(loaded_elf_t));
        if (!ld) {
            panic("Could not allocate loaded elf\n");
            return NULL;
        }

        memset(ld, 0, sizeof(loaded_elf_t));
        ld->ehdr = elf_header;
        ld->auxv = vectors;
        ld->auxv_size = sizeof(struct auxv) * 8;
        ld->ld = &pld;
        ld->ld_size = sizeof(struct proc_ld);
        return ld;
    }
}