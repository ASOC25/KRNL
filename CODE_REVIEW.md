# KRNL Kernel — Comprehensive Code Review

**Date:** 2026-02-20
**Scope:** `kernel/src/` and all subdirectories
**Focus:** Bugs, memory errors, buffer overruns, race conditions, and logical flaws

---

## Table of Contents

1. [Memory Allocator (`mem/allocator.c`)](#1-memory-allocator)
2. [Physical Memory Manager (`mem/pmm.c`)](#2-physical-memory-manager)
3. [Virtual Memory Areas (`mem/mmap.c`)](#3-virtual-memory-areas)
4. [ELF Loader (`process/loader.c`)](#4-elf-loader)
5. [Process Management (`process/process.c`)](#5-process-management)
6. [Scheduler (`process/scheduler.c`)](#6-scheduler)
7. [Signals & Sleep (`process/signals.c`)](#7-signals--sleep)
8. [System Calls (`arch/x86/syscall.c`)](#8-system-calls)
9. [Interrupt Handling (`arch/x86/idt.c`)](#9-interrupt-handling)
10. [Virtual Memory Engine (`arch/x86/vm.c`)](#10-virtual-memory-engine)
11. [VFS (`vfs/vfs.c`)](#11-vfs)
12. [Standard Library (`libraries/std/strings.c`)](#12-standard-library)
13. [Spinlock (`libraries/lock/spinlock.c`)](#13-spinlock)
14. [CPU Initialization (`arch/x86/cpu.c`)](#14-cpu-initialization)
15. [Summary Table](#15-summary-table)

---

## 1. Memory Allocator

**File:** [mem/allocator.c](kernel/src/mem/allocator.c)

---

### BUG-01 — Heap buffer overflow in `detect_double_lok`

**Line:** 163
**Severity:** High

```c
void detect_double_lok(void * ptr) {
    struct deallocation * current = deallocations;
    for (uint64_t i = 0; i < DEALLOCATION_BUFFER_SIZE; i++) {   // scans ALL 0x1000 slots
```

`add_deallocation` wraps its write index when it reaches `DEALLOCATION_BUFFER_SIZE`:

```c
if (deallocation_count >= DEALLOCATION_BUFFER_SIZE) {
    deallocation_count = 0;   // resets to 0, old data stays in the array
}
```

After the wrap, old entries with stale addresses remain in the array. `detect_double_lok` scans all `0x1000` entries unconditionally, including those stale entries. If a new allocation's address happens to collide with a stale entry, the kernel will falsely panic with "Double free detected". Conversely, if the buffer has overwritten the real prior-free record, actual double frees are silently missed.

**Fix:** track which slots are valid or clear entries when they are overwritten.

---

### BUG-02 — `dump_deallocation` prints nothing

**Lines:** 141–151
**Severity:** Low (debug only)

```c
for (int j = 0; j < STACKTRACE_SIZE; j++) {
    if (dealloc->allocation_stacktrace[j] == NULL) {
        break;
    }
    // ← no kprintf here; the loop body is empty
}
```

Both inner loops in `dump_deallocation` have the logging `kprintf` removed, making the function silently do nothing. This defeats its debugging purpose.

---

### BUG-03 — `copy_kstack` and `copy_stack` are identical and both return `source`

**Lines:** 416–486
**Severity:** Medium

Both functions allocate a new physical page, copy source data into it, remap the virtual address to the new page, call `add_allocation`, and then `return source`. Returning `source` rather than a new `stack_t*` means the caller has no way to distinguish the returned pointer from the original, and the newly allocated page is tracked in the allocator but the `stack_t` struct pointing to it is never updated. Furthermore the two functions are byte-for-byte duplicates, indicating dead code.

---

### BUG-04 — Memory leak in `process_create_thread`

**Lines:** 987–993
**File:** [process/process.c](kernel/src/process/process.c)

```c
new_thread->ustack = kmalloc(sizeof(stack_t));   // allocated here
if (!new_thread->ustack) { panic(...); return NULL; }
memset(new_thread->ustack, 0, sizeof(stack_t));
new_thread->ustack = stackalloc(...);             // pointer overwritten, kmalloc leak
```

The `kmalloc`'d `stack_t` is immediately overwritten by `stackalloc`, which returns its own `kmalloc`'d structure. The first allocation is permanently leaked.

---

### BUG-05 — `kstackalloc` computes `top_address` before alignment

**Lines:** 237–241
**File:** [mem/allocator.c](kernel/src/mem/allocator.c)

```c
uint64_t top_address = (uint64_t)(virt_addr + size);
if (top_address % 0x10) {
    top_address -= top_address % 0x10;
}
top_address -= 0x8;
```

`virt_addr` is `VMM_REGION_K_STACK + phys_addr`, where `phys_addr` is page-aligned and `size` is rounded up to a page. `top_address` will always be page-aligned (divisible by 0x1000, hence also by 0x10), so the alignment branch is dead code. The `- 0x8` adjustment below it shifts the stack top to a non-16-byte-aligned position. The System V AMD64 ABI requires that RSP is 16-byte aligned *before* a `CALL` instruction (i.e., the entry RSP should be `16n - 8`). This subtraction achieves that, but since the alignment branch is dead, the top_address is always `page_top - 8`. If `page_top` is not `16n`, the final value is wrong.

---

### BUG-06 — Dead duplicate check in `stackalloc`

**Lines:** 351–353
**File:** [mem/allocator.c](kernel/src/mem/allocator.c)

```c
    if (st != SUCCESS) {                    // st was already checked at line 336
        panic("stackalloc: Failed to map user pages");
    }
```

This second `st` check is dead code — `st` is not reassigned between lines 336 and 351.

---

## 2. Physical Memory Manager

**File:** [mem/pmm.c](kernel/src/mem/pmm.c)

---

### BUG-07 — PMM does not detect double-free

**Lines:** 113–147

`pmm_free_pages` clears bits in the bitmap without verifying that the bits were actually set (i.e., that the pages were allocated). Freeing a page that was already free silently clears a bit that may already be clear, permitting two distinct allocations to share the same physical page.

---

### BUG-08 — PMM lock array lives inside the region it manages

**Lines:** 43–58

The bitmap (`locks`) is placed at `lock_array_start` which is the first bytes of the physical memory region being managed. Before `pmm_remap` is called the locks pointer holds a physical address. If anything accesses the PMM before `pmm_remap` runs, or if `pmm_remap` is not called at all, the bitmap is accessed at the wrong (physical) address.

---

## 3. Virtual Memory Areas

**File:** [mem/mmap.c](kernel/src/mem/mmap.c)

---

### BUG-09 — COW handler copies only the first page of a multi-page VMA (critical)

**Lines:** 116–155
**Severity:** Critical

```c
uint64_t original_physical;
status_t st = vmm_get_physical_address(
    (vmm_root_t *)process->vmm,
    (uint64_t)vma->start,      // ← always VMA start, regardless of fault address
    &original_physical
);
...
void * ptr = malloc(..., vma->size, (uint64_t)vma->start, ...);
...
memcpy(ptr,
    (void *)vmm_to_identity_map(original_physical),  // ← source = only page 1's phys
    vma->size);                                        // ← copy entire VMA size
```

When a page fault occurs mid-VMA (e.g., at page 3 of a 5-page region):

1. `vmm_get_physical_address` resolves the physical address of `vma->start` (page 1), not the faulting address.
2. The entire VMA is unmapped (`vmm_unmap_pages`) — destroying pages 2–5's mappings before their data is saved.
3. A new contiguous allocation is made for the whole VMA.
4. Only the content of page 1 is copied to the new allocation (identity-mapped from `original_physical`).

Pages 2–5 of the original VMA are permanently lost. Every COW fault on a multi-page VMA results in data corruption.

---

### BUG-10 — `vmarea_mprotect` always returns `FAILURE`

**Line:** 282
**Severity:** Medium

```c
status_t vmarea_mprotect(...) {
    vm_area_t * current = process->vm_areas;
    while (current) {
        if (address >= current->start && ...) {
            current->prot = new_prot;
            vmm_mprotect_pages(...);
        }
        current = current->next;
    }
    return FAILURE;   // ← unconditional FAILURE, even on success
}
```

The function always returns `FAILURE`. `syscall_mprotect` propagates this as `-EIO` to userspace.

---

### BUG-11 — `vmarea_munmap` uses mismatched address semantics

**Lines:** 351–366
**Severity:** High

```c
status_t vmarea_munmap(process_t * process, void * address) {
    vm_area_t * vma = vmarea_find(process, address);   // finds VMA *containing* address
    if (!vma) return FAILURE;

    free(process->vmm, address);    // frees at address (not vma->start)

    status_t st = vmarea_remove_locked(process, address);   // removes at address == vma->start?
```

`vmarea_find` accepts any address inside the VMA, but `free` and `vmarea_remove_locked` both require the exact `start` of the allocation. If `address != vma->start`, `free` panics ("Allocation not found").

---

### BUG-12 — `vmarea_remove` / `vmarea_remove_locked` mismatch in COW path

**Line:** 96 (vmarea_fork)

In `vmarea_fork`, `add_allocation` is called for the destination process with the physical address of only `vma->start` — even if the VMA spans multiple pages backed by multiple physical frames. Only the first frame is registered, so later freeing operations will leave subsequent physical frames permanently leaked.

---

## 4. ELF Loader

**File:** [process/loader.c](kernel/src/process/loader.c)

---

### BUG-13 — Massive VLA on kernel stack (critical stack overflow)

**Lines:** 180–182
**Severity:** Critical

```c
uint64_t argv_pointers[argc];      // VLA, argc from user
uint64_t envp_pointers[envc];      // VLA, envc from user
uint64_t ptr_buffer[0x4000];       // 128 KB on the stack!
```

`ptr_buffer[0x4000]` allocates **131,072 bytes** on the kernel stack at compile time. The kernel stack is typically 8–16 KB (`KERNEL_STACK_SIZE`). A single call to `loader_create_args` immediately overflows the kernel stack by ~112 KB before any application logic runs.

Additionally, `argv_pointers` and `envp_pointers` are variable-length arrays controlled by userspace. A process calling `execve` with 2,000 arguments would add 16 KB to the stack overflow.

---

### BUG-14 — Heap buffer overflow in `elf_load_elf` (critical)

**Lines:** 508, 553–554
**Severity:** Critical

```c
struct auxv * vectors = kmalloc(sizeof(struct auxv) * 7);   // ← 7 entries allocated
...
vectors[7].a_type = AT_NULL;   // ← write to index 7: one past the end!
vectors[7].a_val  = NULL;
...
ld->auxv_size = sizeof(struct auxv) * 8;   // ← comment admits 8 entries needed
```

The allocation is for 7 entries (indices 0–6) but the code writes index 7, performing a heap buffer overflow of `sizeof(struct auxv)` bytes past the allocation.

---

### BUG-15 — Use-after-free in `load_dynamic_linker`

**Lines:** 382–383
**Severity:** High

```c
kfree(elf_datab);
return (uint64_t)DYNAMIC_LINKER_BASE_ADDRESS + elf_header->e_entry;  // elf_header ∈ elf_datab
```

`elf_header` is a pointer into `elf_datab`. Reading `elf_header->e_entry` after `kfree(elf_datab)` is a use-after-free. Depending on allocator behaviour the returned entry point will be corrupted.

---

### BUG-16 — ELF segment copy with no bounds checking

**Line:** 324
**Severity:** High

```c
memcpy(
    (uint8_t *)identity + vaddr_offset,
    elf_datab + program_header->p_offset,      // ← from ELF file, not validated
    program_header->p_filesz                    // ← from ELF file, not validated
);
```

None of the following are validated before the copy:
- `p_offset < file_size`
- `p_offset + p_filesz <= file_size`
- `vaddr_offset + p_filesz <= total_pages * 0x1000`

A crafted ELF binary can trigger reads past the file buffer or writes past the allocated segment, achieving arbitrary kernel memory writes.

---

### BUG-17 — `loader_create_args` performs size check after writing `ptr_buffer`

**Lines:** 225–236

The `max_size` guard fires only after the intermediate `ptr_buffer` has already been populated and after `size` is computed. Because `ptr_buffer` is 128 KB on the stack (BUG-13), the overflow occurs before any size check.

---

## 5. Process Management

**File:** [process/process.c](kernel/src/process/process.c)

---

### BUG-18 — Memory leak of individual argv/envp strings on `execve`

**Lines:** 1048–1050
**Severity:** Medium

```c
kfree(process->argv);   // frees only the pointer array
kfree(process->envp);   // frees only the pointer array
kfree(process->auxv);
```

`duplicate_args` allocates every individual string with `kmalloc`. `kfree(process->argv)` frees only the outer pointer array; the individual `argv[i]` strings are never freed. Every `execve` call leaks all prior argv/envp strings.

---

### BUG-19 — Signal actions leaked on `execve`

**Lines:** 1044–1046
**Severity:** Medium

```c
for (int i = 0; i < NSIG; i++) {
    process->signal_actions[i] = 0x0;   // pointer overwritten, kfree never called
}
```

Each `sigaction_t` is individually `kmalloc`'d by `process_sigaction`. Zeroing the pointer without calling `kfree` permanently leaks every registered signal handler on exec.

---

### BUG-20 — `process_destroy` and `process_exit` iterate by `thread_count` not slot range

**Lines:** 1097–1101, 1119–1121
**Severity:** High

```c
for (int i = 0; i < process->thread_count; i++) {
    process_destroy_thread(process, process->threads[i]);   // threads[i] may be NULL
```

Threads are stored in a fixed-size array with potential gaps (when a thread exits its slot is set to NULL but `thread_count` is not adjusted to reflect gaps). Iterating by count will call `process_destroy_thread(process, NULL)` for gap slots, triggering a panic.

---

### BUG-21 — `dup2` does not close `new_fd` when already open

**Line:** 1182
**Severity:** Medium

```c
process->open_files[new_fd] = process->open_files[old_fd];
```

POSIX requires `dup2` to atomically close `new_fd` if it is already open. This implementation simply overwrites the slot, leaking the old file descriptor's `native_path` allocation and any associated filesystem state.

---

### BUG-22 — Signal stack allocated at a fixed address for every signal

**Line:** 343
**Severity:** High

```c
new_scontext->stack = stackalloc(
    process->vmm,
    thread->stack_size,
    VMM_REGION_S_STACK - thread->stack_size,   // ← same address for every signal
    ...
);
```

All signal stacks are allocated at the same virtual address. When a second signal arrives while a first is being handled, `vmarea_collides` detects the collision and `vmarea_mmap` returns `MAP_FAILED`, followed by a panic.

---

### BUG-23 — `process_fork` copies `open_file_count` files using `open_file_count` as a loop limit

**Lines:** 788–791

```c
for (int i = 0; i < parent->open_file_count; i++) {
    child->open_files[i] = parent->open_files[i];
}
```

`open_file_count` is an aggregate count, not a guaranteed bound on valid slots. Files can be at non-contiguous indices (e.g., slot 0 is closed, slots 1 and 2 are open; `open_file_count == 2`). The loop copies slots 0 and 1 and misses slot 2.

---

## 6. Scheduler

**File:** [process/scheduler.c](kernel/src/process/scheduler.c)

---

### BUG-24 — `waitpid(WNOHANG)` always returns the child PID (never returns 0)

**Lines:** 156–166
**Severity:** High

```c
if (options & WNOHANG) {
    if (iterated_process->state == SCHEDULER_STATUS_ZOMBIE) {
        if (status) { *status = ...; }
        process_destroy(iterated_process);
    }
    changed_pid = iterated_process->pid;  // ← set regardless of zombie state
}
```

POSIX requires `waitpid(WNOHANG)` to return 0 when no child has changed state. Here `changed_pid` is set to the child's PID unconditionally after the zombie block, meaning `waitpid(WNOHANG)` always reports the matched child as "exited" even if it is still running.

---

### BUG-25 — Use-after-free in `waitpid` after `process_destroy`

**Lines:** 161–166
**Severity:** High

```c
process_destroy(iterated_process);   // frees the process struct
...
changed_pid = iterated_process->pid; // reads freed memory
```

`process_destroy` calls `kfree(process)`. Reading `iterated_process->pid` immediately after is a use-after-free.

---

### BUG-26 — No idle thread; scheduler panics when all threads sleep

**Line:** 247
**Severity:** High

```c
} else {
    panic("scheduler_get_next_thread: No runable threads found");
```

If every thread is sleeping or waiting (a common legitimate state), the scheduler panics instead of running an idle thread. This makes the system unusable once any workload causes all threads to block simultaneously.

---

### BUG-27 — PID/TID allocation is not atomic on SMP

**Lines:** 22–68
**Severity:** High

`scheduler_get_free_pid_locked` and `scheduler_get_free_tid_locked` use a `static` `last_pid`/`last_tid` variable with no atomic operations or locks. On a multi-core system, two CPUs can simultaneously find the same ID to be "free" and assign the same PID/TID to different processes/threads.

---

### BUG-28 — `scheduler_sigreturn` panics on multiple pending signals

**Line:** 347

```c
void scheduler_sigreturn(cpu_context_t* ctx, thread_t * thread) {
    if (thread->scontext) panic("scheduler_sigreturn: thread still has a signal context");
```

After `process_sigret` removes the completed signal context, if a second signal was already enqueued its `sigctx_t` is still in `thread->scontext`. `scheduler_sigreturn` then panics even though this is a normal, expected state (multiple signals pending).

---

## 7. Signals & Sleep

**File:** [process/signals.c](kernel/src/process/signals.c)

---

### BUG-29 — Lost wakeup race condition in `sleep`

**Lines:** 122–131
**Severity:** High

```c
void sleep(thread_t * thread, int condition) {
    new(condition, thread, NULL, NULL);        // (1) register sleep
    thread->state = SCHEDULER_STATUS_INTERRUPTIBLE_SLEEP;  // (2) mark sleeping
    while (thread->state == SCHEDULER_STATUS_INTERRUPTIBLE_SLEEP) {
        __asm__ volatile("int $0x81");         // (3) yield
    }
}
```

If `wakeup(condition)` is called by another thread (or interrupt handler) between steps (1) and (2) — after the node is added but before the state is set — the wakeup finds the thread not yet sleeping and leaves it running. The thread then reaches step (2), sets itself to sleeping, and never wakes up because the wakeup event is already gone.

---

### BUG-30 — `nanosleep` ignores signal interruptions and does not fill `rem`

**Lines:** 147–157
**Severity:** Medium

The function loops until the thread is no longer sleeping, then returns `SUCCESS` unconditionally. POSIX `nanosleep` must return `-EINTR` if interrupted by a signal and must write the remaining time to `rem`. Neither is implemented.

---

### BUG-31 — `remove` determines the wrong list when duration is 0

**Lines:** 80–91

When a node is removed, the function decides which list head to update based on the node's `duration` fields. If `duration` was set to non-zero at insertion but subsequently modified to zero (e.g., the counter expires and the struct is reused), the wrong list head is updated, corrupting both the sleeping and waiting lists.

---

### BUG-32 — `generate_id` can loop indefinitely

**Lines:** 133–145

```c
int generate_id(thread_t* thread) {
    int current_id = (int)(uintptr_t)thread;
    sleeping_thread_t * current = sleeping_threads_head;
    while (current) {
        if (current->id == current_id) {
            current_id++;
            current = sleeping_threads_head;   // restart from head every collision
        } else {
            current = current->next;
        }
    }
```

Every collision resets to the head of the list. If all IDs in `[thread_addr, INT_MAX]` are taken, the loop never terminates, hanging the kernel.

---

## 8. System Calls

**File:** [arch/x86/syscall.c](kernel/src/arch/x86/syscall.c)

---

### BUG-33 — No user-pointer validation in any syscall (critical)

**Severity:** Critical

Every syscall that accepts a pointer from userspace (`buf`, `path`, `tp`, `status`, etc.) uses it directly with no validation:

```c
// syscall_read:
void *buf = (void *)SYSCALL_ARG1(context);
ssize_t ret = vfs_read(desc, buf, count);

// syscall_log:
char * message = (char *)SYSCALL_ARG0(ctx);
kprintf("[SYSCALL LOG] %s\n", message);

// syscall_arch_prctl ARCH_GET_FS:
int64_t* addr = (int64_t*)SYSCALL_ARG1(context);
*addr = thread->context->fs_base;
```

A userspace process can pass any 64-bit value as a pointer, including kernel addresses. This allows:
- Reading arbitrary kernel memory via `read`, `pread`, `getcwd`
- Writing arbitrary kernel memory via `write`, `stat`, `fstat`, `arch_prctl`, `waitpid`
- Kernel panic via a NULL pointer passed to `vfs_read` / `vfs_write` (which panic on NULL buf)

---

### BUG-34 — `syscall_kill` with out-of-range signal number causes OOB array access

**Lines:** 389–404
**Severity:** High

```c
int sig = (int)SYSCALL_ARG1(context);
...
status_t st = process_kill(target_proc, sig);
```

`process_kill` accesses `process->signal_queue[code]` with no bounds check. If `sig < 0` or `sig >= NSIG`, this is an out-of-bounds array access, potentially reading or writing arbitrary kernel memory adjacent to the `signal_queue` array.

---

### BUG-35 — `syscall_mmap` rejects `PROT_NONE` (== 0) before handling it

**Lines:** 183–210
**Severity:** Medium

```c
if (prot == 0x0) return -EINVAL;            // rejects PROT_NONE
...
if (prot & PROT_NONE) panic("...");         // dead code, never reached
```

`PROT_NONE` is defined as 0. Line 183 rejects it with EINVAL before line 210's panic is reached. Programs that need guard pages (a common mprotect use case) cannot create PROT_NONE mappings.

---

### BUG-36 — `syscall_mmap` panics on `MAP_SHARED` instead of returning an error

**Line:** 209
**Severity:** Medium

```c
if (flags & MAP_SHARED) panic("syscall_mmap: MAP_SHARED not implemented yet");
```

`MAP_SHARED` is a valid and common mmap flag. Any userspace application that uses it (e.g., shared memory between processes, memory-mapped I/O) causes a kernel panic instead of receiving `ENOSYS` or `EINVAL`.

---

### BUG-37 — Memory leak in `syscall_execve` on both success and failure

**Lines:** 349–362
**Severity:** Medium

```c
char * kfilename = kmalloc(fname_len + 1);
char ** kargv = duplicate_argv(...);
char ** kenvp = duplicate_envp(...);

status_t st = process_execve(..., kfilename, kargv, kenvp);
if (st != SUCCESS) {
    return -EIO;   // ← kfilename, kargv, kenvp leaked
}
return 0;          // ← kfilename, kargv, kenvp still leaked (process_execve copies them again)
```

On failure, all three allocations are leaked. On success, `process_execve` calls `duplicate_args` which makes additional copies, so `kfilename`, `kargv`, and `kenvp` are also leaked.

Additionally, `duplicate_argv`/`duplicate_envp` allocate individual strings that are only freed if the outer array is freed, which never happens.

---

### BUG-38 — `syscall_chdir` does not verify the directory exists

**Lines:** 442–452

```c
int len = strlen(path);
if (len >= VFS_PATH_MAX) return -ENAMETOOLONG;
strncpy(proc->cwd.internal_path, path, len);
return 0;
```

The path is stored without checking that the directory exists. A process can set its cwd to any string, including non-existent paths or kernel-internal paths.

---

### BUG-39 — `strncpy` call in `syscall_chdir` does not null-terminate

**Line:** 450

The local `strncpy` (see BUG-46) writes exactly `n` bytes then appends a null at index `n`. When `len == VFS_PATH_MAX - 1`, the null terminator is written at `internal_path[VFS_PATH_MAX - 1]`, which is the last valid byte — acceptable. But when called with `len` equal to exactly `VFS_PATH_MAX - 1` and the source has no null within that range, the result is not null-terminated within the allocated buffer. The check `if (len >= VFS_PATH_MAX) return -ENAMETOOLONG` uses strict `>=`, so a path of exactly `VFS_PATH_MAX - 1` characters passes but the null terminator lands at index `VFS_PATH_MAX - 1`, which is the final valid byte of the buffer. The off-by-one is fine here, but see BUG-46 for the general strncpy safety issue.

---

## 9. Interrupt Handling

**File:** [arch/x86/idt.c](kernel/src/arch/x86/idt.c)

---

### BUG-40 — Page fault COW handler does not validate the faulting process

**Lines:** 121–138

```c
} else if (ctx->interrupt_number == 14) {
    ...
    process_t * current_process = 0x0;
    thread_t *  current_thread = (thread_t *)ctx->ctx_info->thread;
    if (current_thread) {
        current_process = current_thread->process;
    } else {
        exception(ctx);
    }
    status_t status = vmarea_try_cow(current_process, (void *)cr2);
```

If the page fault fires while in kernel mode (accessing kernel memory), `current_thread` may be non-NULL but `current_process->vm_areas` contains only user VMAs. `vmarea_try_cow` will return `FAILURE` and the kernel will then call `exception(ctx)` and panic — but only after potentially dereferencing bad pointers inside `vmarea_find`. A kernel-mode page fault should be handled differently from a user-mode COW fault.

---

### BUG-41 — EOI sent after context switch for dynamic interrupts

**Line:** 161

```c
    apic_local_eoi(cpu_id);
    return;
}
```

For the scheduler interrupt (`INT_SCHEDULE_APIC_TIMER`, line 145-146), `scheduler_handler` switches CPU context by modifying `ctx` in place. The iret at the end of the interrupt stub returns to the new thread. The EOI at line 161 is executed before that iret, so it is sent correctly. However, for `syscall_exit` and `syscall_thread_exit` which trigger `int $0x40` inside kernel mode, the outer interrupt handler's EOI is bypassed if `process_exit` schedules a different thread that never returns here. Depending on the assembly stub structure this may leave the APIC in a state where it cannot deliver further timer interrupts.

---

## 10. Virtual Memory Engine

**File:** [arch/x86/vm.c](kernel/src/arch/x86/vm.c)

---

### BUG-42 — `vm_deallocate_vspace` is a no-op (critical memory leak)

**Lines:** 563–567
**Severity:** Critical

```c
status_t vm_deallocate_vspace(vm_dir * root) {
    (void)root;
    // panic("vm_deallocate_vspace: Not yet implemented");
    return SUCCESS;
}
```

This is called by `vmm_free_root` → `process_destroy`. Every process that exits permanently leaks its entire page table tree (PML4 → PDPT → PD → PT pages, all allocated with `pmm_alloc_pages`). On a long-running system this exhausts physical memory.

---

### BUG-43 — `vm_get_page_info` has a duplicate IS_PRESENT check

**Lines:** 241–248

```c
pml4entry = GET_ENTRY(root, indices.PML4_index);
if (!IS_PRESENT(pml4entry)) {
    panic("vm_get_page_info: PML4 entry not present");   // first check, correct
}

if (!IS_PRESENT(pml4entry)) {                            // second identical check
    panic("vm_get_page_info: PML4 entry not present");
} else if (pml4entry->huge.PS) {
```

The second block is dead code since the first block would have already panicked. The intent was likely to check the `huge.PS` bit in a separate branch, but the PML4 level cannot have huge pages in x86-64, making this logic confusing.

---

### BUG-44 — `vm_check_and_clean_dirty` returns inverted values for huge pages

**Lines:** 345–354

For 4 KB pages the function correctly returns the `D` bit (1 = dirty):
```c
uint8_t was_dirty = ptentry->regular.D;
...
return was_dirty;
```

For 1 GB and 2 MB huge pages it returns:
```c
return pdptentry->huge.D ? SUCCESS : FAILURE;
```

If `SUCCESS = 0` and `FAILURE = 1` (the common convention), this returns 0 when the page IS dirty and 1 when it is NOT dirty — the opposite of the 4 KB path. `vmm_check_and_clean_dirty` checks `if (dirty == 1)`, which will miss dirty huge pages and incorrectly flag clean ones.

---

### BUG-45 — Intermediate page table entries always have user+write bits set

**Lines:** 386–391, 407

```c
vm_perms perms;
perms.read_write = 1;
perms.user = 1;
...
init_entry(pml4entry, VM_PAGE_SIZE_DIR, (uint64_t)pdptable, perms);
```

All intermediate page table levels (PML4, PDPT, PD entries that point to lower tables) are created with `US=1` and `RW=1` regardless of the final page's permissions. This means:
- Kernel pages that should be inaccessible from user mode have their directory entries marked user-accessible. The leaf page entry's `US=0` still blocks user access for those pages.
- But if SMEP/SMAP is ever enabled, this could interact unexpectedly.

More importantly, directory entries for kernel mappings should have `US=0` for defence in depth.

---

## 11. VFS

**File:** [vfs/vfs.c](kernel/src/vfs/vfs.c)

---

### BUG-46 — `vfs_close` leaks `native_path` on duplicate close

**Lines:** 218–232

`vfs_close` always calls `kfree(fd->native_path)`. If the same `vfs_file_descriptor_t` slot is closed twice (e.g., via `dup2` overwriting a slot, then the original fd being closed), `native_path` is freed twice — a double free.

---

### BUG-47 — `vfs_find_mount` panics on same-length mount points

**Line:** 150

```c
} else if (mount_point_len == best_candidate.mount_point_len) {
    panic("vfs_find_mount: Multiple mount points with same length match path");
}
```

Two mount points of the same length that both prefix-match the path are valid (e.g., `/dev` and `/tmp` are both 4 characters but only one would match a given path). This check should only panic if the mount points themselves are identical. As written, it panics for any two equal-length mount points that both happen to match the same path — a situation that can legitimately arise.

---

### BUG-48 — `vfs_remove_mount` has duplicate NULL pointer check

**Lines:** 98–103

```c
if (mount_point == NULL) {
    panic("vfs_remove_mount: mount_point is NULL");
}
if (mount_point == NULL) {           // dead code
    panic("vfs_remove_mount: mount_point is NULL");
}
```

The second check is dead code.

---

### BUG-49 — `vfs_close` does not call a filesystem-level close callback

**Lines:** 218–232

The VFS close operation frees the `native_path` allocation and marks the descriptor invalid, but it never calls `fd->mount->ops->close` (no such function pointer exists in the `vfs_fs_t` struct). Filesystem drivers have no way to flush write buffers or release internal handles when a file is closed.

---

## 12. Standard Library

**File:** [libraries/std/strings.c](kernel/src/libraries/std/strings.c)

---

### BUG-50 — `strncpy` writes `n + 1` bytes (off-by-one overflow)

**Lines:** 332–342
**Severity:** High

```c
void strncpy(char *dest, const char *src, uint64_t n) {
    uint64_t i = 0;
    if (strlen(src) < n) { n = strlen(src); }
    while (i < n && src[i] != '\0') {
        dest[i] = src[i];
        i++;
    }
    dest[i] = '\0';   // ← writes byte at index i, which may equal n
}
```

When `src` is at least `n` bytes long, the loop copies `n` bytes (indices 0..n-1). Then `dest[n] = '\0'` writes a byte at index `n` — one past the caller-specified bound. Any call where `dest` is exactly `n` bytes large overflows by one byte.

This affects `syscall_execve` (`strncpy(kfilename, filename, fname_len + 1)`), `duplicate_argv`, `duplicate_envp`, `syscall_chdir`, and `syscall_getcwd`.

---

### BUG-51 — `atou64` fails to parse hex digits

**Lines:** 190–214
**Severity:** Medium

```c
while (!isdigit(*nptr)) {
    nptr++;   // skip all non-decimal-digit characters
}
```

After stripping the `0x` prefix, the function skips non-digit characters to "find" the number. For a hex string like `"0xABCD"`, after removing `"0x"`, `nptr` points to `"ABCD"`. `isdigit('A')` is false, so the loop advances past all hex digits, leaving `nptr` pointing to `""`. The result is always 0 for any hex value whose digits are entirely `A`–`F`.

---

### BUG-52 — `load32` has undefined behaviour when shifting `uint8_t` by 24

**Lines:** 439–447

```c
value |= (*src_ptr++ << 24);
```

`*src_ptr++` is `uint8_t`, which promotes to `int` (signed 32-bit) under C integer promotion rules. For values ≥ 128, the shift `(int)0x80 << 24 = 0x80000000` overflows a signed 32-bit integer, which is undefined behaviour in C. The correct cast is `(uint32_t)(*src_ptr++) << 24`.

---

### BUG-53 — `memcmp` does not return negative/positive values

**Lines:** 363–372

```c
uint64_t memcmp(const void *dest, const void *src, uint64_t size) {
    ...
    if (d[i] != s[i]) { return 1; }
    ...
    return 0;
}
```

Standard `memcmp` must return a negative, zero, or positive integer. This version returns only 0 or 1, and uses `uint64_t` as the return type (unsigned). Any code using `memcmp` for ordering comparisons (e.g., `if (memcmp(a, b, n) < 0)`) will always fail.

---

### BUG-54 — `strtok` uses a single `static` variable (not re-entrant)

**Lines:** 272–303

`strtok` stores state in `static char* lastToken`. Any concurrent call from a different thread, or nested calls (e.g., from an interrupt handler), corrupts the shared state. In a multithreaded kernel this is a data race.

---

## 13. Spinlock

**File:** [libraries/lock/spinlock.c](kernel/src/libraries/lock/spinlock.c)

---

### BUG-55 — Interrupts disabled AFTER acquiring the lock

**Lines:** 8–11
**Severity:** High

```c
if (spinlock_test_and_acq(lock)) {
    __asm__("cli");       // ← interrupts disabled AFTER lock acquired
    global_spinlock_counter++;
    break;
}
```

Between the test-and-set succeeding and the `cli` executing, a timer interrupt can fire. If the interrupt handler attempts to acquire the same spinlock, it will spin forever while holding the CPU — a deadlock. The `cli` must precede the test-and-set.

---

### BUG-56 — `global_spinlock_counter` is unsynchronized on SMP

**Line:** 10

```c
global_spinlock_counter++;
```

This is a non-atomic read-modify-write on a shared variable. On a multi-core system two CPUs can simultaneously increment the counter, causing lost updates.

---

## 14. CPU Initialization

**File:** [arch/x86/cpu.c](kernel/src/arch/x86/cpu.c)

---

### BUG-57 — TSS kernel stack is not tracked and is leaked

**Lines:** 50–54

```c
void cpu_tss_init(core_context_t * cpu_ctx) {
    cpu_ctx->cpu_tss = (tss_t*)kmalloc(sizeof(tss_t));
    stack_t * kernel_stack = kstackalloc(vmm_get_root(), KERNEL_STACK_SIZE);
    ...
    cpu_ctx->cpu_tss->rsp[0] = (uint64_t)(kernel_stack->top);
    // kernel_stack itself is never stored, so it cannot be freed
}
```

The `stack_t*` returned by `kstackalloc` is stored only in a local variable. It is never stored in `cpu_ctx` or elsewhere. The allocation is permanently leaked, and the associated physical pages cannot be reclaimed.

---

### BUG-58 — `cpu_context_init` allocates `cinfo` but does not initialize it

**Lines:** 58–63

```c
core_context_t * ctx = (core_context_t *)kmalloc(sizeof(core_context_t));
ctx->core_id = getApicId();
ctx->cinfo = (context_info_t *)kmalloc(sizeof(context_info_t));
// cinfo is never initialized (memset or field assignment)
```

`ctx->cinfo` is allocated but not zeroed or initialized. `cpu_get_current_thread` reads `ctx->cinfo->thread` which will contain garbage until the first explicit assignment.

---

### BUG-59 — Application processors are never properly initialized

**Lines:** 71–76, 112–113

```c
void callback(boot_smp_info_t *lcpu) {
    (void)lcpu;
    while (1) { __asm__("hlt"); }
}

// in cpu_init():
cpu->goto_address = (void*)(uint64_t)callback;
```

APs are given a trampoline that halts them immediately. They never execute `cpu_init_id`, so they have no IDT, no GDT, no APIC initialization, and no syscall support. The kernel claims SMP support but all non-BSP cores are permanently halted.

---

## 15. Summary Table

| ID | File | Line(s) | Category | Severity |
|----|------|---------|----------|----------|
| BUG-01 | allocator.c | 163 | Logic flaw / false panic | High |
| BUG-02 | allocator.c | 141–151 | Logic flaw (debug) | Low |
| BUG-03 | allocator.c | 416–486 | Memory leak / dead code | Medium |
| BUG-04 | process.c | 987–993 | Memory leak | Medium |
| BUG-05 | allocator.c | 237–241 | Logic flaw (dead branch) | Low |
| BUG-06 | allocator.c | 351–353 | Dead code | Low |
| BUG-07 | pmm.c | 113–147 | Missing double-free detection | Medium |
| BUG-08 | pmm.c | 43–58 | Init ordering hazard | Medium |
| BUG-09 | mmap.c | 116–155 | **Data corruption (COW)** | **Critical** |
| BUG-10 | mmap.c | 282 | Logic flaw (always FAILURE) | Medium |
| BUG-11 | mmap.c | 351–366 | Panic on valid munmap call | High |
| BUG-12 | mmap.c | 96 | Memory leak (multi-page fork) | Medium |
| BUG-13 | loader.c | 180–182 | **Stack overflow (128 KB VLA)** | **Critical** |
| BUG-14 | loader.c | 508, 553–554 | **Heap buffer overflow** | **Critical** |
| BUG-15 | loader.c | 382–383 | **Use-after-free** | High |
| BUG-16 | loader.c | 324 | OOB read/write from ELF data | High |
| BUG-17 | loader.c | 225–236 | Size check too late | Medium |
| BUG-18 | process.c | 1048–1050 | Memory leak (argv strings) | Medium |
| BUG-19 | process.c | 1044–1046 | Memory leak (signal actions) | Medium |
| BUG-20 | process.c | 1097–1101 | NULL deref / panic | High |
| BUG-21 | process.c | 1182 | FD leak on dup2 | Medium |
| BUG-22 | process.c | 343 | Panic on second signal | High |
| BUG-23 | process.c | 788–791 | FD copy with wrong loop limit | Medium |
| BUG-24 | scheduler.c | 156–166 | Logic flaw (waitpid WNOHANG) | High |
| BUG-25 | scheduler.c | 161–166 | **Use-after-free** | High |
| BUG-26 | scheduler.c | 247 | Panic with no runable threads | High |
| BUG-27 | scheduler.c | 22–68 | Race condition (PID/TID SMP) | High |
| BUG-28 | scheduler.c | 347 | Spurious panic (multi-signal) | High |
| BUG-29 | signals.c | 122–131 | **Race condition (lost wakeup)** | High |
| BUG-30 | signals.c | 147–157 | Missing EINTR / rem handling | Medium |
| BUG-31 | signals.c | 80–91 | List corruption (wrong head) | High |
| BUG-32 | signals.c | 133–145 | Infinite loop possible | Medium |
| BUG-33 | syscall.c | all | **No user-pointer validation** | **Critical** |
| BUG-34 | syscall.c | 389–404 | OOB array access via kill | High |
| BUG-35 | syscall.c | 183–210 | PROT_NONE rejected incorrectly | Medium |
| BUG-36 | syscall.c | 209 | Panic on MAP_SHARED | Medium |
| BUG-37 | syscall.c | 349–362 | Memory leak on execve | Medium |
| BUG-38 | syscall.c | 442–452 | chdir without dir validation | Medium |
| BUG-39 | syscall.c | 450 | strncpy edge case | Low |
| BUG-40 | idt.c | 121–138 | Kernel PF mishandled as COW | High |
| BUG-41 | idt.c | 161 | EOI ordering on nested int | Medium |
| BUG-42 | vm.c | 563–567 | **Memory leak (page tables never freed)** | **Critical** |
| BUG-43 | vm.c | 241–248 | Dead duplicate check | Low |
| BUG-44 | vm.c | 345–354 | Inverted dirty bit for huge pages | Medium |
| BUG-45 | vm.c | 386–391 | Overly permissive dir entries | Low |
| BUG-46 | vfs.c | 218–232 | Double free on dup + close | High |
| BUG-47 | vfs.c | 150 | Spurious panic (same-len mounts) | Medium |
| BUG-48 | vfs.c | 98–103 | Dead code (double NULL check) | Low |
| BUG-49 | vfs.c | 218–232 | Missing fs-level close callback | Medium |
| BUG-50 | strings.c | 332–342 | **Off-by-one buffer overflow** | High |
| BUG-51 | strings.c | 190–214 | Hex parsing always returns 0 | Medium |
| BUG-52 | strings.c | 439–447 | Signed shift UB (load32) | Medium |
| BUG-53 | strings.c | 363–372 | memcmp returns wrong type | Low |
| BUG-54 | strings.c | 272–303 | strtok not re-entrant | High |
| BUG-55 | spinlock.c | 8–11 | **Race: CLI after lock acquire** | High |
| BUG-56 | spinlock.c | 10 | Unsynchronized counter on SMP | Medium |
| BUG-57 | cpu.c | 50–54 | Memory leak (TSS stack) | Medium |
| BUG-58 | cpu.c | 58–63 | Uninitialized cinfo | Medium |
| BUG-59 | cpu.c | 71–76, 112–113 | APs never initialized | High |

### Severity breakdown

| Severity | Count |
|----------|-------|
| Critical | 7 |
| High | 25 |
| Medium | 21 |
| Low | 9 |
| **Total** | **62** |

### Critical issues requiring immediate attention

1. **BUG-09** — COW fault corrupts data for every multi-page private mapping
2. **BUG-13** — 128 KB stack allocation in `loader_create_args` always overflows the kernel stack
3. **BUG-14** — Heap buffer overflow writing auxv entry 7 into unallocated memory
4. **BUG-33** — No user-pointer validation in any syscall; arbitrary kernel read/write from userspace
5. **BUG-42** — Page tables are never freed; every process exit leaks its entire address space
6. **BUG-15** — Use-after-free reading `e_entry` after `kfree(elf_datab)`
7. **BUG-50** — `strncpy` writes one byte past the specified bound; affects every string copy site
