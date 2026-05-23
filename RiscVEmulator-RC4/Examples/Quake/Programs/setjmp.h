/* setjmp.h shim — uses Runtime/setjmp.c's 14×u32 buffer. */
#ifndef RVEMU_SETJMP_H
#define RVEMU_SETJMP_H

typedef unsigned int jmp_buf[14];

int  setjmp (jmp_buf env);
void longjmp(jmp_buf env, int val) __attribute__((noreturn));

#endif
