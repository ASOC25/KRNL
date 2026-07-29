[BITS 64]

; Signal restorer blob mapped at 0x80000000 in every process.
; Called after the signal handler returns (as the return address on the user stack).
; Just invokes rt_sigreturn (syscall 49) to restore the pre-signal context.

global signal_trampoline_start
global signal_trampoline
global signal_trampoline_end

align 16
signal_trampoline_start:
signal_trampoline:
    mov eax, 49     ; SYS_SIGRET = rt_sigreturn
    syscall
signal_trampoline_end:
