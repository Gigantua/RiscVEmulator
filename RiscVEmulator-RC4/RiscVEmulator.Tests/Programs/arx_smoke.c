// Phase 1 smoke test for the ARX primitive (__builtin_riscv_arx).
//   rd = rotl(rs1 + rs3, imm5) ^ rs2
//
// Compile with: clang --target=riscv32-unknown-elf -march=rv32i+arx ...
// The host runtime writes UART bytes to 0x10000000 and halts on a
// store to 0x40000000. We don't pull in libc here on purpose — the
// test must be self-contained so it can run even with a half-built
// runtime.

typedef unsigned int u32;

static void uart_putc(char c) {
    *(volatile unsigned char*)0x10000000u = (unsigned char)c;
}
static void uart_puts(const char* s) {
    while (*s) uart_putc(*s++);
}
static void uart_putx(u32 v) {
    char buf[9];
    for (int i = 7; i >= 0; --i) {
        unsigned d = (v >> (i * 4)) & 0xFu;
        buf[7 - i] = (char)(d < 10 ? '0' + d : 'a' + d - 10);
    }
    buf[8] = 0;
    uart_puts(buf);
}
static void host_exit(int code) {
    *(volatile unsigned int*)0x40000000u = (unsigned int)code;
    for (;;) {}
}

void _start(void) {
    // arx(0x100, 0, 0x0F, 0) -> rotl(0x100+0, 0) ^ 0x0F = 0x10F
    u32 r0 = __builtin_riscv_arx(0x100u, 0u, 0x0Fu, 0u);
    uart_puts("r0=0x"); uart_putx(r0); uart_putc('\n');

    // arx(1, 4, 0, 0) -> rotl(1, 4) ^ 0 = 0x10  (pure rotate)
    u32 r1 = __builtin_riscv_arx(1u, 4u, 0u, 0u);
    uart_puts("r1=0x"); uart_putx(r1); uart_putc('\n');

    // arx(0xAA, 0, 0x55, 0) -> (0xAA ^ 0x55) = 0xFF (pure XOR)
    u32 r2 = __builtin_riscv_arx(0xAAu, 0u, 0x55u, 0u);
    uart_puts("r2=0x"); uart_putx(r2); uart_putc('\n');

    // arx(0x1000, 0, 0, 0x234) -> (0x1000 + 0x234) ^ 0 = 0x1234 (pure ADD)
    u32 r3 = __builtin_riscv_arx(0x1000u, 0u, 0u, 0x234u);
    uart_puts("r3=0x"); uart_putx(r3); uart_putc('\n');

    int ok = (r0 == 0x10Fu) & (r1 == 0x10u)
           & (r2 == 0xFFu)  & (r3 == 0x1234u);
    uart_puts(ok ? "ARX OK\n" : "ARX FAIL\n");
    host_exit(ok ? 0 : 1);
}
