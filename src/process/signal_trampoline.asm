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
;   void signal_trampoline(int signo, sigaction_t *sigact, cpu_context_t *ctx);
; SysV ABI args: rdi, rsi, rdx
signal_trampoline:
    ret

signal_trampoline_end:
