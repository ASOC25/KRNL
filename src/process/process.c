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
#include <krnl/mem/mmap.h>
#include <krnl/libraries/std/errno.h>
#include <krnl/process/signals.h>

#include <krnl/libraries/assert/assert.h>

extern void set_cpu_fs_base(uint64_t base);

vfs_file_descriptor_t * process_get_fd(process_t *proc, int fd) {
    if (!proc) return NULL;

    if (fd < 0 || fd >= MAX_OPEN_FILES) {

        return NULL;
    }
    vfs_file_descriptor_t *desc = &proc->open_files[fd];
    if (desc->mount == NULL && desc->native_path == NULL) {

        return NULL; // unused slot
    }

    return desc;
}

int process_allocate_fd_slot_locked(process_t *proc) {
    if (!proc) return -1;
    for (int i = 0; i < MAX_OPEN_FILES; ++i) {
        vfs_file_descriptor_t *d = &proc->open_files[i];
        if (d->mount == NULL && d->native_path == NULL) {
            return i;
        }
    }
    return -1;
}

int process_allocate_fd_slot(process_t *proc) {
    if (!proc) return -1;

    int slot = process_allocate_fd_slot_locked(proc);

    return slot;
}

void create_args(process_t * process, const char ** argv, const char ** envp, struct auxv ** out_auxv, uint64_t * out_auxv_size) {
    //Allocate and build in new buffers

    //Copy argv
    int argc = 0;
    while (argv && argv[argc]) {
        argc++;
    }
    char ** new_argv = kmalloc((argc + 1) * sizeof(char *));
    for (int i = 0; i < argc; i++) {
        size_t len = strlen(argv[i]);
        new_argv[i] = kmalloc(len + 1);
        strcpy(new_argv[i], argv[i]);
    }
    new_argv[argc] = NULL;
    //Copy envp
    int envc = 0;
    while (envp && envp[envc]) {
        envc++;
    }
    char ** new_envp = kmalloc((envc + 1) * sizeof(char *));
    for (int i = 0; i < envc; i++) {
        size_t len = strlen(envp[i]);
        new_envp[i] = kmalloc(len + 1);
        strcpy(new_envp[i], envp[i]);
    }
    new_envp[envc] = NULL;
    //Create auxv and copy out_auxv_size entries, include null entries
    struct auxv * new_auxv = NULL;
    uint64_t auxv_size = 0;
    if (out_auxv && out_auxv_size && *out_auxv && *out_auxv_size > 0) {
        auxv_size = *out_auxv_size;
        new_auxv = kmalloc(sizeof(struct auxv) * auxv_size);
        for (uint64_t i = 0; i < auxv_size; i++) {
            new_auxv[i] = (*out_auxv)[i];
        }
    }
    process->argv = new_argv;
    process->envp = new_envp;
    process->auxv = new_auxv;
    process->auxv_size = auxv_size;
}

void process_open_stdfiles(process_t * process, const char * tty) {
    // Open stdin, stdout, stderr to the given tty
    for (int fd = 0; fd < 3; fd++) {
        int slot = process_allocate_fd_slot_locked(process);
        if (slot < 0) {
            panic("process_open_stdfiles: Unable to allocate fd slot");
        }
        vfs_file_descriptor_t newfd;
        status_t st = vfs_open(tty, /*O_RDWR*/ 2, &newfd);
        if (st != SUCCESS || !newfd.valid) {
            panic("process_open_stdfiles: Unable to open tty for stdfile");
        }
        process->open_files[slot] = newfd;
        process->open_file_count++;
    }
}

process_t * process_create(process_t * parent, const char * filename, const char * tty, const char ** argv, const char ** envp) {
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

    loaded_elf_t * elf = elf_load_elf(new_process, filename);
    if (!elf) {
        panic("process_init: Failed to load /init.elf");
    }

    create_args(new_process, argv, envp, &elf->auxv, &elf->auxv_size);

    new_process->binary_entry = (void *)elf->ehdr->e_entry;
    new_process->thread_count = 0;
    new_process->current_thread = NULL;
    new_process->nice = 0xA;
    new_process->state = SCHEDULER_STATUS_RUNABLE;
    new_process->event_queue = NULL;
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

    return new_process;
}

status_t process_init_thread_context(context_t * ctx, vmm_root_t* root, void * pc, void * stack_top, char ** args, thread_t * thread) {
    ctx->cpu_ctx.rip = (uint64_t)pc;
    ctx->cpu_ctx.rsp = (uint64_t)stack_top;
    ctx->cpu_ctx.rflags = 0x202; // Interrupts enabled
    ctx->cpu_ctx.cr3 = (uint64_t)vmm_from_identity_map((uint64_t)root);
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
    memset(ctx->cpu_ctx.ctx_info, 0, sizeof(context_info_t));
    ctx->cpu_ctx.ctx_info->thread = thread;
    ctx->cpu_ctx.ctx_info->cs = ctx->cpu_ctx.cs;
    ctx->cpu_ctx.ctx_info->ss = ctx->cpu_ctx.ss;
    ctx->cpu_ctx.ctx_info->kernel_stack = thread->kstack->top;
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

thread_t * duplicate_thread(process_t * parent, thread_t * og) {
    if (!parent || !og) {
        panic("duplicate_thread: parent or og thread is NULL");
    }

    thread_t * new_thread = kmalloc(sizeof(thread_t));
    if (!new_thread) {
        panic("duplicate_thread: Failed to allocate memory for new thread");
        return NULL;
    }
    memset(new_thread, 0, sizeof(thread_t));
    new_thread->event_queue = NULL;
    new_thread->stack_size = og->stack_size;
    new_thread->kstack = kstackalloc(parent->vmm, KERNEL_STACK_SIZE);

    if (!new_thread->kstack) {
        panic("duplicate_thread: Failed to allocate memory for new kstack");
    }
    memcpy((void*)((uint64_t)new_thread->kstack->base), og->kstack->base, KERNEL_STACK_SIZE);

    new_thread->ustack = copy_stack(parent->vmm, og->ustack);
    if (!new_thread->kstack || !new_thread->ustack) {
        panic("duplicate_thread: Failed to copy stacks for new thread");
        kfree(new_thread);
        return NULL;
    }

    context_info_t * new_ctx_info = kmalloc(sizeof(context_info_t));
    if (!new_ctx_info) {
        panic("duplicate_thread: Failed to allocate memory for context_info_t");
        kfree(new_thread);
        return NULL;
    }
    new_thread->prio = og->prio;

    new_ctx_info->thread = new_thread;
    new_ctx_info->kernel_stack = new_thread->kstack->top;
    new_ctx_info->cs = og->context->cpu_ctx.ctx_info->cs;
    new_ctx_info->ss = og->context->cpu_ctx.ctx_info->ss;

    cpu_context_t * new_cpu_ctx = kmalloc(sizeof(cpu_context_t));
    if (!new_cpu_ctx) {
        panic("duplicate_thread: Failed to allocate memory for cpu_context_t");
        kfree(new_ctx_info);
        kfree(new_thread);
        return NULL;
    }
    memset(new_cpu_ctx, 0, sizeof(cpu_context_t));
    new_cpu_ctx->cr3 = (uint64_t)vmm_from_identity_map((uint64_t)parent->vmm);
    new_cpu_ctx->ctx_info = new_ctx_info;
    new_cpu_ctx->rax = og->context->cpu_ctx.rax;
    new_cpu_ctx->rbx = og->context->cpu_ctx.rbx;
    new_cpu_ctx->rcx = og->context->cpu_ctx.rcx;
    new_cpu_ctx->rdx = og->context->cpu_ctx.rdx;
    new_cpu_ctx->rsi = og->context->cpu_ctx.rsi;
    new_cpu_ctx->rdi = og->context->cpu_ctx.rdi;
    new_cpu_ctx->rbp = og->context->cpu_ctx.rbp;
    new_cpu_ctx->r8 = og->context->cpu_ctx.r8;
    new_cpu_ctx->r9 = og->context->cpu_ctx.r9;
    new_cpu_ctx->r10 = og->context->cpu_ctx.r10;
    new_cpu_ctx->r11 = og->context->cpu_ctx.r11;
    new_cpu_ctx->r12 = og->context->cpu_ctx.r12;
    new_cpu_ctx->r13 = og->context->cpu_ctx.r13;
    new_cpu_ctx->r14 = og->context->cpu_ctx.r14;
    new_cpu_ctx->r15 = og->context->cpu_ctx.r15;

    new_cpu_ctx->interrupt_number = og->context->cpu_ctx.interrupt_number;
    new_cpu_ctx->error_code = og->context->cpu_ctx.error_code;

    new_cpu_ctx->rip = og->context->cpu_ctx.rip;
    new_cpu_ctx->cs = og->context->cpu_ctx.cs;
    new_cpu_ctx->rflags = og->context->cpu_ctx.rflags;
    new_cpu_ctx->rsp = og->context->cpu_ctx.rsp;
    new_cpu_ctx->ss = og->context->cpu_ctx.ss;

    context_t * new_ctx = kmalloc(sizeof(context_t));
    if (!new_ctx) {
        panic("duplicate_thread: Failed to allocate memory for context_t");
        kfree(new_cpu_ctx);
        kfree(new_ctx_info);
        kfree(new_thread);
        return NULL;
    }
    memset(new_ctx, 0, sizeof(context_t));
    new_ctx->cpu_ctx = *new_cpu_ctx;
    new_ctx->fs_base = og->context->fs_base;
    new_ctx->simd_ctx = simd_create_context();
    if (!new_ctx->simd_ctx) {
        panic("duplicate_thread: Failed to allocate memory for SIMD context");
        kfree(new_ctx);
        kfree(new_cpu_ctx);
        kfree(new_ctx_info);
        kfree(new_thread);
        return NULL;
    }
    memcpy(new_ctx->simd_ctx, og->context->simd_ctx, 512);

    new_thread->kcontext = kmalloc(sizeof(context_t));
    if (!new_thread->kcontext) {
        panic("duplicate_thread: Failed to allocate memory for kernel context_t");
        simd_free_context(new_ctx->simd_ctx);
        kfree(new_ctx);
        kfree(new_cpu_ctx);
        kfree(new_ctx_info);
        kfree(new_thread);
        return NULL;
    }
    memset(new_thread->kcontext, 0, sizeof(context_t));
    new_thread->kcontext_pending = 0;
    new_thread->kcontext->simd_ctx = kmalloc(512);
    if (!new_thread->kcontext->simd_ctx) {
        panic("duplicate_thread: Failed to allocate memory for kernel SIMD context");
        kfree(new_thread->kcontext);
        simd_free_context(new_ctx->simd_ctx);
        kfree(new_ctx);
        kfree(new_cpu_ctx);
        kfree(new_ctx_info);
        kfree(new_thread);
        return NULL;
    }
    memset(new_thread->kcontext->simd_ctx, 0, 512);
    
    new_thread->context = new_ctx;
    new_thread->entry = og->entry;
    new_thread->state = og->state;
    new_thread->process = (void *)parent;

    return new_thread;
}

process_t * process_fork(process_t * parent, thread_t * forking_thread) {
    if (!parent || !forking_thread) {
        return NULL;
    }

    kprintf("Process %d is forking thread %p\n", parent->pid, forking_thread);

    process_t * child = kmalloc(sizeof(process_t));
    if (!child) {
        panic("process_fork: Failed to allocate memory for child process");

        return NULL;
    }
    memset(child, 0, sizeof(process_t));
    vmarea_sync(parent);

    if (!parent->vmm) {
        panic("process_fork: Parent process has no VMM");
        kfree(child);

        return NULL;
    } else {
        child->vmm = vmm_duplicate_fullspace(parent->vmm);
        if (!child->vmm) {
            panic("process_fork: Failed to duplicate VMM for child process");
            kfree(child);

            return NULL;
        }
    }

    status_t st = vmarea_fork(child, parent);
    if (st != SUCCESS) {
        panic("process_fork: Failed to duplicate VM areas for child process");
        vmm_free_root(child->vmm);
        kfree(child);

        return NULL;
    }

    //duplicate only forking_thread
    thread_t * child_thread = duplicate_thread(child, forking_thread);
    if (!child_thread) {
        panic("process_fork: Failed to duplicate thread for child process");
        vmarea_remove_all(child);
        vmm_free_root(child->vmm);
        kfree(child);

        return NULL;
    }

    child->threads[0] = *child_thread;
    child->threads[0].context->cpu_ctx.ctx_info->thread = &child->threads[0]; //UTTERLY STUPID
    child->thread_count = 1;
    child->main_thread = &child->threads[0];
    child->current_thread = &child->threads[0];
    child->pid = -1; // Will be set by scheduler
    child->ppid = parent->pid;
    child->uid = parent->uid;
    child->gid = parent->gid;
    child->nice = parent->nice;
    child->event_queue = NULL;
    child->binary_entry = parent->binary_entry;
    child->exit_code = 0;
    child->state = SCHEDULER_STATUS_RUNABLE;
    for (int i = 0; i < parent->open_file_count; i++) {
        child->open_files[i] = parent->open_files[i];
    }
    child->open_file_count = parent->open_file_count;

    create_args(child, (const char **)parent->argv, (const char **)parent->envp, &parent->auxv, &parent->auxv_size);
    child_thread->context->cpu_ctx.rax = 0; // Child process gets 0 return value from fork

    return child;
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
    new_thread->kcontext = kmalloc(sizeof(context_t));
    new_thread->kcontext_pending = 0;
    if (!new_thread->kcontext) {
        panic("process_create_thread: Failed to allocate kernel CPU context");
        return NULL;
    }
    memset(new_thread->kcontext, 0, sizeof(context_t));

    new_thread->context->simd_ctx = simd_create_context();
    if (!new_thread->context->simd_ctx) {
        panic("process_create_thread: Failed to allocate SIMD context");
        return NULL;
    }
    memset(new_thread->context->simd_ctx, 0, 512);

    new_thread->kcontext->simd_ctx = simd_create_context();
    if (!new_thread->kcontext->simd_ctx) {
        panic("process_create_thread: Failed to allocate kernel SIMD context");
        return NULL;
    }
    memset(new_thread->kcontext->simd_ctx, 0, 512);

    new_thread->process = (void *)process;
    new_thread->entry = entry_point;
    new_thread->state = SCHEDULER_STATUS_RUNABLE;
    new_thread->prio = process->nice;
    new_thread->stack_size = NEW_PROCESS_STACK_SIZE; // 16 KB stack
    new_thread->event_queue = NULL;
    new_thread->kstack = kstackalloc(process->vmm, KERNEL_STACK_SIZE);
    if (!new_thread->kstack) {
        panic("context_init: Failed to allocate kernel stack for process");
        return NULL;
    }

    new_thread->ustack = kmalloc(sizeof(farlands_stack_t));
    if (!new_thread->ustack) {
        panic("process_create_thread: Failed to allocate user stack structure");
        return NULL;
    }
    memset(new_thread->ustack, 0, sizeof(farlands_stack_t));
    status_t st = stackalloc(process->vmm, new_thread->stack_size, VMM_REGION_U_STACK - new_thread->stack_size, VMM_WRITE_BIT | VMM_USER_BIT, new_thread->ustack);
    if (st != SUCCESS) {
        panic("process_create_thread: Failed to allocate user stack");
        return NULL;
    }

    uint64_t old_top = (uint64_t)new_thread->ustack->handle_top;
    new_thread->ustack->handle_top = loader_create_args(new_thread->ustack->handle_top, new_thread->stack_size, process->argv, process->envp, process->auxv);
    new_thread->ustack->top -= (old_top - (uint64_t)new_thread->ustack->handle_top);

    if (process->thread_count == 0) {
        process->main_thread = new_thread;
    }

    parse_stack(new_thread->ustack->handle_top);
    process->thread_count++;

    return new_thread;
}

status_t process_execve(process_t * process, const char * filename, const char ** argv, const char ** envp) {
    if (!process) {
        panic("process_execve: process is NULL");
        return FAILURE;
    }

    process->vmm = vmm_duplicate_kspace();
    if (!process->vmm) {
        panic("process_execve: Failed to duplicate kernel space VMM");

        return FAILURE;
    }
    vmarea_remove_all(process);

    loaded_elf_t * elf = elf_load_elf(process, filename);
    if (!elf) {
        panic("process_execve: Failed to load ELF binary");

        return FAILURE;
    }

    create_args(process, argv, envp, &elf->auxv, &elf->auxv_size);

    process->binary_entry = (void *)elf->ehdr->e_entry;

    thread_t * new_thread = process_create_thread(process, process->binary_entry);
    if (!new_thread) {
        panic("process_execve: Failed to create main thread");

        return FAILURE;
    }

    process->main_thread = new_thread;
    process->current_thread = new_thread;
    process->threads[0] = *new_thread;
    process->thread_count = 1;

    status_t st = process_init_thread_context(
        new_thread->context,
        process->vmm,
        process->binary_entry,
        (void *)new_thread->ustack->top,
        process->argv,
        new_thread
    );

    if (st != SUCCESS) {
        panic("process_execve: Failed to initialize main thread context");

    }

    return SUCCESS;
}

status_t process_enqueue_event(thread_t * thread, int event) {
    //Make sure thread is valid and the event is not already in the queue (no duplicates)
    if (!thread) {
        panic("process_enqueue_event: thread is NULL");
        return FAILURE;
    }

    event_queue_t * current = thread->event_queue;
    while (current != NULL) {
        if (current->event == event) {
            //Event already in queue

            return SUCCESS;
        }
        current = current->next;
    }
    event_queue_t * new_event = kmalloc(sizeof(event_queue_t));
    if (!new_event) {
        panic("process_enqueue_event: Failed to allocate memory for new event");

        return FAILURE;
    }
    new_event->event = event;
    new_event->next = NULL;

    if (!thread->event_queue) {
        thread->event_queue = new_event;
    } else {
        event_queue_t * current = thread->event_queue;
        while (current->next != NULL) {
            current = current->next;
        }
        current->next = new_event;
    }

    return SUCCESS;
}

status_t process_dequeue_event(thread_t * thread, int * out_event) {
    //Pop the first event from the thread's event queue
    if (!thread || !out_event) {
        panic("process_dequeue_event: thread or out_event is NULL");
        return FAILURE;
    }


    if (!thread->event_queue) {

        return FAILURE; // No events
    }
    event_queue_t * event_node = thread->event_queue;
    *out_event = event_node->event;
    thread->event_queue = event_node->next;
    kfree(event_node);

    return SUCCESS;
}

void process_set_exit_code(process_t * process, int code) {
    if (!process) {
        panic("process_set_exit_code: process is NULL");
        return;
    }
    process->exit_code = code;
}

status_t process_destroy_thread(process_t * process, thread_t * thread) {
    if (!thread) {
        panic("process_destroy_thread: thread is NULL");
        return FAILURE;
    }
    
    if (thread->context) {
        simd_free_context(thread->context->simd_ctx);
        kfree(thread->context->cpu_ctx.ctx_info);
        kfree(thread->context);
    }

    if (thread->kcontext) {
        simd_free_context(thread->kcontext->simd_ctx);
        kfree(thread->kcontext->cpu_ctx.ctx_info);
        kfree(thread->kcontext);
    }

    if (thread->ustack) {
        stackfree(process->vmm, thread->ustack);
    }
    if (thread->kstack) {
        kstackfree(thread->kstack);
    }

    return SUCCESS;
}

status_t process_destroy(process_t * process) {
    if (!process) {
        panic("process_destroy: process is NULL");
        return FAILURE;
    }

    for (int i = 0; i < process->thread_count; i++) {
        process_destroy_thread(process, &process->threads[i]);
    }

    vmarea_remove_all(process);
    vmm_free_root(process->vmm);
    kfree(process);

    return SUCCESS;
}

status_t process_exit(process_t * process, int code) {
    if (!process) {
        panic("process_exit: process is NULL");
        return FAILURE;
    }

    process_set_exit_code(process, code);
    //Change all threads to ZOMBIE
    for (int i = 0; i < process->thread_count; i++) {
        process->threads[i].state = SCHEDULER_STATUS_ZOMBIE;
    }
    process->state = SCHEDULER_STATUS_ZOMBIE;
    wakeup(SIGNAL_WAITPID);
    return SUCCESS;
}

void process_init(const char * INIT_PROCESS, const char * INIT_TTY) {
    assert(INIT_PROCESS != NULL);
    process_t * init_process = process_create(INIT_PROCESS_PARENT_CODE, INIT_PROCESS, INIT_TTY, NULL, NULL);
    if (!init_process) {
        panic("process_init: Failed to create init process");
    }
    thread_t * init_thread = process_create_thread(init_process, init_process->binary_entry);
    if (!init_thread) {
        panic("process_init: Failed to create init thread");
    }

    status_t st = process_init_thread_context(init_thread->context, init_process->vmm, init_thread->entry, (uint8_t *)init_thread->ustack->top, init_process->argv, init_thread);
    if (st != SUCCESS) {
        panic("process_init: Failed to initialize init thread context");
    }

    st = scheduler_add(init_process->main_thread);
    if (st != SUCCESS) {
        panic("process_init: Failed to add init process to scheduler");
    }

}