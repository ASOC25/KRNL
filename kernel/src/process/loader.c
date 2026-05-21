#include <krnl/process/loader.h>
#include <krnl/libraries/std/string.h>
#include <krnl/libraries/std/stddef.h>
#include <krnl/libraries/std/stdint.h>
#include <krnl/mem/allocator.h>
#include <krnl/libraries/std/elf.h>
#include <krnl/mem/mmap.h>
#include <krnl/vfs/vfs.h>
#include <krnl/process/process.h>
#include <krnl/debug/perf.h>

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

void * loader_create_args(void * stack, void * user_stack_top, uint64_t max_size, char ** argv, char ** envp, struct auxv* auxv) {
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

    uint64_t * argv_pointers = (uint64_t *)kmalloc((argc + 1) * sizeof(uint64_t));
    uint64_t * envp_pointers = (uint64_t *)kmalloc((envc + 1) * sizeof(uint64_t));
    uint64_t * ptr_buffer    = (uint64_t *)kmalloc(max_size);
    if (!argv_pointers || !envp_pointers || !ptr_buffer) {
        kfree(argv_pointers);
        kfree(envp_pointers);
        kfree(ptr_buffer);
        panic("loader_create_args: Failed to allocate staging buffers");
    }
    uint64_t * ptr = ptr_buffer;
    uint64_t original_addr = (uint64_t)ptr;
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

    char * ascii_ptr = (char *)ptr;

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

    /* Round size up to 16-byte boundary so that (stack - size) is 16-byte
     * aligned at process entry, as required by the SysV AMD64 ABI. */
    uint64_t size = (uint64_t)ascii_ptr - original_addr;
    size = (size + 15) & ~15ULL;
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
            argv_ptr[i] += ((uint64_t)((uint8_t*)user_stack_top - size));
        } else {
            argv_ptr[i] = NULL;
        }
    }
    char  ** envp_ptr = (char **)(stack_ptr + sizeof(uint64_t) + (argc + 1) * sizeof(uint64_t));
    for (int i = 0; i < envc; i++) {
        if (envp_ptr[i] != NULL) {
            envp_ptr[i] += ((uint64_t)((uint8_t*)user_stack_top - size));
        } else {
            envp_ptr[i] = NULL;
        }
    }
    
    kfree(argv_pointers);
    kfree(envp_pointers);
    kfree(ptr_buffer);
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

status_t allocate_vdso(process_t* process) {
    void * addr = vmarea_mmap(process, VDSO_USER_ADDRESS, 0x1000, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0, 1);
    if (addr == NULL || addr != VDSO_USER_ADDRESS) {
        panic("allocate_vdso: Failed to allocate VDSO page");
        return FAILURE;
    }

    void * identity = to_kident((vmm_root_t *)process->vmm, addr);
    if (identity == NULL) {
        panic("allocate_vdso: Failed to get identity mapped address for VDSO");
        return FAILURE;
    }
    memset(identity, 0, 0x1000);

    /* vdso_t lives at the start of the page */
    vdso_t * hdr = (vdso_t *)identity;
    /* vdso_entry_t for the signal trampoline follows immediately */
    vdso_entry_t * tramp = (vdso_entry_t *)((uint8_t *)identity + sizeof(vdso_t));

    tramp->id   = VDSO_ENTRY_SIGNAL_TRAMP;
    tramp->info = SIGNAL_TRAMPOLINE_ADDRESS;
    tramp->size = 0x1000;
    tramp->next = NULL;

    hdr->pd            = NULL;
    hdr->base_addresss = (uint64_t)VDSO_USER_ADDRESS;
    hdr->size          = 0x1000;
    /* entry pointer must be the user-space address of the entry */
    hdr->entry = (vdso_entry_t *)((uint8_t *)VDSO_USER_ADDRESS + sizeof(vdso_t));

    return SUCCESS;
}

status_t allocate_segment(process_t* process, uint8_t * elf_datab, Elf64_Phdr * program_header, void* base) {
    //kprintf("Starting ALLOCATE SEGMENT\n");
    if (program_header->p_type != PT_LOAD) return SUCCESS; //Not all program headers need to be loaded, only PT_LOAD

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

// Extract STT_FUNC symbols from an in-memory ELF, returning a heap-allocated
// proc_symtab_t or NULL. Prefers SHT_SYMTAB over SHT_DYNSYM.
// load_base is added at resolution time (0 for ET_EXEC, actual base for SO).
proc_symtab_t * extract_elf_symtab(uint8_t * elf_data, size_t file_size, uint64_t load_base) {
    if (!elf_data || file_size < sizeof(Elf64_Ehdr)) return NULL;
    Elf64_Ehdr * eh = (Elf64_Ehdr *)elf_data;
    if (!eh->e_shoff || !eh->e_shnum) return NULL;
    if (eh->e_shoff + (uint64_t)eh->e_shnum * sizeof(Elf64_Shdr) > file_size) return NULL;

    Elf64_Shdr * shdrs = (Elf64_Shdr *)(elf_data + eh->e_shoff);

    // Prefer full .symtab; fall back to .dynsym
    Elf64_Shdr * sym_sh = NULL;
    Elf64_Shdr * str_sh = NULL;
    for (int pass = 0; pass < 2 && !sym_sh; pass++) {
        uint32_t want = pass == 0 ? SHT_SYMTAB : SHT_DYNSYM;
        for (int i = 0; i < eh->e_shnum; i++) {
            if (shdrs[i].sh_type != want) continue;
            uint32_t li = shdrs[i].sh_link;
            if (li >= eh->e_shnum) continue;
            if (shdrs[i].sh_offset + shdrs[i].sh_size > file_size) continue;
            if (shdrs[li].sh_offset + shdrs[li].sh_size > file_size) continue;
            sym_sh = &shdrs[i];
            str_sh = &shdrs[li];
            break;
        }
    }
    if (!sym_sh) return NULL;

    uint64_t raw_count = sym_sh->sh_size / sizeof(Elf64_Sym);
    Elf64_Sym * raw = (Elf64_Sym *)(elf_data + sym_sh->sh_offset);

    uint64_t func_count = 0;
    for (uint64_t k = 0; k < raw_count; k++) {
        if (ELF64_ST_TYPE(raw[k].st_info) == STT_FUNC &&
            raw[k].st_value != 0 && raw[k].st_size != 0)
            func_count++;
    }
    if (func_count == 0) return NULL;

    Elf64_Sym * syms    = kmalloc(func_count * sizeof(Elf64_Sym));
    char      * strtab  = kmalloc(str_sh->sh_size);
    proc_symtab_t * st  = kmalloc(sizeof(proc_symtab_t));
    if (!syms || !strtab || !st) {
        if (syms)   kfree(syms);
        if (strtab) kfree(strtab);
        if (st)     kfree(st);
        return NULL;
    }

    uint64_t j = 0;
    for (uint64_t k = 0; k < raw_count; k++) {
        if (ELF64_ST_TYPE(raw[k].st_info) == STT_FUNC &&
            raw[k].st_value != 0 && raw[k].st_size != 0)
            syms[j++] = raw[k];
    }
    memcpy(strtab, elf_data + str_sh->sh_offset, str_sh->sh_size);

    st->syms      = syms;
    st->strtab    = strtab;
    st->count     = func_count;
    st->load_base = load_base;
    st->next      = NULL;
    return st;
}

//Load dynamic linker at DYNAMIC_LINKER_BASE
uint64_t load_dynamic_linker(process_t* process, char* dynamic_linker_path) {
    vfs_file_descriptor_t fd;
    status_t st = vfs_open(dynamic_linker_path, 0, &fd);
    if(st != SUCCESS || !fd.valid) {
        return 0;
    }
    vfs_stat_t stat_buf;
    if (vfs_fstat(&fd, &stat_buf) != SUCCESS) {
        vfs_close(&fd);
        return 0;
    }

    size_t file_size = stat_buf.st_size;
    uint8_t * elf_datab = (uint8_t *)kmalloc(file_size);
    if (!elf_datab) {
        vfs_close(&fd);
        return 0;
    }

    ssize_t bytes_read = vfs_read(&fd, elf_datab, file_size);
    if ((size_t)bytes_read != file_size) {
        kfree(elf_datab);
        vfs_close(&fd);
        return 0;
    }

    vfs_close(&fd);

    if (parse_elf_file(elf_datab) != SUCCESS) {
        kfree(elf_datab);
        return 0;
    }

    Elf64_Ehdr * elf_header = (Elf64_Ehdr *)elf_datab;
    if (elf_header->e_ident[EI_CLASS] != ELFCLASS64) {
        kfree(elf_datab);
        return 0;
    }
    
    if (elf_header->e_type != ET_DYN) {
        kfree(elf_datab);
        return 0;
    }

    for (int i = 0; i < elf_header->e_phnum; i++) {
        Elf64_Phdr * program_header = (Elf64_Phdr *)(elf_datab + elf_header->e_phoff + i * elf_header->e_phentsize);
        if (allocate_segment(process, elf_datab, program_header, (void *)DYNAMIC_LINKER_BASE_ADDRESS) != SUCCESS) {
            kfree(elf_datab);
            return 0;
        }
    }
    uint64_t entry_point = (uint64_t)elf_header->e_entry + DYNAMIC_LINKER_BASE_ADDRESS;

    // Extract ld.so symbols and locate _r_debug for lazy shlib loading
    proc_symtab_t * ld_st = extract_elf_symtab(elf_datab, file_size,
                                                (uint64_t)DYNAMIC_LINKER_BASE_ADDRESS);
    if (ld_st) {
        ld_st->next = process->symtab_list;
        process->symtab_list = ld_st;
    }

    kfree(elf_datab);
    return entry_point;
}

loaded_elf_t* elf_load_elf(process_t * process, const char * filename, thread_t* thread) {
    PERF_BEGIN(t_open);
    vfs_file_descriptor_t fd;
    status_t st = vfs_open(filename, 0, &fd);
    if(st != SUCCESS || !fd.valid) {
        return NULL;
    }

    vfs_stat_t stat_buf;
    if (vfs_fstat(&fd, &stat_buf) != SUCCESS) {
        vfs_close(&fd);
        return NULL;
    }
    PERF_END(t_open, "    elf_load/vfs-open+stat");

    size_t file_size = stat_buf.st_size;
    uint8_t * elf_datab = (uint8_t *)kmalloc(file_size);
    if (!elf_datab) {
        panic("elf_load_elf: Failed to allocate memory for ELF file %s\n", filename);
        vfs_close(&fd);
        return NULL;
    }

    PERF_BEGIN(t_read);
    ssize_t bytes_read = vfs_read(&fd, elf_datab, file_size);
    PERF_END(t_read, "    elf_load/vfs-read");
    if ((size_t)bytes_read != file_size) {
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
        panic("elf_load_elf: Only ELF64 files are supported\n");
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
        panic("HEY!!!Dynamic ELF not supported\n");
        return NULL;
    }

    if (elf_header->e_type != ET_EXEC) {
        kfree(elf_datab);
        panic("Invalid ELF type\n");
        return NULL;
    }

    PERF_BEGIN(t_teardown);
    vmarea_remove_all(process);

    //Iterate all threads and remove them
    for (int i = 0; i < MAX_THREADS_PER_PROCESS; i++) {
        thread_t * t = process->threads[i];
        if (t && t != thread) {
            process_destroy_thread(process, t);
            process->threads[i] = NULL;
        }
    }
    PERF_END(t_teardown, "    elf_load/vmarea-remove-all");

    Elf64_Phdr * program_header = (Elf64_Phdr *) (elf_datab + elf_header->e_phoff);
    struct proc_ld pld = {0};
    Elf64_Phdr * first_load = NULL;

    PERF_BEGIN(t_segs);
    for (int i = 0; i < elf_header->e_phnum; i++) {
        if (program_header[i].p_type == PT_LOAD) {
            if (!first_load) first_load = &program_header[i];
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
    PERF_END(t_segs, "    elf_load/load-segments");

    if (!pld.at_phdr && first_load) {
        pld.at_phdr = (void *)(first_load->p_vaddr + elf_header->e_phoff - first_load->p_offset);
    }

    PERF_BEGIN(t_tramp);
    if (allocate_signal_trampoline(process) != SUCCESS) {
        panic("elf_load_elf: Failed to allocate signal trampoline\n");
        kfree(elf_datab);
        return NULL;
    }

    if (allocate_vdso(process) != SUCCESS) {
        panic("elf_load_elf: Failed to allocate VDSO\n");
        kfree(elf_datab);
        return NULL;
    }
    PERF_END(t_tramp, "    elf_load/trampoline+vdso");

    struct auxv * vectors = kmalloc(sizeof(struct auxv) * 8);
    if (!vectors) {
        panic("elf_load_elf: Failed to allocate memory for auxiliary vectors\n");
        kfree(elf_datab);
        return NULL;
    }

    loaded_elf_t * ld = kmalloc(sizeof(loaded_elf_t));
    if (!ld) {
        panic("Could not allocate loaded elf\n");
        kfree(elf_datab);
        kfree(vectors);
        kfree(ld);
        return NULL;
    }
    memset(ld, 0, sizeof(loaded_elf_t));

    if (pld.ld_path) {
        PERF_BEGIN(t_dynld);
        ld->entry = load_dynamic_linker(process, pld.ld_path);
        PERF_END(t_dynld, "    elf_load/load-dynamic-linker");
        if (ld->entry == 0) {
            panic("elf_load_elf: Failed to load dynamic linker\n");
            kfree(elf_datab);
            kfree(vectors);
            kfree(ld);
            return NULL;
        }
    } else {
        ld->entry = (void*)elf_header->e_entry;
    }

    memset(vectors, 0, sizeof(struct auxv) * 8);
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
    vectors[6].a_val = VDSO_USER_ADDRESS;
    vectors[7].a_type = AT_NULL;
    vectors[7].a_val = 0;

    if (pld.ld_path) kfree(pld.ld_path);

    // Free old symtab list (execve replacing binary) and shlib state
    proc_symtab_t * old = process->symtab_list;
    while (old) {
        proc_symtab_t * next = old->next;
        kfree(old->syms);
        kfree(old->strtab);
        kfree(old);
        old = next;
    }
    process->symtab_list = NULL;
    process->r_debug_va = 0;
    process->shlib_syms_loaded = 0;

    PERF_BEGIN(t_syms);
    proc_symtab_t * exe_st = extract_elf_symtab(elf_datab, file_size, 0);
    if (exe_st) {
        exe_st->next = process->symtab_list;
        process->symtab_list = exe_st;
    }
    PERF_END(t_syms, "    elf_load/extract-symtab");

    // Record the VA of DT_DEBUG's d_ptr field so we can read the r_debug
    // pointer at exception time (ld.so fills it in during startup).
    Elf64_Phdr * phdrs = (Elf64_Phdr *)(elf_datab + elf_header->e_phoff);
    for (int i = 0; i < elf_header->e_phnum; i++) {
        if (phdrs[i].p_type != PT_DYNAMIC) continue;
        size_t dyn_count = phdrs[i].p_filesz / sizeof(Elf64_Dyn);
        Elf64_Dyn * dyns = (Elf64_Dyn *)(elf_datab + phdrs[i].p_offset);
        for (size_t j = 0; j < dyn_count; j++) {
            if (dyns[j].d_tag == DT_DEBUG) {
                // d_un.d_ptr is 8 bytes after d_tag within each Elf64_Dyn entry
                process->r_debug_va = phdrs[i].p_vaddr
                                    + (uint64_t)j * sizeof(Elf64_Dyn)
                                    + sizeof(Elf64_Sxword);
                break;
            }
        }
        break;
    }

    ld->ehdr = NULL;
    kfree(elf_datab);
    ld->auxv = vectors;
    ld->auxv_size = 8;
    ld->ld = NULL;
    ld->ld_size = 0;
    return ld;
}