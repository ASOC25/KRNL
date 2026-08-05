#include <krnl/process/process.h>
#include <krnl/debug/debug.h>
#include <krnl/debug/perf.h>
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
#include <krnl/process/pipe.h>
#include <krnl/fs/tty/tty.h>

#include <krnl/libraries/assert/assert.h>

extern void set_cpu_fs_base(uint64_t base);

/* Set once, in process_init(), and never again. Orphans are reparented to
   this pid so they can still be reaped (see process_exit()/
   scheduler_reparent_children()). -1 means "no init yet". */
static pid_t g_init_pid = -1;

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

void duplicate_args(process_t * process, char ** argv, char ** envp, struct auxv ** out_auxv, uint64_t * out_auxv_size) {
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
        status_t st = vfs_open(tty, O_RDWR, 0, &newfd);
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

    duplicate_args(new_process, (char**)argv, (char**)envp, &elf->auxv, &elf->auxv_size);
    memset(new_process->threads, 0, MAX_THREADS_PER_PROCESS * sizeof(thread_t *));
    new_process->binary_entry = (void *)elf->entry;
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
        process_open_stdfiles(new_process, tty);
    } else {
        new_process->ppid = parent->pid;
        new_process->uid = parent->uid;
        new_process->gid = parent->gid;
        for (int i = 0; i < parent->open_file_count; i++) {
            new_process->open_files[i] = parent->open_files[i];
        }
        new_process->open_file_count = parent->open_file_count;
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
        int nargs = 0;
        while (args[nargs]) nargs++;
        ctx->cpu_ctx.rdi = (nargs > 0) ? (uint64_t)args[0] : 0;
        ctx->cpu_ctx.rsi = (nargs > 1) ? (uint64_t)args[1] : 0;
        ctx->cpu_ctx.rdx = (nargs > 2) ? (uint64_t)args[2] : 0;
        ctx->cpu_ctx.rcx = (nargs > 3) ? (uint64_t)args[3] : 0;
        ctx->cpu_ctx.r8  = (nargs > 4) ? (uint64_t)args[4] : 0;
        ctx->cpu_ctx.r9  = (nargs > 5) ? (uint64_t)args[5] : 0;
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

/* Save cpu_ctx (the raw, currently-active interrupt frame) into ctx (the
   thread_t's own persistent context_t). Each thread_t owns a single,
   never-reassigned context_info_t (allocated once at thread creation); we
   deliberately keep ctx->cpu_ctx.ctx_info pointing at that same allocation
   rather than adopting whatever ctx_info pointer happens to be in
   cpu_ctx->ctx_info (that's just whichever thread's context_info_t was
   active in [gs:0x8] right before this interrupt — usually this thread's
   own, but not guaranteed). A previous version of this function copied
   cpu_ctx->ctx_info's *contents* into this thread's own ctx_info allocation
   in place; when the two didn't refer to the same thread, that silently
   overwrote a live, unrelated thread's context_info_t with this thread's
   identity, corrupting the other thread's saved cs/ss/kernel_stack/thread
   fields for good — see git history for the (extensively debugged) crash
   this caused. */
void context_save(context_t* ctx, cpu_context_t* cpu_ctx){
    context_info_t * own_ctx_info = ctx->cpu_ctx.ctx_info;
    simd_save_context(ctx->simd_ctx);
    memcpy(&ctx->cpu_ctx, cpu_ctx, sizeof(cpu_context_t));
    ctx->cpu_ctx.ctx_info = own_ctx_info;
}

/* Restore ctx (the thread_t's own persistent context_t) into cpu_ctx (the
   raw interrupt frame that will actually be resumed via iretq/sysret).
   cpu_ctx->ctx_info is set to ctx's own context_info_t (see context_save's
   comment above for why this must be the thread's own allocation, not
   whatever used to be active). */
void context_restore(context_t* ctx, cpu_context_t* cpu_ctx){
    simd_restore_context(ctx->simd_ctx);
    set_cpu_fs_base(ctx->fs_base);
    memcpy(cpu_ctx, &ctx->cpu_ctx, sizeof(cpu_context_t));
}

status_t process_handle_default_signal(thread_t * thread, signal_t * signal) {
    if (!thread || !signal)
        panic("process_handle_default_signal: thread or signal is NULL");

    process_t *proc = GET_PROC(thread);
    int signo = signal->signo;

    switch (signo) {
        /* Term: abnormal termination */
        case SIGHUP:
        case SIGINT:
        case SIGKILL:
        case SIGUSR1:
        case SIGPIPE:
        case SIGUSR2:
        case SIGALRM:
        case SIGTERM:
        case SIGSTKFLT:
        case SIGVTALRM:
        case SIGPROF:
        case SIGIO:     /* == SIGPOLL */
        case SIGPWR:
        /* Core: terminate (no core dump facility) */
        case SIGQUIT:
        case SIGILL:
        case SIGTRAP:
        case SIGABRT:   /* == SIGIOT */
        case SIGBUS:
        case SIGFPE:
        case SIGSEGV:
        case SIGXCPU:
        case SIGXFSZ:
        case SIGSYS:    /* == SIGUNUSED */
            process_exit(proc, 128 + signo);
            return SUCCESS;

        /* Stop: suspend all threads in the process */
        case SIGSTOP:
        case SIGTSTP:
        case SIGTTIN:
        case SIGTTOU:
            for (int i = 0; i < MAX_THREADS_PER_PROCESS; i++) {
                if (proc->threads[i])
                    proc->threads[i]->state = SCHEDULER_STATUS_STOPPED;
            }
            proc->state = SCHEDULER_STATUS_STOPPED;
            /* Deliberately not sending the parent an actual SIGCHLD signal
               here (only the wakeup() below, which is what unblocks a
               waitpid() and is all job control actually needs) -- doing so
               was tried and reproducibly made bash's own sigchld_handler
               misbehave (it re-ran the just-stopped foreground command from
               history), a latent bash/mlibc bug this code path had simply
               never exercised before. See FEATURES.md / project memory. */
            wakeup(SIGNAL_WAITPID);
            return SUCCESS;

        /* Continue: clear pending stop signals and resume stopped threads */
        case SIGCONT: {
            for (int s = SIGSTOP; s <= SIGTTOU; s++) {
                signal_t *sq = proc->signal_queue[s];
                while (sq) {
                    signal_t *next = sq->next;
                    kfree(sq);
                    sq = next;
                }
                proc->signal_queue[s] = NULL;
            }
            for (int i = 0; i < MAX_THREADS_PER_PROCESS; i++) {
                if (proc->threads[i] && proc->threads[i]->state == SCHEDULER_STATUS_STOPPED)
                    proc->threads[i]->state = SCHEDULER_STATUS_RUNABLE;
            }
            proc->state = SCHEDULER_STATUS_CONTINUED;
            /* See the matching comment in the Stop case above. */
            wakeup(SIGNAL_WAITPID);
            return SUCCESS;
        }

        /* Ignore: SIGCHLD, SIGURG, SIGWINCH, and unhandled RT signals */
        default:
            return SUCCESS;
    }
}

status_t signal_deliver(thread_t *thread, signal_t *signal, cpu_context_t *ctx) {
    process_t *proc = GET_PROC(thread);
    int signo = signal->signo;
    sigaction_t *action = proc->signal_actions[signo];

    if (!action || (uintptr_t)action->sa_handler == (uintptr_t)SIG_DFL) {
        status_t st = process_handle_default_signal(thread, signal);
        kfree(signal);
        return st;
    }
    if ((uintptr_t)action->sa_handler == (uintptr_t)SIG_IGN) {
        kfree(signal);
        return SUCCESS;
    }

    /* Build rt_sigframe on user stack.
       The x86-64 SysV ABI reserves a 128-byte "red zone" below RSP that
       leaf functions may use without adjusting RSP. ctx->rsp here is
       wherever the thread happened to be asynchronously preempted (any
       instruction boundary, not just a syscall/function-call boundary),
       so that redzone can be live with real data (spilled locals,
       in-flight pointers, ...). Placing the frame right below the raw
       RSP -- without skipping the redzone first -- clobbers that data,
       corrupting whatever the thread was in the middle of computing. This
       was silently corrupting memory on every signal delivery (surfacing
       as sporadic, differently-shaped crashes soon after job-control-heavy
       startup code like bash's, which calls sigaction/sigprocmask
       repeatedly and is therefore the most likely place to have a signal
       actually land mid-instruction).
       frame_addr is 16-aligned; rsp_for_handler = frame_addr - 8 satisfies
       the ABI requirement of RSP%16==8 at handler entry. */
    uint64_t frame_sz   = sizeof(struct rt_sigframe);
    uint64_t frame_addr = (ctx->rsp - 128 - frame_sz) & ~0xFULL;
    uint64_t rsp_for_handler = frame_addr - 8;

    struct rt_sigframe *kframe = to_kident(proc->vmm, (void *)frame_addr);
    if (!kframe) {
        kfree(signal);
        return FAILURE;
    }

    /* siginfo */
    memset(&kframe->info, 0, sizeof(siginfo_t));
    kframe->info.si_signo = signo;
    kframe->info.si_errno = signal->info.si_errno;
    kframe->info.si_code  = signal->info.si_code;

    /* ucontext */
    memset(&kframe->uc, 0, sizeof(k_ucontext_t));
    kframe->uc.uc_sigmask = proc->sig_mask; /* save old mask */

    k_mcontext_t *mc = &kframe->uc.uc_mcontext;
    mc->gregs[MC_R8]     = ctx->r8;
    mc->gregs[MC_R9]     = ctx->r9;
    mc->gregs[MC_R10]    = ctx->r10;
    mc->gregs[MC_R11]    = ctx->r11;
    mc->gregs[MC_R12]    = ctx->r12;
    mc->gregs[MC_R13]    = ctx->r13;
    mc->gregs[MC_R14]    = ctx->r14;
    mc->gregs[MC_R15]    = ctx->r15;
    mc->gregs[MC_RDI]    = ctx->rdi;
    mc->gregs[MC_RSI]    = ctx->rsi;
    mc->gregs[MC_RBP]    = ctx->rbp;
    mc->gregs[MC_RBX]    = ctx->rbx;
    mc->gregs[MC_RDX]    = ctx->rdx;
    mc->gregs[MC_RAX]    = ctx->rax;
    mc->gregs[MC_RCX]    = ctx->rcx;
    mc->gregs[MC_RSP]    = ctx->rsp;
    mc->gregs[MC_RIP]    = ctx->rip;
    mc->gregs[MC_EFL]    = ctx->rflags;
    mc->gregs[MC_CSGSFS] = ctx->cs;
    mc->gregs[MC_ERR]    = ctx->error_code;
    mc->gregs[MC_TRAPNO] = ctx->interrupt_number;

    /* Write restorer address at [rsp_for_handler] (acts as return address) */
    uint64_t *kret = to_kident(proc->vmm, (void *)rsp_for_handler);
    if (!kret) {
        kfree(signal);
        return FAILURE;
    }
    void *restorer = (action->sa_flags & SA_RESTORER) ? (void *)action->sa_restorer
                                                      : proc->stramp_address;
    *kret = (uint64_t)restorer;

    /* Update signal mask: block sa_mask[0] + this signal (unless SA_NODEFER) */
    proc->sig_mask |= action->sa_mask[0];
    if (!(action->sa_flags & SA_NODEFER))
        proc->sig_mask |= (1UL << (signo - 1));
    proc->sig_mask &= ~((1UL << (SIGKILL - 1)) | (1UL << (SIGSTOP - 1)));

    /* Redirect ctx to handler */
    void *handler = (action->sa_flags & SA_SIGINFO) ? (void *)action->sa_sigaction
                                                    : (void *)action->sa_handler;
    ctx->rip = (uint64_t)handler;
    ctx->rsp = rsp_for_handler;
    ctx->rdi = (uint64_t)signo;
    ctx->rsi = frame_addr + __builtin_offsetof(struct rt_sigframe, info);
    ctx->rdx = frame_addr + __builtin_offsetof(struct rt_sigframe, uc);

    kfree(signal);
    return SUCCESS;
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
        if (process->signal_queue[sig] == NULL) continue;
        /* Skip signals blocked by sig_mask (SIGKILL/SIGSTOP always delivered) */
        if (sig != SIGKILL && sig != SIGSTOP) {
            if (process->sig_mask & (1UL << (sig - 1))) continue;
        }
        signal_t * ret = process->signal_queue[sig];
        process->signal_queue[sig] = ret->next;
        ret->next = NULL;
        return ret;
    }

    return NULL;
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
    new_thread->stack_size = og->stack_size;

    PERF_BEGIN(t_kstack);
    new_thread->kstack = kstackalloc(parent->vmm, KERNEL_STACK_SIZE);
    if (!new_thread->kstack) {
        panic("duplicate_thread: Failed to allocate memory for new kstack");
    }
    memcpy((void*)((uint64_t)new_thread->kstack->base), og->kstack->base, KERNEL_STACK_SIZE);
    new_thread->tid = -1; // Will be set by scheduler
    PERF_END(t_kstack, "    duplicate_thread/kstackalloc+copy");

    PERF_BEGIN(t_ustack);
    new_thread->ustack = copy_stack(parent->vmm, og->ustack, og->context->cpu_ctx.rsp);
    PERF_END(t_ustack, "    duplicate_thread/copy_stack (ustack)");
    if (!new_thread->kstack || !new_thread->ustack) {
        panic("duplicate_thread: Failed to copy stacks for new thread");
        kfree(new_thread);
        return NULL;
    }

    PERF_BEGIN(t_ctx);
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
    /* Mirrors new_ctx->fs_base above: a forked child that never execve()s
       only ever gets FS_BASE re-synced via arch_prctl(ARCH_SET_FS, ...),
       which a fork inherits TLS across without re-calling. Leaving this at
       the memset's 0 meant the first time this child blocked in a kernel-
       context wait (sleep()/wakeup() -- pipe/tty/futex/fifo) and later woke
       back up, scheduler_handler's kcontext_pending resume path applied
       fs_base=0 to the live FS_BASE MSR, and the next TLS-relative access
       (get_current_tcb(), used by nearly every libc call) dereferenced
       fs:0 as a NULL pointer. */
    new_thread->kcontext->fs_base = og->context->fs_base;
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
    /* BUG-59: kcontext's ctx_info->thread was never set for any thread but
       idle (see scheduler_create_idle_thread's kctx_info), so as soon as a
       thread's kernel-context sleep resumed through kcontext_restore_
       trampoline (installing this struct into [gs:0x8]), any interrupt
       that fired before the thread's syscall returned to userspace (and
       [gs:0x8] got restored to the real, correctly-populated user ctx_info
       via the syscall epilogue's `pop [gs:0x8]`) saw ctx_info->thread ==
       NULL. syscall_pselect's retry loop is the first call site that sleeps
       more than once per syscall, so it's the first to have a *second*
       nested interrupt land inside that window -- scheduler_handler's
       "ending_thread" then resolves to NULL, kcontext_pending never gets
       re-armed, and the next scheduling round wrongly resumes the thread's
       stale saved *user* context instead of its in-flight kernel context,
       silently rewinding it back to wherever it was last preempted in
       userspace. */
    new_thread->kcontext->cpu_ctx.ctx_info->thread = new_thread;
    new_thread->kcontext->cpu_ctx.ctx_info->kernel_stack = new_thread->kstack->top;
    new_thread->kcontext->cpu_ctx.ctx_info->cs = GDT_KERNEL_CODE * sizeof(gdt_entry_t);
    new_thread->kcontext->cpu_ctx.ctx_info->ss = GDT_KERNEL_DATA * sizeof(gdt_entry_t);
    PERF_END(t_ctx, "    duplicate_thread/ctx-alloc");

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
    if (!child) {
        panic("process_fork: Failed to allocate memory for child process");
        return NULL;
    }
    memset(child, 0, sizeof(process_t));

    PERF_BEGIN(t_vmm_sync);
    vmarea_sync(parent);
    PERF_END(t_vmm_sync, "  process_fork/vmarea_sync");

    if (!parent->vmm) {
        panic("process_fork: Parent process has no VMM");
        kfree(child);
        return NULL;
    }

    PERF_BEGIN(t_vmm_dup);
    child->vmm = vmm_duplicate_fullspace(parent->vmm);
    PERF_END(t_vmm_dup, "  process_fork/vmm_duplicate_fullspace");
    if (!child->vmm) {
        panic("process_fork: Failed to duplicate VMM for child process");
        kfree(child);
        return NULL;
    }

    PERF_BEGIN(t_vmarea);
    status_t st = vmarea_fork(child, parent);
    PERF_END(t_vmarea, "  process_fork/vmarea_fork");
    if (st != SUCCESS) {
        panic("process_fork: Failed to duplicate VM areas for child process");
        vmm_free_root(child->vmm);
        kfree(child);
        return NULL;
    }

    PERF_BEGIN(t_thread);
    memset(child->threads, 0, MAX_THREADS_PER_PROCESS * sizeof(thread_t *));
    child->threads[0] = duplicate_thread(child, forking_thread);
    PERF_END(t_thread, "  process_fork/duplicate_thread");
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
    child->pgid = parent->pgid;
    child->sid = parent->sid;
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

    PERF_BEGIN(t_fds);
    /* BUG-23: iterate all MAX_OPEN_FILES slots, not just open_file_count,
     * because open files may occupy non-contiguous indices. */
    for (int i = 0; i < MAX_OPEN_FILES; i++) {
        child->open_files[i] = parent->open_files[i];
        /* Duplicate native_path to avoid double-free when parent/child close independently */
        if (parent->open_files[i].native_path) {
            size_t plen = strlen(parent->open_files[i].native_path);
            char * dup_path = kmalloc(plen + 1);
            strncpy(dup_path, parent->open_files[i].native_path, plen + 1);
            child->open_files[i].native_path = dup_path;
        }
        /* Child now holds an additional reference to the shared pipe */
        if (parent->open_files[i].pipe) {
            pipe_dup(&child->open_files[i]);
        }
    }
    child->open_file_count = parent->open_file_count;
    PERF_END(t_fds, "  process_fork/dup-fds");

    PERF_BEGIN(t_sigs);
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
    for (int i = 0; i < NSIG; i++) {
        child->signal_queue[i] = NULL;
    }
    child->sig_mask = parent->sig_mask;
    PERF_END(t_sigs, "  process_fork/dup-signals");

    PERF_BEGIN(t_args);
    duplicate_args(child, (char **)parent->argv, (char **)parent->envp, &parent->auxv, &parent->auxv_size);
    PERF_END(t_args, "  process_fork/duplicate_args");

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
    /* BUG-59: see duplicate_thread's identical fix for the full explanation
       -- this struct's ->thread must never be left NULL, or a kernel-context
       resume through it silently breaks any second nested interrupt taken
       before this thread's next real return to userspace. kernel_stack is
       set below once new_thread->kstack exists. */
    new_thread->kcontext->cpu_ctx.ctx_info->thread = new_thread;
    new_thread->kcontext->cpu_ctx.ctx_info->cs = GDT_KERNEL_CODE * sizeof(gdt_entry_t);
    new_thread->kcontext->cpu_ctx.ctx_info->ss = GDT_KERNEL_DATA * sizeof(gdt_entry_t);
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
    new_thread->kcontext->cpu_ctx.ctx_info->kernel_stack = new_thread->kstack->top;

    new_thread->ustack = stackalloc(process->vmm, new_thread->stack_size, VMM_REGION_U_STACK - new_thread->stack_size, VMM_WRITE_BIT | VMM_USER_BIT, 1);
    if (new_thread->ustack == NULL) {
        panic("process_create_thread: Failed to allocate user stack");
        return NULL;
    }

    void * identity_base = to_kident(process->vmm, (void*)new_thread->ustack->base);
    void * identity_top = identity_base + new_thread->stack_size;
    void * user_stack_virtual = (void *)((uint64_t)new_thread->ustack->base + new_thread->stack_size);
    void * altered_identity_top = loader_create_args(identity_top, user_stack_virtual, new_thread->stack_size, process->argv, process->envp, process->auxv);
    uint64_t stack_offset = (uint64_t)identity_top - (uint64_t)altered_identity_top;

    new_thread->ustack->top = (void *)((uint64_t)user_stack_virtual - stack_offset);

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

    PERF_BEGIN(t_elf);
    loaded_elf_t * elf = elf_load_elf(process, filename, thread);
    PERF_END(t_elf, "  process_execve/elf_load_elf");
    if (!elf) {
        return FAILURE;
    }

    PERF_BEGIN(t_cleanup);
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
        if (process->signal_actions[i]) {
            kfree(process->signal_actions[i]);
            process->signal_actions[i] = NULL;
        }
    }

    if (process->argv) {
        for (int i = 0; process->argv[i] != NULL; i++)
            kfree(process->argv[i]);
        kfree(process->argv);
    }
    if (process->envp) {
        for (int i = 0; process->envp[i] != NULL; i++)
            kfree(process->envp[i]);
        kfree(process->envp);
    }
    kfree(process->auxv);
    duplicate_args(process, argv, envp, &elf->auxv, &elf->auxv_size);
    PERF_END(t_cleanup, "  process_execve/signal-cleanup+dup-args");

    PERF_BEGIN(t_stack);
    // The old user stack pages are still mapped (stackalloc tracks them outside vmareas).
    // Properly unmap+free physical pages, then free the descriptor.
    stackfree(process->vmm, thread->ustack);
    kfree(thread->ustack);
    thread->stack_size = NEW_PROCESS_STACK_SIZE;
    thread->ustack = stackalloc(process->vmm, thread->stack_size,
                                VMM_REGION_U_STACK - thread->stack_size,
                                VMM_WRITE_BIT | VMM_USER_BIT, 1);
    if (!thread->ustack) {
        panic("process_execve: Failed to allocate new user stack");
        return FAILURE;
    }
    PERF_END(t_stack, "  process_execve/stack-alloc");

    PERF_BEGIN(t_args);
    void * exec_identity_base = to_kident(process->vmm, (void*)thread->ustack->base);
    void * exec_identity_top  = exec_identity_base + thread->stack_size;
    void * exec_user_top      = (void *)((uint64_t)thread->ustack->base + thread->stack_size);
    void * exec_new_top = loader_create_args(exec_identity_top, exec_user_top, thread->stack_size,
                                             process->argv, process->envp, process->auxv);
    uint64_t exec_stack_offset = (uint64_t)exec_identity_top - (uint64_t)exec_new_top;
    thread->ustack->top = (void *)((uint64_t)exec_user_top - exec_stack_offset);
    PERF_END(t_args, "  process_execve/loader_create_args");

    process->binary_entry = elf->entry;
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
    return SUCCESS;
}

void process_set_exit_code(process_t * process, int code) {
    if (!process) {
        panic("process_set_exit_code: process is NULL");
        return;
    }
    process->exit_code = code;
}

// r_debug struct offsets (x86-64 ABI): version(4) + pad(4) + r_map ptr(8)
#define R_DEBUG_RMAP_OFFSET  8
// link_map struct offsets: l_addr(8) + l_name ptr(8) + l_ld ptr(8) + l_next ptr(8)
#define LMAP_L_ADDR  0
#define LMAP_L_NAME  8
#define LMAP_L_NEXT  24

static uint64_t kident_read_u64(vmm_root_t * vmm, uint64_t va) {
    uint8_t * p = (uint8_t *)to_kident(vmm, (void *)va);
    if (!p) return 0;
    uint64_t v;
    memcpy(&v, p, 8);
    return v;
}

static void kident_read_cstr(vmm_root_t * vmm, uint64_t va, char * out, size_t maxlen) {
    out[0] = '\0';
    if (!va) return;
    size_t page_avail = 0x1000 - (va & 0xfff);
    char * src = (char *)to_kident(vmm, (void *)va);
    if (!src) return;
    size_t n = maxlen < page_avail ? maxlen : page_avail;
    size_t i = 0;
    for (; i < n - 1 && src[i]; i++) out[i] = src[i];
    out[i] = '\0';
}

void process_load_shlib_symtabs(process_t * proc) {
    if (!proc || proc->shlib_syms_loaded || !proc->r_debug_va) return;
    proc->shlib_syms_loaded = 1;

    /* r_debug_va is the VA of DT_DEBUG's d_ptr field in the executable's
     * .dynamic section.  ld.so writes the actual _r_debug address there at
     * startup, so we must dereference it before reading the link map. */
    uint64_t rdebug_actual_va = kident_read_u64(proc->vmm, proc->r_debug_va);
    if (!rdebug_actual_va) return;

    uint64_t r_map_va = kident_read_u64(proc->vmm, rdebug_actual_va + R_DEBUG_RMAP_OFFSET);
    uint64_t visited[64];
    int visited_count = 0;

    while (r_map_va) {
        // Cycle guard
        int already = 0;
        for (int i = 0; i < visited_count; i++) {
            if (visited[i] == r_map_va) { already = 1; break; }
        }
        if (already || visited_count >= 64) break;
        visited[visited_count++] = r_map_va;

        uint64_t l_addr    = kident_read_u64(proc->vmm, r_map_va + LMAP_L_ADDR);
        uint64_t l_name_va = kident_read_u64(proc->vmm, r_map_va + LMAP_L_NAME);
        uint64_t l_next_va = kident_read_u64(proc->vmm, r_map_va + LMAP_L_NEXT);

        char path[256];
        kident_read_cstr(proc->vmm, l_name_va, path, sizeof(path));

        if (path[0] != '\0' && l_addr != 0) {
            // Check whether we already have symbols for this load base
            int have = 0;
            for (proc_symtab_t * s = proc->symtab_list; s; s = s->next) {
                if (s->load_base == l_addr) { have = 1; break; }
            }
            if (!have) {
                vfs_file_descriptor_t fd;
                if (vfs_open(path, 0, 0, &fd) == SUCCESS && fd.valid) {
                    vfs_stat_t stat;
                    if (vfs_fstat(&fd, &stat) == SUCCESS) {
                        size_t fsz = (size_t)stat.st_size;
                        uint8_t * data = kmalloc(fsz);
                        if (data) {
                            ssize_t rd = vfs_read(&fd, data, fsz);
                            if ((size_t)rd == fsz) {
                                proc_symtab_t * st = extract_elf_symtab(data, fsz, l_addr);
                                if (st) {
                                    st->next = proc->symtab_list;
                                    proc->symtab_list = st;
                                }
                            }
                            kfree(data);
                        }
                    }
                    vfs_close(&fd);
                }
            }
        }
        r_map_va = l_next_va;
    }
}

const char * process_resolve_symbol(process_t * proc, uint64_t addr) {
    if (!proc) return NULL;
    const char * best = NULL;
    uint64_t best_start = 0;
    for (proc_symtab_t * st = proc->symtab_list; st; st = st->next) {
        for (uint64_t i = 0; i < st->count; i++) {
            Elf64_Sym * s = &st->syms[i];
            uint64_t va = st->load_base + s->st_value;
            if (addr >= va && addr < va + s->st_size && va >= best_start) {
                best_start = va;
                best = st->strtab + s->st_name;
            }
        }
    }
    return best;
}

status_t process_destroy(process_t * process) {
    if (!process) {
        panic("process_destroy: process is NULL");
        return FAILURE;
    }

    for (int i = 0; i < MAX_THREADS_PER_PROCESS; i++) {
        if (process->threads[i]) {
            //kprintf("process_destroy: Destroying thread %d of process %d\n", i, process->pid);
            process_destroy_thread(process, process->threads[i]);
            process->threads[i] = NULL;
        }
    }

    //kprintf("process_destroy: Removing all VM areas for process %d\n", process->pid);
    vmarea_remove_all(process);
    vmm_free_root(process->vmm);

    proc_symtab_t * st = process->symtab_list;
    while (st) {
        proc_symtab_t * next = st->next;
        kfree(st->syms);
        kfree(st->strtab);
        kfree(st);
        st = next;
    }

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
    for (int i = 0; i < MAX_THREADS_PER_PROCESS; i++) {
        if (process->threads[i])
            process->threads[i]->state = SCHEDULER_STATUS_ZOMBIE;
    }
    process->state = SCHEDULER_STATUS_ZOMBIE;
    /* Reparent any children to init so they can still be reaped — a zombie
       will never call waitpid() again, so its children would otherwise be
       unreapable the moment it exits, not just once it's later destroyed. */
    if (g_init_pid >= 0 && process->pid != g_init_pid) {
        scheduler_reparent_children(process->pid, g_init_pid);
    }
    /* Close all open fds now, not whenever the parent eventually reaps this
       zombie via process_destroy() — otherwise a pipe's reader/writer on the
       other end can block indefinitely on a peer that is functionally dead
       but hasn't had its fds released yet (e.g. `producer | consumer` hangs
       if consumer dies first and producer fills the pipe buffer before the
       shell gets around to waitpid()-reaping consumer). A zombie thread
       never runs again, so releasing its fds here is safe. */
    for (int i = 0; i < MAX_OPEN_FILES; i++) {
        if (process->open_files[i].valid) {
            vfs_close(&process->open_files[i]);
        }
    }
    /* No real SIGCHLD delivery to the parent here — see the Stop-case
       comment in process_handle_default_signal(). wakeup() below is what
       actually unblocks a parent's waitpid(); no caller in this kernel
       relies on the signal itself arriving. */
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

    g_init_pid = init_process->pid;

    /* Init is its own session/process-group leader. Since there is no
       getty/login to hand the controlling terminal's foreground group to
       whoever opens it, seed the tty's foreground pgrp here — otherwise
       bash's job-control startup (which expects tcgetpgrp(tty) == getpgrp())
       sends itself SIGTTIN and immediately stops. */
    init_process->pgid = init_process->pid;
    init_process->sid = init_process->pid;
    tty_set_foreground_pgrp(init_process->pgid);
}

int process_dup(process_t * process, int old_fd, int new_fd) {
    //If new_fd is -1, find the first available slot (like dup)
    //If new_fd is >= 0, try to use that slot (like dup2)
    if (!process) {
        panic("process_dup: process is NULL");
        return -1;
    }

    if (old_fd < 0 || old_fd >= MAX_OPEN_FILES) {
        return -EBADF;
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
            return -EMFILE;
        }
    } else {
        if (new_fd < 0 || new_fd >= MAX_OPEN_FILES) {
            return -EBADF;
        }
    }

    if (new_fd == old_fd)
        return new_fd;

    if (process->open_files[new_fd].valid) {
        vfs_close(&process->open_files[new_fd]); /* pipe-aware: releases pipe refcount if applicable */
        process->open_files[new_fd].mount = NULL;
        process->open_files[new_fd].native_path = NULL;
        process->open_files[new_fd].pipe = NULL;
        process->open_files[new_fd].flags = 0;
        process->open_files[new_fd].position = 0;
        if (process->open_file_count > 0) process->open_file_count--;
    }

    process->open_files[new_fd] = process->open_files[old_fd];
    /* New fd holds an additional reference to the shared pipe */
    if (process->open_files[old_fd].pipe) {
        pipe_dup(&process->open_files[new_fd]);
    }
    /* BUG-46: duplicate native_path so that closing either fd independently
     * does not cause a double-free of the shared pointer. */
    if (process->open_files[old_fd].native_path) {
        size_t plen = strlen(process->open_files[old_fd].native_path);
        char * dup_path = kmalloc(plen + 1);
        strncpy(dup_path, process->open_files[old_fd].native_path, plen + 1);
        process->open_files[new_fd].native_path = dup_path;
    }
    return new_fd;
}