[BITS 64]

; Dummy userspace signal trampoline blob.
; The loader copies the bytes from signal_trampoline_start..signal_trampoline_end
; into a fixed userspace mapping (see allocate_signal_trampoline).

global signal_trampoline_start
global signal_trampoline
global signal_trampoline_end

align 16
signal_trampoline_start:

; C prototype (for when this becomes real):
;   void signal_trampoline(int signo, sigaction_t *sigact, cpu_context_t *ctx); Hidden arg stack pointer is also passed
; SysV ABI args: rdi, rsi, rdx, rcx, r8, r9
; rdi = signo
; rsi = sigaction_t *sigact
; rdx = cpu_context_t *ctx
; rcx = stack pointer (hidden arg) set now
; r8 = handler address (hidden arg) set now
signal_trampoline:
;Call sigret syscall (code 49)
;First zero registers
    xor rax, rax
    xor rbx, rbx
    xor r9, r9
    xor r10, r10
    xor r11, r11
    xor r12, r12
    xor r13, r13
    xor r14, r14
    xor r15, r15

; Set the new stack pointer from rcx
    mov rsp, rcx
; Jump to the handler address in r8
    call r8
; Now return to userspace with sigret
    mov rax, 0x31
    syscall
;Never reach this!

signal_trampoline_end: