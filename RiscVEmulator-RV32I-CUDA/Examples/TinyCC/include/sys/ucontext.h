#pragma once

/* RISC-V register indices in gregs array */
#define REG_PC    0
#define REG_RA    1
#define REG_SP    2
#define REG_GP    3
#define REG_TP    4
#define REG_T0    5
#define REG_T1    6
#define REG_T2    7
#define REG_S0    8
#define REG_FP    8   /* s0/fp */
#define REG_S1    9
#define REG_A0    10
#define REG_A1    11

#define NGREG  32

typedef unsigned long gregset_t[NGREG];

struct mcontext_t {
    gregset_t __gregs;
};

typedef struct {
    unsigned long   uc_flags;
    void           *uc_link;
    struct { void *ss_sp; int ss_flags; unsigned int ss_size; } uc_stack;
    struct mcontext_t uc_mcontext;
    unsigned long   uc_sigmask;
} ucontext_t;
