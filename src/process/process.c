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

void create_args(process_t * process, char ** argv, char ** envp, struct auxv ** out_auxv, uint64_t * out_auxv_size) {
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

uint8_t process_pending_signal(process_t * process) {
    if (!process) {
        return 0;
    }

    for (int sig = 1; sig < NSIG; sig++) {
        if (process->signal_queue[sig] != NULL) {
            return 1;
        }
    }

    return 0;
}

process_t * process_create(process_t * parent, const char * filename, const char * tty, vfs_path_t root, vfs_path_t cwd, const char ** argv, const char ** envp) {
    if (!parent) {
        return NULL;
    }

    process_t * new_process = kmalloc(sizeof(process_t));
    //kprintf("process_create: Creating process for %s\n", filename);
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

    loaded_elf_t * elf = elf_load_elf(new_process, filename, 0);
    if (!elf) {
        panic("process_init: Failed to load /init.elf");
    }

    create_args(new_process, (char**)argv, (char**)envp, &elf->auxv, &elf->auxv_size);
    memset(new_process->threads, 0, MAX_THREADS_PER_PROCESS * sizeof(thread_t *));
    new_process->binary_entry = (void *)elf->ehdr->e_entry;
    new_process->thread_count = 0;
    new_process->current_thread = NULL;
    new_process->nice = 0xA;
    new_process->state = SCHEDULER_STATUS_RUNABLE;
    new_process->stramp_address = (void *)SIGNAL_TRAMPOLINE_ADDRESS;
    new_process->main_thread = NULL;
    new_process->pid = -1; // Will be set by scheduler
    new_process->rootdir.mount = root.mount;
    memcpy(new_process->rootdir.internal_path, root.internal_path, VFS_PATH_MAX);
    new_process->cwd.mount = cwd.mount;
    memcpy(new_process->cwd.internal_path, cwd.internal_path, VFS_PATH_MAX);

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
    //kprintf("context_init: Created context_info_t at %p\n", ctx->cpu_ctx.ctx_info);
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

//Save cpu_ctx in ctx
void context_save(context_t* ctx, cpu_context_t* cpu_ctx){
    context_info_t * old_info = ctx->cpu_ctx.ctx_info;
    memcpy(old_info, cpu_ctx->ctx_info, sizeof(context_info_t));
    simd_save_context(ctx->simd_ctx);
    memcpy(&ctx->cpu_ctx, cpu_ctx, sizeof(cpu_context_t));
    ctx->cpu_ctx.ctx_info = old_info;
}

void context_restore(context_t* ctx, cpu_context_t* cpu_ctx){
    context_info_t * old_info = cpu_ctx->ctx_info;
    memcpy(old_info, ctx->cpu_ctx.ctx_info, sizeof(context_info_t));
    simd_restore_context(ctx->simd_ctx);
    set_cpu_fs_base(ctx->fs_base);
    memcpy(cpu_ctx, &ctx->cpu_ctx, sizeof(cpu_context_t));
    cpu_ctx->ctx_info = old_info;
}

status_t process_handle_default_signal(thread_t * thread, signal_t * signal) {
    if (!thread || !signal) {
        panic("process_handle_default_signal: thread or signal is NULL");
        return FAILURE;
    }

    //kprintf("process_handle_default_signal: Handling default signal %d for process %d\n", signal->signo, ((process_t *)thread->process)->pid);
    switch (signal->signo) {
        case SIGKILL:
        case SIGTERM:
            //Terminate the process
            //kprintf("process_handle_default_signal: Terminating process %d due to signal %d\n", ((process_t *)thread->process)->pid, signal->signo);
            process_exit((process_t *)thread->process, 128 + signal->signo);
            return SUCCESS;
        default:
            //kprintf("process_handle_default_signal: Unhandled default signal %d for process %d\n", signal->signo, ((process_t *)thread->process)->pid);
            return FAILURE;
    }
}

status_t process_create_scontext(thread_t * thread, signal_t * signal) {
    if (!thread) {
        panic("process_create_scontext: thread is NULL");
        return FAILURE;
    }

    process_t * process = (process_t *)thread->process;
    if (!process) {
        panic("process_create_scontext: thread's process is NULL");
        return FAILURE;
    }

    //If the process does not have a signal handler for this signal, call the default handler right away
    sigaction_t * action = process->signal_actions[signal->signo];
    if (action == NULL)
        return process_handle_default_signal(thread, signal);

    context_t * sig_ctx = kmalloc(sizeof(context_t));
    //kprintf("process_create_scontext: Created SIGNAL context_t for process %d thread %p at %p\n", ((process_t *)thread->process)->pid, thread, sig_ctx);
    if (!sig_ctx) {
        panic("process_create_scontext: Failed to allocate memory for signal context");
        return FAILURE;
    }
    memset(sig_ctx, 0, sizeof(context_t));
    sig_ctx->simd_ctx = simd_create_context();
    if (!sig_ctx->simd_ctx) {
        panic("process_create_scontext: Failed to allocate memory for signal SIMD context");
        kfree(sig_ctx);
        return FAILURE;
    }

    sig_ctx->cpu_ctx.ctx_info = (context_info_t *)kmalloc(sizeof(context_info_t));
    if (!sig_ctx->cpu_ctx.ctx_info) {
        panic("process_create_scontext: Failed to allocate memory for signal context_info_t");
        simd_free_context(sig_ctx->simd_ctx);
        kfree(sig_ctx);
        return FAILURE;
    }
    memset(sig_ctx->cpu_ctx.ctx_info, 0, sizeof(context_info_t));
    sigctx_t * new_scontext = kmalloc(sizeof(sigctx_t));
        if (!new_scontext) {
        panic("process_create_scontext: Failed to allocate memory for thread's sigctx_t");
        kfree(sig_ctx->cpu_ctx.ctx_info);
        simd_free_context(sig_ctx->simd_ctx);
        kfree(sig_ctx);
        return FAILURE;
    }

    new_scontext->signal = signal;
    new_scontext->context = sig_ctx;
    new_scontext->stack = stackalloc(process->vmm, thread->stack_size, VMM_REGION_S_STACK - thread->stack_size, VMM_WRITE_BIT | VMM_USER_BIT);
    new_scontext->kstack = kstackalloc(process->vmm, KERNEL_STACK_SIZE);
    if (!new_scontext->stack || !new_scontext->kstack) {
        panic("process_create_scontext: Failed to allocate signal stack");
        kfree(new_scontext);
        kfree(sig_ctx->cpu_ctx.ctx_info);
        simd_free_context(sig_ctx->simd_ctx);
        kfree(sig_ctx);
        return FAILURE;
    }
    new_scontext->in_progress = 0;
    new_scontext->next = NULL;
    stack_t* old_kstack = thread->kstack;
    thread->kstack = new_scontext->kstack;
    status_t st = process_init_thread_context(
        new_scontext->context,
        process->vmm,
        process->stramp_address,
        new_scontext->stack->top,
        0,
        thread
    );
    thread->kstack = old_kstack;
    if (st != SUCCESS) {
        panic("process_create_scontext: Failed to initialize signal thread context");
        kfree(new_scontext);
        kfree(sig_ctx->cpu_ctx.ctx_info);
        simd_free_context(sig_ctx->simd_ctx);
        kfree(sig_ctx);
        return FAILURE;
    }

    //Hardcode the parameters for the signal trampoline
    new_scontext->context->cpu_ctx.rdi = signal->signo; // signo
    new_scontext->context->cpu_ctx.rsi = (uint64_t)action; // sigaction_t *sigact
    new_scontext->context->cpu_ctx.rdx = (uint64_t)&new_scontext->context->cpu_ctx; // cpu_context_t *ctx
    new_scontext->context->cpu_ctx.rcx = (uint64_t)new_scontext->stack->top; // stack pointer (hidden arg)
    new_scontext->context->cpu_ctx.r8 = (uint64_t)action->sa_handler; // handler address (hidden arg)
   
    //Iterate the thread's scontext list and add it in the correct place
    //Remember that lower signal number have priority
    sigctx_t * current = thread->scontext;
    sigctx_t * prev = NULL;
    while (current != NULL && current->signal->signo < signal->signo) {
        prev = current;
        current = current->next;;
    }
    if (prev == NULL) {
        //Insert at head
        new_scontext->next = thread->scontext;
        thread->scontext = new_scontext;
    } else {
        //Insert in middle or end
        new_scontext->next = prev->next;
        prev->next = new_scontext;
    }

    return SUCCESS;
}

void process_sigret(thread_t * thread) {
    if (!thread) {
        panic("process_sigret: thread is NULL");
        return;
    }

    //Remove the currently running signal context if any, remember to handle the linked list
    if (thread->scontext && thread->scontext->in_progress) {
        sigctx_t * finished_ctx = thread->scontext;
        thread->scontext = finished_ctx->next;
        //kprintf("process_sigret: Freeing finished signal context %p for process %d thread %p\n", finished_ctx, ((process_t *)thread->process)->pid, thread);
        simd_free_context(finished_ctx->context->simd_ctx);
        kfree(finished_ctx->context->cpu_ctx.ctx_info);
        kfree(finished_ctx->context);
        stackfree(((process_t *)thread->process)->vmm, finished_ctx->stack);
        kstackfree(finished_ctx->kstack);
        kfree(finished_ctx);
    }
}

sigctx_t * process_get_scontext(thread_t * thread) {
    return thread->scontext;
}

status_t process_sigaction(process_t * process, int signum, const struct sigaction * act, struct sigaction * oldact) {
    if (!process) {
        return -EINVAL;
    }
    if (signum < 1 || signum >= NSIG) {
        return -EINVAL;
    }

    if (oldact) {
        sigaction_t * existing = process->signal_actions[signum];
        if (existing) {
            memcpy(oldact, existing, sizeof(sigaction_t));
        } else {
            memset(oldact, 0, sizeof(sigaction_t));
        }
    }

    if (act) {
        sigaction_t * new_action = kmalloc(sizeof(sigaction_t));
        if (!new_action) {
            return -ENOMEM;
        }
        memcpy(new_action, act, sizeof(sigaction_t));
        process->signal_actions[signum] = new_action;
    } else {
        //Remove existing action
        sigaction_t * existing = process->signal_actions[signum];
        if (existing) {
            kfree(existing);
            process->signal_actions[signum] = NULL;
        }
    }
    return SUCCESS;
}

status_t process_kill(process_t * process, int code) {
    if (!process) {
        return FAILURE;
    }
    //Kill doesn't kill the process, it sends a signal to it
    signal_t * sig = kmalloc(sizeof(signal_t));
    if (!sig) {
        panic("process_kill: Failed to allocate memory for signal");
        return FAILURE;
    }

    sig->signo = code;
    sig->next = NULL;

    //Place it at the end of the signal queue
    if (process->signal_queue[code] == NULL) {
        process->signal_queue[code] = sig;
    } else {
        signal_t * current = process->signal_queue[code];
        while (current->next != NULL) {
            current = current->next;
        }
        current->next = sig;
    }

    return SUCCESS;
}

signal_t * process_get_signal(process_t * process) {
    if (!process) {
        return NULL;
    }

    for (int sig = 1; sig < NSIG; sig++) {
        if (process->signal_queue[sig] != NULL) {
            signal_t * ret = process->signal_queue[sig];
            process->signal_queue[sig] = ret->next;
            ret->next = NULL;
            return ret;
        }
    }

    return NULL;
}

thread_t * duplicate_thread(process_t * parent, thread_t * og) {
    if (!parent || !og) {
        panic("duplicate_thread: parent or og thread is NULL");
    }

    thread_t * new_thread = kmalloc(sizeof(thread_t));
    //kprintf("duplicate_thread: Duplicating thread %p for process %d into new thread at %p\n", og, parent->pid, new_thread);
    if (!new_thread) {
        panic("duplicate_thread: Failed to allocate memory for new thread");
        return NULL;
    }
    memset(new_thread, 0, sizeof(thread_t));
    new_thread->stack_size = og->stack_size;
    new_thread->kstack = kstackalloc(parent->vmm, KERNEL_STACK_SIZE);
    new_thread->tid = -1; // Will be set by scheduler
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
    //kprintf("duplicate_thread: Created new context_info_t at %p\n", new_ctx_info);
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
    //kprintf("duplicate_thread: Created new cpu_context_t at %p\n", new_cpu_ctx);
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
    //kprintf("duplicate_thread: Created new context_t at %p\n", new_ctx);
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
    //kprintf("duplicate_thread: Created new SIMD for process %d thread %p at %p\n", parent->pid, og, new_ctx->simd_ctx);
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
    //kprintf("duplicate_thread: Created new KERNEL context_t for process %d thread %p at %p\n", parent->pid, og, new_thread->kcontext);
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
    new_thread->kcontext->simd_ctx = simd_create_context();
    //kprintf("duplicate_thread: Created new KERNEL SIMD for process %d thread %p at %p\n", parent->pid, og, new_thread->kcontext->simd_ctx);
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
    new_thread->kcontext->cpu_ctx.ctx_info = (context_info_t*)kmalloc(sizeof(context_info_t));
    //kprintf("duplicate_thread: Created new KERNEL context_info_t for process %d thread %p at %p\n", parent->pid, og, new_thread->kcontext->cpu_ctx.ctx_info);
    if (!new_thread->kcontext->cpu_ctx.ctx_info) {
        panic("duplicate_thread: Failed to allocate memory for kernel context_info_t");
        simd_free_context(new_thread->kcontext->simd_ctx);
        kfree(new_thread->kcontext);
        simd_free_context(new_ctx->simd_ctx);
        kfree(new_ctx);
        kfree(new_cpu_ctx);
        kfree(new_ctx_info);
        kfree(new_thread);
        return NULL;
    }
    memset(new_thread->kcontext->cpu_ctx.ctx_info, 0, sizeof(context_info_t));

    new_thread->context = new_ctx;
    new_thread->entry = og->entry;
    new_thread->state = og->state;
    new_thread->process = (void *)parent;

    return new_thread;
}

status_t process_thread_exit(thread_t * thread) {
    if (!thread) {
        return FAILURE;
    }

    if (!thread->process) {
        panic("process_thread_exit: thread has no associated process");
        return FAILURE;
    }

    process_t * process = (process_t *)thread->process;
    vmm_root_t * vmm = process->vmm;

    //Free thread resources
    if (thread->kstack) {
        kstackfree(thread->kstack);
    }
    if (thread->ustack) {
        stackfree(vmm, thread->ustack);
    }

    if (thread->context) {
        if (thread->context->cpu_ctx.ctx_info) {
            kfree(thread->context->cpu_ctx.ctx_info);
        }
        if (thread->context->simd_ctx) {
            simd_free_context(thread->context->simd_ctx);
        }
        kfree(thread->context);
    }
    if (thread->kcontext) {
        if (thread->kcontext->cpu_ctx.ctx_info) {
            kfree(thread->kcontext->cpu_ctx.ctx_info);
        }
        if (thread->kcontext->simd_ctx) {
            simd_free_context(thread->kcontext->simd_ctx);
        }
        kfree(thread->kcontext);
    }

    //Remove from scheduler queues and from process
    status_t st = scheduler_remove(thread);
    if (st != SUCCESS) {
        panic("process_thread_exit: Failed to remove thread from scheduler");
    }
    for (int i = 0; i < MAX_THREADS_PER_PROCESS; i++) {
        process_t * process = (process_t *)thread->process;
        if (process->threads[i] == thread) {
            process->threads[i] = NULL;
            process->thread_count--;
            break;
        }
    }
    kfree(thread);
    return SUCCESS;
}

thread_t * process_get_current_thread(void) {
    return (thread_t *)cpu_get_current_thread();
}

process_t * process_fork(process_t * parent, thread_t * forking_thread) {
    if (!parent || !forking_thread) {
        return NULL;
    }

    //kprintf("Process %d is forking thread %p\n", parent->pid, forking_thread);

    process_t * child = kmalloc(sizeof(process_t));
    //kprintf("process_fork: Allocated child process at %p\n", child);
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
        //kprintf("process_fork: Duplicated VMM for child process at %p (parent was: %p | pid: %d\n", child->vmm, parent->vmm, parent->pid);
    }

    status_t st = vmarea_fork(child, parent);
    if (st != SUCCESS) {
        panic("process_fork: Failed to duplicate VM areas for child process");
        vmm_free_root(child->vmm);
        kfree(child);

        return NULL;
    }

    //duplicate only forking_thread
    memset(child->threads, 0, MAX_THREADS_PER_PROCESS * sizeof(thread_t *));
    child->threads[0] = duplicate_thread(child, forking_thread);
    if (!child->threads[0]) {
        panic("process_fork: Failed to duplicate thread for child process");
        vmarea_remove_all(child);
        vmm_free_root(child->vmm);
        kfree(child);

        return NULL;
    }

    child->threads[0]->context->cpu_ctx.ctx_info->thread = child->threads[0]; //UTTERLY STUPID
    child->thread_count = 1;
    child->main_thread = child->threads[0];
    child->current_thread = child->threads[0];
    child->pid = -1; // Will be set by scheduler
    child->ppid = parent->pid;
    child->uid = parent->uid;
    child->gid = parent->gid;
    child->stramp_address = parent->stramp_address;
    child->nice = parent->nice;
    child->cwd.mount = parent->cwd.mount;
    memcpy(child->cwd.internal_path, parent->cwd.internal_path, VFS_PATH_MAX);
    child->rootdir.mount = parent->rootdir.mount;
    memcpy(child->rootdir.internal_path, parent->rootdir.internal_path, VFS_PATH_MAX);
    child->binary_entry = parent->binary_entry;
    child->exit_code = 0;
    child->state = SCHEDULER_STATUS_RUNABLE;
    for (int i = 0; i < parent->open_file_count; i++) {
        child->open_files[i] = parent->open_files[i];
    }
    child->open_file_count = parent->open_file_count;
    //Duplicate signal actions
    for (int i = 0; i < NSIG; i++) {
        if (parent->signal_actions[i]) {
            sigaction_t * new_action = kmalloc(sizeof(sigaction_t));
            if (!new_action) {
                panic("process_fork: Failed to allocate memory for signal action");
            }
            memcpy(new_action, parent->signal_actions[i], sizeof(sigaction_t));
            child->signal_actions[i] = new_action;
        } else {
            child->signal_actions[i] = NULL;
        }
    }
    //Initialize signal queue to NULL
    for (int i = 0; i < NSIG; i++) {
        child->signal_queue[i] = NULL;
    }

    create_args(child, (char **)parent->argv, (char **)parent->envp, &parent->auxv, &parent->auxv_size);
    child->threads[0]->context->cpu_ctx.rax = 0; // Child process gets 0 return value from fork

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

int process_find_thread_slot(process_t* process) {
    if (!process) {
        panic("process_find_thread_slot: process is NULL");
        return -1;
    }

    for (int i = 0; i < MAX_THREADS_PER_PROCESS; i++) {
        if (process->threads[i] == NULL) {
             return i;
        }
    }

    panic("process_find_thread_slot: No available thread slots");
    return -1;
}

status_t process_destroy_thread(process_t * process, thread_t * thread) {
    if (!thread) {
        panic("process_destroy_thread: thread is NULL");
        return FAILURE;
    }

    if (thread->context && thread->kcontext) {
        if (thread->context->simd_ctx == thread->kcontext->simd_ctx) {
            panic("process_destroy_thread: thread context and kcontext SIMD contexts are the same");
        }
    }
    
    if (thread->context) {

        if (thread->context->simd_ctx) {
            //kprintf("process_destroy_thread: Freeing SIMD context %p from thread %p\n", thread->context->simd_ctx, thread);
            simd_free_context(thread->context->simd_ctx);
        }
        if (thread->context->cpu_ctx.ctx_info) {
            //kprintf("process_destroy_thread: Freeing context_info_t %p from thread %p\n", thread->context->cpu_ctx.ctx_info, thread);
            kfree(thread->context->cpu_ctx.ctx_info);
        }

        //kprintf("process_destroy_thread: Freeing context %p from thread %p\n", thread->context, thread);
        kfree(thread->context);
    }

    if (thread->kcontext) {
        if (thread->kcontext->simd_ctx) {
            //kprintf("process_destroy_thread: Freeing KERNEL SIMD context %p from thread %p\n", thread->kcontext->simd_ctx, thread);
            simd_free_context(thread->kcontext->simd_ctx);
        }
        if (thread->kcontext->cpu_ctx.ctx_info) {
            //kprintf("process_destroy_thread: Freeing KERNEL context_info_t %p from thread %p\n", thread->kcontext->cpu_ctx.ctx_info, thread);
            kfree(thread->kcontext->cpu_ctx.ctx_info);
        }

        //kprintf("process_destroy_thread: Freeing KERNEL context %p from thread %p\n", thread->kcontext, thread);
        kfree(thread->kcontext);
    }

    if (thread->ustack) {
        //kprintf("process_destroy_thread: Freeing user stack %p from thread %p\n", thread->ustack, thread);
        stackfree(process->vmm, thread->ustack);
    }
    if (thread->kstack) {
        //kprintf("process_destroy_thread: Freeing kernel stack %p from thread %p\n", thread->kstack, thread);
        kstackfree(thread->kstack);
    }

    scheduler_remove(thread);
    kfree(thread);
    return SUCCESS;
}

thread_t * process_create_thread(process_t * process, void * entry_point) {
    if (!process) panic("process_create_thread: process is NULL");
    if (process->thread_count >= MAX_THREADS_PER_PROCESS) {
        panic("process_create_thread: Maximum thread count reached");
        return NULL;
    }

    int new_thread_slot = process_find_thread_slot(process);
    if (new_thread_slot < 0) {
        panic("process_create_thread: No available thread slots");
        return NULL;
    }

    thread_t * new_thread = kmalloc(sizeof(thread_t));

    //kprintf("process_create_thread: Creating thread for process %d at slot %d\n", process->pid, process->thread_count);
    memset(new_thread, 0, sizeof(thread_t));

    new_thread->context = kmalloc(sizeof(context_t));
    //kprintf("process_create_thread: Created context for process %d thread at %p\n", process->pid, new_thread->context);
    if (!new_thread->context) {
        panic("process_create_thread: Failed to allocate CPU context");
        return NULL;
    }
    memset(new_thread->context, 0, sizeof(context_t));
    new_thread->kcontext = kmalloc(sizeof(context_t));
    //kprintf("process_create_thread: Created KERNEL context for process %d thread at %p\n", process->pid, new_thread->kcontext);
    new_thread->kcontext_pending = 0;
    if (!new_thread->kcontext) {
        panic("process_create_thread: Failed to allocate kernel CPU context");
        return NULL;
    }
    memset(new_thread->kcontext, 0, sizeof(context_t));

    new_thread->context->simd_ctx = simd_create_context();
    //kprintf("process_create_thread: Created SIMD context for process %d thread at %p\n", process->pid, new_thread->context->simd_ctx);
    if (!new_thread->context->simd_ctx) {
        panic("process_create_thread: Failed to allocate SIMD context");
        return NULL;
    }
    memset(new_thread->context->simd_ctx, 0, 512);

    new_thread->kcontext->cpu_ctx.ctx_info = (context_info_t*)kmalloc(sizeof(context_info_t));
    //kprintf("process_create_thread: Created KERNEL context_info_t for process %d thread at %p\n", process->pid, new_thread->kcontext->cpu_ctx.ctx_info);
    if (!new_thread->kcontext->cpu_ctx.ctx_info) {
        panic("process_create_thread: Failed to allocate kernel context_info_t");
        return NULL;
    }
    memset(new_thread->kcontext->cpu_ctx.ctx_info, 0, sizeof(context_info_t));
    new_thread->kcontext->simd_ctx = simd_create_context();
    //kprintf("process_create_thread: Created KERNEL SIMD context for process %d thread at %p\n", process->pid, new_thread->kcontext->simd_ctx);
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
    new_thread->tid = -1; // Will be set by scheduler
    new_thread->kstack = kstackalloc(process->vmm, KERNEL_STACK_SIZE);
    if (!new_thread->kstack) {
        panic("context_init: Failed to allocate kernel stack for process");
        return NULL;
    }

    new_thread->ustack = kmalloc(sizeof(stack_t));
    if (!new_thread->ustack) {
        panic("process_create_thread: Failed to allocate user stack structure");
        return NULL;
    }
    memset(new_thread->ustack, 0, sizeof(stack_t));
    new_thread->ustack = stackalloc(process->vmm, new_thread->stack_size, VMM_REGION_U_STACK - new_thread->stack_size, VMM_WRITE_BIT | VMM_USER_BIT);
    if (new_thread->ustack == NULL) {
        panic("process_create_thread: Failed to allocate user stack");
        return NULL;
    }

    void * identity_base = to_kident(process->vmm, (void*)new_thread->ustack->base);
    void * identity_top = identity_base + new_thread->stack_size;
    void * altered_identity_top = loader_create_args(identity_top, new_thread->stack_size, process->argv, process->envp, process->auxv);
    uint64_t stack_offset = (uint64_t)identity_top - (uint64_t)altered_identity_top;

    new_thread->ustack->top -= stack_offset;

    if (process->thread_count == 0) {
        process->main_thread = new_thread;
    }

    //parse_stack(altered_identity_top);
    process->thread_count++;
    process->threads[new_thread_slot] = new_thread;

    return new_thread;
}

status_t process_execve(thread_t * thread, cpu_context_t * ctx, char * filename, char ** argv, char ** envp) {
    if (!thread) {
        panic("process_execve: thread is NULL");
        return FAILURE;
    }
    process_t * process = (process_t *)thread->process;
    if (!process) {
        panic("process_execve: thread has no associated process");
        return FAILURE;
    }

    loaded_elf_t * elf = elf_load_elf(process, filename, thread);
    if (!elf) {
        return FAILURE;
    }

    //Empty signal queue
    for (int i = 0; i < NSIG; i++) {
        signal_t * sig = process->signal_queue[i];;
        while (sig) {
            signal_t * next = sig->next;
            kfree(sig);
            sig = next;
        }
    }

    //Empty signal handlers
    for (int i = 0; i < NSIG; i++) {
        process->signal_actions[i] = 0x0;
    }

    kfree(process->argv);
    kfree(process->envp);
    kfree(process->auxv);
    create_args(process, argv, envp, &elf->auxv, &elf->auxv_size);

    process->binary_entry = (void *)elf->ehdr->e_entry;
    process->main_thread = thread;
    process->current_thread = thread;
    process->threads[0] = thread;
    for (int i = 1; i < MAX_THREADS_PER_PROCESS; i++) {
        process->threads[i] = NULL;
    }
    process->thread_count = 1;

    status_t st = process_init_thread_context(
        thread->context,
        process->vmm,
        process->binary_entry,
        (void *)thread->ustack->top,
        process->argv,
        thread
    );

    //kprintf("process_execve: Initialized main thread context at %p\n", thread->context);
    if (st != SUCCESS) {
        panic("process_execve: Failed to initialize main thread context");

    }

    context_restore(thread->context, ctx);
    cpu_set_context_info(thread->context->cpu_ctx.ctx_info);
    //kprintf("process_execve: Restored context for main thread %p\n", thread);
    return SUCCESS;
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
        //kprintf("process_destroy: Destroying thread %d of process %d\n", i, process->pid);
        process_destroy_thread(process, process->threads[i]);
        process->threads[i] = NULL;
    }

    //kprintf("process_destroy: Removing all VM areas for process %d\n", process->pid);
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
        process->threads[i]->state = SCHEDULER_STATUS_ZOMBIE;
    }
    process->state = SCHEDULER_STATUS_ZOMBIE;
    wakeup(SIGNAL_WAITPID);
    return SUCCESS;
}

void process_init(const char * INIT_PROCESS, const char * INIT_TTY, vfs_path_t INIT_CWD, vfs_path_t INIT_ROOT) {
    assert(INIT_PROCESS != NULL);
    process_t * init_process = process_create(INIT_PROCESS_PARENT_CODE, INIT_PROCESS, INIT_TTY, INIT_CWD, INIT_ROOT, NULL, NULL);
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

int process_dup(process_t * process, int old_fd, int new_fd) {
    //If new_fd is -1, find the first available slot (like dup)
    //If new_fd is >= 0, try to use that slot (like dup2)
    if (!process) {
        panic("process_dup: process is NULL");
        return -1;
    }

    if (old_fd < 0 || old_fd >= MAX_OPEN_FILES) {
        panic("process_dup: old_fd is out of bounds");
        return -1;
    }

    if (new_fd == -1) {
        //Find first available slot
        for (int i = 0; i < MAX_OPEN_FILES; i++) {
            if (!process->open_files[i].valid) {
                new_fd = i;
                break;
            }
        }
        if (new_fd == -1) {
            panic("process_dup: No available file descriptor slots");
            return -1;
        }
    } else {
        if (new_fd < 0 || new_fd >= MAX_OPEN_FILES) {
            panic("process_dup: new_fd is out of bounds");
            return -1;
        }
    }

    process->open_files[new_fd] = process->open_files[old_fd];
    return new_fd;
}