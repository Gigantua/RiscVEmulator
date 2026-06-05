#pragma once
#include <stdint.h>

/* jmp_buf saves: ra, sp, s0-s11 = 14 registers */
typedef uint32_t jmp_buf[14];

int  setjmp(jmp_buf env);
void longjmp(jmp_buf env, int val) __attribute__((noreturn));
