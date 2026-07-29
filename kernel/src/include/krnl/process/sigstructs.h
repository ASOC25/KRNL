#ifndef _SIGSTRUCTS_H
#define _SIGSTRUCTS_H

#include <krnl/libraries/std/stdint.h>

#define NSIG		65

#define SIGHUP		 1
#define SIGINT		 2
#define SIGQUIT		 3
#define SIGILL		 4
#define SIGTRAP		 5
#define SIGABRT		 6
#define SIGIOT		 6
#define SIGBUS		 7
#define SIGFPE		 8
#define SIGKILL		 9
#define SIGUSR1		10
#define SIGSEGV		11
#define SIGUSR2		12
#define SIGPIPE		13
#define SIGALRM		14
#define SIGTERM		15
#define SIGSTKFLT	16
#define SIGCHLD		17
#define SIGCONT		18
#define SIGSTOP		19
#define SIGTSTP		20
#define SIGTTIN		21
#define SIGTTOU		22
#define SIGURG		23
#define SIGXCPU		24
#define SIGXFSZ		25
#define SIGVTALRM	26
#define SIGPROF		27
#define SIGWINCH	28
#define SIGIO		29
#define SIGPOLL		SIGIO

#define SIGPWR		30
#define SIGSYS		31
#define	SIGUNUSED	31
#define SIGRTMIN	32
#define SIGRTMAX	_NSIG

#define SA_NOCLDSTOP  1
#define SA_NOCLDWAIT  2
#define SA_SIGINFO    4
#define SA_ONSTACK    0x08000000
#define SA_RESTART    0x10000000
#define SA_NODEFER    0x40000000
#define SA_RESETHAND  0x80000000
#define SA_RESTORER   0x04000000

#define MINSIGSTKSZ	2048
#define SIGSTKSZ	8192

#define SIG_BLOCK    0
#define SIG_UNBLOCK  1
#define SIG_SETMASK  2

#define SIG_DFL     0
#define SIG_IGN     1
#define SIG_ERR     2
#define SIG_HOLD    3

#define SIGNAL_SLEEP_INTERRUPT 0x81
#define SIGNAL_SIGRETURN_INTERRUPT 0x82
#define SIGNAL_WAITPID 0x1000

typedef unsigned long sigset_t;

/* siginfo_t: 128-byte musl/mlibc-compatible layout */
typedef struct {
    int si_signo;
    int si_errno;
    int si_code;
    uint8_t __pad[128 - 3 * sizeof(int)];
} siginfo_t;

/* sigaction_t: layout matches mlibc's struct sigaction for x86-64.
   sa_handler and sa_sigaction share the same address (anonymous union). */
typedef struct sigaction {
    union {
        void (*sa_handler)(int);
        void (*sa_sigaction)(int, siginfo_t *, void *);
    };
    unsigned long sa_flags;
    void (*sa_restorer)(void);
    unsigned long sa_mask[16]; /* mlibc sigset_t: 128 bytes, sig[0] = signals 1-64 */
} sigaction_t;

typedef struct signal {
    int signo;
    siginfo_t info;
    struct signal * next;
} signal_t;

/* ── Unix signal frame types (x86-64) ─────────────────────────────────── */

/* Alternate-stack descriptor (matches mlibc stack_t) */
typedef struct {
    void     *ss_sp;
    int       ss_flags;
    uint64_t  ss_size;
} sigstack_t;

/* mcontext_t register array indices (match mlibc REG_* constants) */
#define MC_R8      0
#define MC_R9      1
#define MC_R10     2
#define MC_R11     3
#define MC_R12     4
#define MC_R13     5
#define MC_R14     6
#define MC_R15     7
#define MC_RDI     8
#define MC_RSI     9
#define MC_RBP     10
#define MC_RBX     11
#define MC_RDX     12
#define MC_RAX     13
#define MC_RCX     14
#define MC_RSP     15
#define MC_RIP     16
#define MC_EFL     17
#define MC_CSGSFS  18
#define MC_ERR     19
#define MC_TRAPNO  20
#define MC_OLDMASK 21
#define MC_CR2     22
#define K_NGREG    23

/* mcontext_t: matches mlibc's mcontext_t for x86-64 */
typedef struct {
    uint64_t gregs[K_NGREG];
    void    *fpregs;
    uint64_t __reserved1[8];
} k_mcontext_t;

/* ucontext_t: matches mlibc's ucontext_t for x86-64.
   uc_sigmask is padded to 128 bytes to match mlibc's sigset_t. */
typedef struct k_ucontext {
    uint64_t          uc_flags;
    struct k_ucontext *uc_link;
    sigstack_t         uc_stack;
    k_mcontext_t       uc_mcontext;
    sigset_t           uc_sigmask;
    uint64_t           __uc_sigmask_pad[15]; /* pad to 128 bytes total */
} k_ucontext_t;

/* rt_sigframe: pushed on user stack on signal delivery */
struct rt_sigframe {
    siginfo_t    info;  /* 128 bytes, matches mlibc siginfo_t */
    k_ucontext_t uc;
};

#endif