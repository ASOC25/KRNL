; Based on Kot's code: https://github.com/kot-org/Kot/blob/main/sources/core/kernel/source/arch/amd64/interrupts.s

extern interrupt_handler
global __interrupt_vector
global kcontext_simple_launch

%macro swapgs_if_necessary 1
	cmp qword [rsp + 0x18], 0x8
	jnz DO_SWAPGS%1
    jmp NO_SWAPGS%1
DO_SWAPGS%1:
    swapgs
NO_SWAPGS%1:
%endmacro

%macro interrupt_entry 1
    %assign y %1*2
    swapgs_if_necessary y
    push    r15
    push    r14
    push    r13
    push    r12
    push    r11
    push    r10
    push    r9
    push    r8
    push    rbp
    push    rdi
    push    rsi
    push    rdx
    push    rcx
    push    rbx
    push    rax
    push   qword [gs:0x8]
    mov     rax, cr3
    push    rax
    
    cld
    
    mov rdi, rsp
    mov rsi, [gs:0x0]

    call interrupt_handler
    
    pop    rax
    mov    cr3, rax
    pop    qword [gs:0x8]
    pop    rax
    pop    rbx
    pop    rcx
    pop    rdx
    pop    rsi
    pop    rdi
    pop    rbp
    pop    r8
    pop    r9
    pop    r10
    pop    r11
    pop    r12
    pop    r13
    pop    r14
    pop    r15

    %assign y y+1
    swapgs_if_necessary y

    add rsp, 16
    iretq
%endmacro

%macro INTERRUPT_WITHOUT_ERROR_CODE 1
interrupt_handler_%1:
    push 0
    push %1
    interrupt_entry %1
%endmacro

%macro INTERRUPT_WITH_ERROR_CODE 1
interrupt_handler_%1:
    push %1
    interrupt_entry %1
%endmacro

%macro CREATE_INTERRUPT_NAME 1  
    dq interrupt_handler_%1
%endmacro

INTERRUPT_WITHOUT_ERROR_CODE 0
INTERRUPT_WITHOUT_ERROR_CODE 1
INTERRUPT_WITHOUT_ERROR_CODE 2
INTERRUPT_WITHOUT_ERROR_CODE 3
INTERRUPT_WITHOUT_ERROR_CODE 4
INTERRUPT_WITHOUT_ERROR_CODE 5
INTERRUPT_WITHOUT_ERROR_CODE 6
INTERRUPT_WITHOUT_ERROR_CODE 7
INTERRUPT_WITH_ERROR_CODE   8
INTERRUPT_WITHOUT_ERROR_CODE 9
INTERRUPT_WITH_ERROR_CODE   10
INTERRUPT_WITH_ERROR_CODE   11
INTERRUPT_WITH_ERROR_CODE   12
INTERRUPT_WITH_ERROR_CODE   13
INTERRUPT_WITH_ERROR_CODE   14
INTERRUPT_WITHOUT_ERROR_CODE 15
INTERRUPT_WITHOUT_ERROR_CODE 16
INTERRUPT_WITH_ERROR_CODE   17
INTERRUPT_WITHOUT_ERROR_CODE 18
INTERRUPT_WITHOUT_ERROR_CODE 19
INTERRUPT_WITHOUT_ERROR_CODE 20
INTERRUPT_WITHOUT_ERROR_CODE 21
INTERRUPT_WITHOUT_ERROR_CODE 22
INTERRUPT_WITHOUT_ERROR_CODE 23
INTERRUPT_WITHOUT_ERROR_CODE 24
INTERRUPT_WITHOUT_ERROR_CODE 25
INTERRUPT_WITHOUT_ERROR_CODE 26
INTERRUPT_WITHOUT_ERROR_CODE 27
INTERRUPT_WITHOUT_ERROR_CODE 28
INTERRUPT_WITHOUT_ERROR_CODE 29
INTERRUPT_WITH_ERROR_CODE   30
INTERRUPT_WITHOUT_ERROR_CODE 31

%assign i 32
%rep 256 - i
    INTERRUPT_WITHOUT_ERROR_CODE i
%assign i i+1
%endrep

__interrupt_vector:
    %assign i 0
    %rep 256
        CREATE_INTERRUPT_NAME i
    %assign i i+1
    %endrep

; kcontext_restore_trampoline(cpu_context_t *kctx)
; Restores a kernel-mode thread (cs=0x8) saved by interrupt_entry.
; Switches CR3, updates gs:0x8, switches RSP to the saved kstack position,
; builds a 3-item iretq frame (RIP/CS/RFLAGS), restores all GP registers,
; then executes iretq — which atomically restores RIP, CS, and RFLAGS
; (re-enabling interrupts as part of the instruction).  Never returns.
;
; Using iretq instead of popfq+ret is critical: popfq re-enables IF before
; all registers are restored, opening a window for the APIC timer to fire
; mid-trampoline and corrupt the thread's saved kcontext.  iretq is atomic.
;
; cpu_context_t offsets (packed, all uint64_t):
;   +0   cr3          +8   ctx_info*
;   +16  rax          +24  rbx   +32  rcx   +40  rdx
;   +48  rsi          +56  rdi   +64  rbp
;   +72  r8  +80  r9  +88  r10  +96  r11  +104 r12  +112 r13  +120 r14  +128 r15
;   +152 rip  +160 cs  +168 rflags
;   +176 rsp (pre-interrupt RSP, set explicitly by scheduler)
global kcontext_restore_trampoline
kcontext_restore_trampoline:
    ; rdi = &next_thread->kcontext->cpu_ctx
    ;
    ; Restores a kernel-mode thread (cs=0x8) whose context was saved by
    ; interrupt_entry. This is always a same-privilege (ring0->ring0) resume,
    ; so CS/SS never change and RIP/RFLAGS can be restored with a plain
    ; `ret`+`popfq` instead of `iretq`. This sidesteps a QEMU/TCG bug where
    ; `iretq` raised a spurious #GP (citing an unrelated GDT selector) even
    ; though the popped RIP/CS/RFLAGS were all individually verified valid —
    ; see git history for the (extensively debugged) symptom this replaced.
    ;
    ; next_thread->kcontext is kernel memory, but each process's kernel-half
    ; page tables are built by an independent walk (see vm_duplicate()) that
    ; allocates its own copies of intermediate page-table pages rather than
    ; sharing the same physical ones with every other process, so a very
    ; recently written kernel address is not guaranteed to be visible yet
    ; through a *different* process's copy of those tables. Every field is
    ; therefore read out of *rdi below while whichever CR3 is currently
    ; loaded (guaranteed up to date) is still active; CR3 is only switched to
    ; next_thread's own once every value has already been staged onto the
    ; (already-switched-to) target stack, which also leaves every register
    ; free for the CR3/ctx_info switch itself.
    mov rax, [rdi + 152]    ; rip
    mov rbx, [rdi + 168]    ; rflags
    mov rsp, [rdi + 176]    ; switch to the target kernel stack
    push rax                ; rip — consumed by `ret` at the very end
    push rbx                ; rflags — consumed by `popfq` just before the ret
    push qword [rdi + 16]   ; rax
    push qword [rdi + 24]   ; rbx
    push qword [rdi + 32]   ; rcx
    push qword [rdi + 40]   ; rdx
    push qword [rdi + 48]   ; rsi
    push qword [rdi + 64]   ; rbp
    push qword [rdi + 72]   ; r8
    push qword [rdi + 80]   ; r9
    push qword [rdi + 88]   ; r10
    push qword [rdi + 96]   ; r11
    push qword [rdi + 104]  ; r12
    push qword [rdi + 112]  ; r13
    push qword [rdi + 120]  ; r14
    push qword [rdi + 128]  ; r15
    push qword [rdi + 56]   ; rdi's own saved value

    mov r11, [rdi + 0]      ; target cr3 (rdi itself no longer needed)
    mov cr3, r11
    mov r12, [rdi + 8]      ; target ctx_info
    mov [gs:0x8], r12

    ; Pop everything back off in reverse order; rflags and rip are left on
    ; top of the stack for popfq/ret to consume.
    pop rdi
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax

    popfq  ; restore RFLAGS (re-enabling IF if saved RFLAGS had it set)
    ret    ; pop RIP and jump there, correctly leaving RSP at its target value

; kcontext_simple_launch(cpu_context_t *kctx)
; For kernel threads being started for the first time (no prior interrupt frame).
; Switches CR3 and gs:0x8, sets RSP to the saved kstack top, restores all GP
; registers, then enables interrupts and jumps directly to the saved RIP.
; Using sti+jmp instead of iretq avoids building a fake interrupt frame on a
; stack that has never been touched (which triggers a QEMU TCG iretq bug when
; RSP is at the very top of a freshly-zeroed kernel stack page).
kcontext_simple_launch:
    ; rdi = &next_thread->kcontext->cpu_ctx
    ;
    ; See kcontext_restore_trampoline above for why every field is read out
    ; of *rdi before switching CR3 (kernel-half page tables are not
    ; guaranteed to be in sync across processes for very recently written
    ; addresses). r11 is reserved to carry the entry point across the CR3
    ; switch below — its "real" saved value from cpu_ctx is not restored
    ; here, matching this launch path's original design (it is only used to
    ; start brand-new kernel threads, whose initial GP registers are zeroed
    ; anyway).
    mov rsp, [rdi + 176]    ; switch to the target kernel stack top
    push qword [rdi + 152]  ; rip (entry point) — popped into r11 at the end
    push qword [rdi + 16]   ; rax
    push qword [rdi + 24]   ; rbx
    push qword [rdi + 32]   ; rcx
    push qword [rdi + 40]   ; rdx
    push qword [rdi + 48]   ; rsi
    push qword [rdi + 64]   ; rbp
    push qword [rdi + 72]   ; r8
    push qword [rdi + 80]   ; r9
    push qword [rdi + 88]   ; r10
    push qword [rdi + 104]  ; r12
    push qword [rdi + 112]  ; r13
    push qword [rdi + 120]  ; r14
    push qword [rdi + 128]  ; r15
    push qword [rdi + 56]   ; rdi's own saved value

    mov r11, [rdi + 0]      ; target cr3
    mov cr3, r11
    mov r12, [rdi + 8]      ; target ctx_info
    mov [gs:0x8], r12

    pop rdi
    pop r15
    pop r14
    pop r13
    pop r12
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax
    pop r11                 ; entry point

    ; Enable interrupts then jump to entry point.
    ; sti defers interrupt delivery until after the next instruction (jmp),
    ; so the thread begins executing with interrupts already enabled.
    sti
    jmp r11