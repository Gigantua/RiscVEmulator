#pragma once

/* Signal numbers */
#define SIGHUP   1
#define SIGINT   2
#define SIGQUIT  3
#define SIGILL   4
#define SIGTRAP  5
#define SIGABRT  6
#define SIGBUS   7
#define SIGFPE   8
#define SIGKILL  9
#define SIGUSR1  10
#define SIGSEGV  11
#define SIGUSR2  12
#define SIGPIPE  13
#define SIGALRM  14
#define SIGTERM  15

/* Signal action flags */
#define SA_SIGINFO  0x00000004
#define SA_RESTART  0x10000000

/* FPE codes */
#define FPE_INTDIV  1
#define FPE_INTOVF  2
#define FPE_FLTDIV  3
#define FPE_FLTOVF  4
#define FPE_FLTUND  5
#define FPE_FLTRES  6
#define FPE_FLTINV  7
#define FPE_FLTSUB  8

/* SEGV codes */
#define SEGV_MAPERR 1
#define SEGV_ACCERR 2

/* BUS codes */
#define BUS_ADRALN  1
#define BUS_ADRERR  2
#define BUS_OBJERR  3

typedef int sig_atomic_t;
typedef unsigned long sigset_t;

typedef struct siginfo {
    int si_signo;
    int si_errno;
    int si_code;
    union {
        struct { int si_pid; unsigned int si_uid; } _kill;
        struct { void *si_addr; } _sigfault;
        struct { int si_band; int si_fd; } _sigpoll;
    } _sifields;
} siginfo_t;
#define si_addr   _sifields._sigfault.si_addr
#define si_pid    _sifields._kill.si_pid

struct sigaction {
    union {
        void (*sa_handler)(int);
        void (*sa_sigaction)(int, siginfo_t *, void *);
    };
    sigset_t   sa_mask;
    int        sa_flags;
    void     (*sa_restorer)(void);
};

/* Stubs: no real signal handling on bare metal */
static inline int sigaction(int signum, const struct sigaction *act, struct sigaction *oldact)
{
    (void)signum; (void)act; (void)oldact;
    return -1;
}

static inline void (*signal(int signum, void (*handler)(int)))(int)
{
    (void)signum; (void)handler;
    return (void (*)(int))(-1);
}
