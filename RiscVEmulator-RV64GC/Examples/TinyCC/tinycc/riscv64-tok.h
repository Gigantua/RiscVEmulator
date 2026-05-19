/* riscv64-tok.h — assembler token list for the RISC-V 64 backend.
 *
 * Intentionally empty in this bare-metal embedding. tcctok.h includes this
 * file whenever TCC_TARGET_RISCV64 is defined, but the JIT demo builds
 * without riscv64-asm.c (see libtcc.c / tcc.h) — there is no inline-asm
 * backend, so no DEF_ASM() opcode/register tokens are needed. The RISCV32
 * branch never had a *-tok.h at all; this keeps the RISCV64 branch buildable
 * on the same terms. Restore the upstream token list here if asm support is
 * re-enabled.
 */
