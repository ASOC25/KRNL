; Based on Kot's code: https://github.com/kot-org/Kot/blob/main/sources/core/kernel/source/arch/amd64/interrupts.s

extern interrupt_handler
global __interrupt_vector

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
; Restores a kernel-mode thread whose context was saved by interrupt_entry.
; Switches RSP to the target's pre-interrupt RSP (stored in kctx->rsp by the
; scheduler), builds an iretq frame at that RSP, restores all registers, and
; executes iretq.  Never returns.
;
; cpu_context_t offsets (packed, all uint64_t):
;   +0   cr3          +8   ctx_info*
;   +16  rax          +24  rbx   +32  rcx   +40  rdx
;   +48  rsi          +56  rdi   +64  rbp
;   +72  r8  +80  r9  +88  r10  +96  r11  +104 r12  +112 r13  +120 r14  +128 r15
;   +152 rip         +160 cs           +168 rflags
;   +176 rsp (pre-interrupt RSP, set explicitly by scheduler)
global kcontext_restore_trampoline
kcontext_restore_trampoline:
    ; rdi = &next_thread->kcontext->cpu_ctx

    ; Switch to target thread's pre-interrupt RSP
    mov rsp, [rdi + 176]

    ; Build iretq frame below that RSP (kernel→kernel: only RIP/CS/RFLAGS needed)
    push qword [rdi + 168]  ; RFLAGS
    push qword [rdi + 160]  ; CS
    push qword [rdi + 152]  ; RIP

    ; Restore CR3
    mov rax, [rdi + 0]
    mov cr3, rax

    ; Restore ctx_info into [gs:0x8]
    mov rax, [rdi + 8]
    mov [gs:0x8], rax

    ; Restore general-purpose registers (rax and rdi last)
    mov rax, [rdi + 16]
    mov rbx, [rdi + 24]
    mov rcx, [rdi + 32]
    mov rdx, [rdi + 40]
    mov rsi, [rdi + 48]
    mov rbp, [rdi + 64]
    mov r8,  [rdi + 72]
    mov r9,  [rdi + 80]
    mov r10, [rdi + 88]
    mov r11, [rdi + 96]
    mov r12, [rdi + 104]
    mov r13, [rdi + 112]
    mov r14, [rdi + 120]
    mov r15, [rdi + 128]
    mov rdi, [rdi + 56]

    iretq