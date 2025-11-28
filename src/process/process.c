#include <krnl/process/process.h>
#include <krnl/debug/debug.h>
#include <krnl/mem/vmm.h>
#include <krnl/mem/allocator.h>
#include <krnl/process/scheduler.h>
#include <krnl/arch/x86/cpu.h>
#include <krnl/arch/x86/gdt.h>
#include <krnl/libraries/std/stddef.h>
#include <krnl/libraries/std/string.h>
#include <krnl/mem/allocator.h>
#include <krnl/process/loader.h>

extern void set_cpu_fs_base(uint64_t base);

vfs_file_descriptor_t * process_get_fd(process_t *proc, int fd) {
    if (!proc) return NULL;
    if (fd < 0 || fd >= MAX_OPEN_FILES) return NULL;
    vfs_file_descriptor_t *desc = &proc->open_files[fd];
    if (desc->mount == NULL && desc->native_path == NULL) {
        return NULL; // unused slot
    }
    return desc;
}

int process_allocate_fd_slot(process_t *proc) {
    if (!proc) return -1;
    for (int i = 0; i < MAX_OPEN_FILES; ++i) {
        vfs_file_descriptor_t *d = &proc->open_files[i];
        if (d->mount == NULL && d->native_path == NULL) {
            return i;
        }
    }
    return -1;
}

void process_open_stdfiles(process_t * process, const char * tty) {
    // Open stdin, stdout, stderr to the given tty
    for (int fd = 0; fd < 3; fd++) {
        int slot = process_allocate_fd_slot(process);
        if (slot < 0) {
            panic("process_open_stdfiles: Unable to allocate fd slot");
        }
        vfs_file_descriptor_t *newfd = vfs_open(tty, /*O_RDWR*/ 2);
        if (!newfd) {
            panic("process_open_stdfiles: Unable to open tty for stdfile");
        }
        process->open_files[slot] = *newfd;
        process->open_file_count++;
    }
}

process_t * process_create(process_t * parent, const char * filename, const char * tty, char ** argv, char ** envp) {
    if (!parent) {
        return NULL;
    }

    process_t * new_process = kmalloc(sizeof(process_t));
    if (!new_process) {
        panic("Failed to allocate memory for new process");
        return NULL;
    }
    memset(new_process, 0, sizeof(process_t));

    if (parent == INIT_PROCESS_PARENT_CODE) {
        new_process->vmm = vmm_duplicate_kspace();
    } else {
        new_process->vmm = vmm_duplicate_fullspace(parent->vmm);
    }

    if (!new_process->vmm) {
        panic("Failed to duplicate VMM for new process");
        return NULL;
    }

    loaded_elf_t * elf = elf_load_elf(new_process->vmm, filename);
    if (!elf) {
        panic("process_init: Failed to load /init.elf");
    }
    new_process->auxv = elf->auxv;
    new_process->auxv_size = elf->auxv_size;
    new_process->argv = argv;
    new_process->envp = envp;
    new_process->binary_entry = (void *)elf->ehdr->e_entry;
    new_process->thread_count = 0;
    new_process->current_thread = NULL;
    new_process->nice = 0xA;
    new_process->current_nice = 0xA;
    new_process->main_thread = NULL;
    new_process->pid = -1; // Will be set by scheduler


    if (parent == INIT_PROCESS_PARENT_CODE) {
        new_process->ppid = -1;
        new_process->uid = 0;
        new_process->gid = 0;
        for (int i = 0; i < MAX_OPEN_FILES; i++) {
            new_process->open_files[i] = (vfs_file_descriptor_t){0};
        }
        new_process->open_file_count = 0;
        new_process->parent = NULL;
        process_open_stdfiles(new_process, tty);
    } else {
        new_process->ppid = parent->pid;
        new_process->uid = parent->uid;
        new_process->gid = parent->gid;
        for (int i = 0; i < parent->open_file_count; i++) {
            new_process->open_files[i] = parent->open_files[i];
        }
        new_process->open_file_count = parent->open_file_count;
        new_process->parent = parent;
    }

    new_process->argv = argv;
    new_process->envp = envp;
    //Auxv
    return new_process;
}

status_t process_thread_context_init(context_t * ctx, vmm_root* root, void * pc, void * stack_top, char ** args, thread_t * thread) {
    ctx->cpu_ctx.rip = (uint64_t)pc;
    ctx->cpu_ctx.rsp = (uint64_t)stack_top;
    ctx->cpu_ctx.rflags = 0x202; // Interrupts enabled
    ctx->cpu_ctx.cr3 = (uint64_t)vmm_from_identity_map(root);
    if (!ctx->cpu_ctx.cr3) {
        panic("context_init: Failed to get CR3 from VMM root");
        return FAILURE;
    }
    ctx->cpu_ctx.rflags = RFLAGS_INTERRUPT_ENABLE | RFLAGS_ONE;
    if (args != NULL) {
        ctx->cpu_ctx.rdi = (uint64_t)args[0]; // argv
        ctx->cpu_ctx.rsi = (uint64_t)args[1];
        ctx->cpu_ctx.rdx = (uint64_t)args[2];
        ctx->cpu_ctx.rcx = (uint64_t)args[3];
        ctx->cpu_ctx.r8 = (uint64_t)args[4];
        ctx->cpu_ctx.r9 = (uint64_t)args[5];
    }

    ctx->cpu_ctx.cs = GDT_USER_CODE * sizeof(gdt_entry_t) | 0x3;
    ctx->cpu_ctx.ss = GDT_USER_DATA * sizeof(gdt_entry_t) | 0x3;

    ctx->cpu_ctx.ctx_info = (context_info_t*)kmalloc(sizeof(context_info_t));
    if (!ctx->cpu_ctx.ctx_info) {
        panic("context_init: Failed to allocate context_info_t");
        return FAILURE;
    }
    ctx->cpu_ctx.ctx_info->thread = thread;
    ctx->cpu_ctx.ctx_info->cs = ctx->cpu_ctx.cs;
    ctx->cpu_ctx.ctx_info->ss = ctx->cpu_ctx.ss;
    ctx->cpu_ctx.ctx_info->kernel_stack = kstackalloc(KERNEL_STACK_SIZE);
    if (!ctx->cpu_ctx.ctx_info->kernel_stack) {
        panic("context_init: Failed to allocate kernel stack");
        return FAILURE;
    }

    ctx->fs_base = 0; //Set later if needed
    return SUCCESS;
}

void* simd_create_context(void) {
    return kmalloc(512);
}

void simd_free_context(void* ctx) {
    kfree(ctx);
}

void simd_save_context(void* ctx) {
    __asm__ volatile("fxsave (%0) "::"r"(ctx));
}

void simd_restore_context(void* ctx) {
    __asm__ volatile("fxrstor (%0) "::"r"(ctx));
}

void context_save(context_t* ctx, cpu_context_t* cpu_ctx){
    simd_save_context(ctx->simd_ctx);
    memcpy(&ctx->cpu_ctx, cpu_ctx, sizeof(cpu_context_t));
}

void context_restore(context_t* ctx, cpu_context_t* cpu_ctx){
    simd_restore_context(ctx->simd_ctx);
    set_cpu_fs_base(ctx->fs_base);
    memcpy(cpu_ctx, &ctx->cpu_ctx, sizeof(cpu_context_t));
}

void parse_stack(void * stack) {
    //Print the argc, argv, envp, auxv from the stack
    size_t * pointer_table = (size_t *)stack;
    size_t argc = *pointer_table++;
    kprintf("argc: %zu\n", argc);
    char ** argv = (char **)pointer_table;
    for (size_t i = 0; i < argc; i++) {
        if (argv[i] == NULL) {
            kprintf("argv[%zu] at %p points to NULL\n", i, (void *)&argv[i]);
        } else {
            kprintf("argv[%zu] at %p points to: %p, value: %s\n", i, (void *)&argv[i], (void *)argv[i], argv[i]);
        }
    }
    pointer_table += argc + 1; // Move past argv pointers
    char ** envp = (char **)pointer_table;
    size_t envp_count = 0;
    while (envp[envp_count] != NULL) {
        kprintf("envp[%zu] at %p points to: %p, value: %s\n", envp_count, (void *)&envp[envp_count], (void *)envp[envp_count], envp[envp_count]);
        envp_count++;
    }
    pointer_table += envp_count + 1; // Move past envp pointers
    struct auxv * auxv = (struct auxv *)pointer_table;
    size_t auxv_count = 0;
    while (auxv[auxv_count].a_type != AT_NULL) {
        kprintf("auxv[%zu]: type: %llu, value: %p\n", auxv_count, auxv[auxv_count].a_type, auxv[auxv_count].a_val);
        auxv_count++;
    }
    kprintf("End of auxv\n");
    kprintf("End of stack parsing\n");
}

thread_t * process_create_thread(process_t * process, void * entry_point) {
    if (!process) panic("process_create_thread: process is NULL");

    thread_t * new_thread = &process->threads[process->thread_count];
    if (process->thread_count >= MAX_THREADS_PER_PROCESS) {
        panic("process_create_thread: Maximum thread count reached");
        return NULL;
    }
    memset(new_thread, 0, sizeof(thread_t));

    new_thread->context = kmalloc(sizeof(context_t));
    if (!new_thread->context) {
        panic("process_create_thread: Failed to allocate CPU context");
        return NULL;
    }
    memset(new_thread->context, 0, sizeof(context_t));

    new_thread->context->simd_ctx = simd_create_context();
    if (!new_thread->context->simd_ctx) {
        panic("process_create_thread: Failed to allocate SIMD context");
        return NULL;
    }
    memset(new_thread->context->simd_ctx, 0, 512);
    new_thread->process = (void *)process;
    new_thread->entry = entry_point;
    new_thread->stack_size = NEW_PROCESS_STACK_SIZE; // 16 KB stack

    farlands_stack_t farlands;

    status_t st = kstackalloc_farlands(process->vmm, new_thread->stack_size, VMM_REGION_U_STACK, VMM_WRITE_BIT | VMM_USER_BIT, &farlands);
    if (st != SUCCESS) {
        panic("process_create_thread: Failed to allocate user stack");
        return NULL;
    }
    memset((void *)farlands.handle_base, 0, new_thread->stack_size);
    
    farlands.handle_top = loader_create_args(farlands.handle_top, new_thread->stack_size, process->argv, process->envp, process->auxv);
    farlands.top -= ((uint64_t)farlands.handle_top - (uint64_t)(farlands.handle_base));

    new_thread->stack = kmalloc(sizeof(stack_t));
    if (!new_thread->stack) {
        panic("process_create_thread: Failed to allocate stack_t");
        return NULL;
    }
    new_thread->stack->top = farlands.top;
    new_thread->stack->base = farlands.base;
    new_thread->stack->flags = farlands.flags;
    new_thread->stack->guard_size = farlands.guard_size;

    if (process->thread_count == 0) {
        process->main_thread = new_thread;
    }

    parse_stack(farlands.handle_top);

    process->thread_count++;

    return new_thread;
}

void process_set_exit_code(process_t * process, int code) {
    if (!process) {
        panic("process_set_exit_code: process is NULL");
        return;
    }
    process->exit_code = code;
}

status_t process_destroy(process_t * process) {
    if (!process) {
        panic("process_destroy: process is NULL");
        return FAILURE;
    }

    for (int i = 0; i < process->thread_count; i++) {
        process_destroy_thread(&process->threads[i]);
    }

    vmm_free_root(process->vmm);
    kfree(process);
    return SUCCESS;
}

status_t process_destroy_thread(thread_t * thread) {
    if (!thread) {
        panic("process_destroy_thread: thread is NULL");
        return FAILURE;
    }

    if (thread->context) {
        simd_free_context(thread->context->simd_ctx);
        kfree(thread->context);
    }

    if (thread->stack) {
        kstackfree(thread->stack);
    }

    return SUCCESS;
}

void process_init(const char * INIT_PROCESS, const char * INIT_TTY) {
    process_t * init_process = process_create(INIT_PROCESS_PARENT_CODE, INIT_PROCESS, INIT_TTY, NULL, NULL);
    if (!init_process) {
        panic("process_init: Failed to create init process");
    }
    thread_t * init_thread = process_create_thread(init_process, init_process->binary_entry);
    if (!init_thread) {
        panic("process_init: Failed to create init thread");
    }

    status_t st = process_thread_context_init(init_thread->context, init_process->vmm, init_thread->entry, (uint8_t *)init_thread->stack->top, init_process->argv, init_thread);
    if (st != SUCCESS) {
        panic("process_init: Failed to initialize init thread context");
    }

    st = scheduler_add_process(init_process, SCHEDULER_QUEUE_RUNABLE);
    if (st != SUCCESS) {
        panic("process_init: Failed to add init process to scheduler");
    }
}