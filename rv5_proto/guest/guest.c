/* Bare-metal RV32I guest. Writes ASCII output to a memory buffer; the
   host (C# orchestrator) reads from that buffer after EBREAK. */

typedef unsigned int   u32;
typedef          int   i32;
typedef unsigned char  u8;

/* --- output buffer in guest memory (0x80000..0xC0000) --- */
#define OUT_BASE 0x80000u
#define OUT_END  0xC0000u

static volatile char* out_ptr;

static void out_init(void) { out_ptr = (volatile char*)OUT_BASE; }

static void emit(char ch) {
    if ((u32)(unsigned long)out_ptr < OUT_END) {
        *out_ptr = ch;
        out_ptr++;
    }
}
static void emit_str(const char* s) { while (*s) emit(*s++); }

static void emit_dec(u32 v) {
    char buf[12]; int n = 0;
    if (v == 0) { emit('0'); return; }
    while (v) {
        u32 r = v, q = 0;
        while (r >= 10) { r -= 10; q++; }
        buf[n++] = (char)('0' + r);
        v = q;
    }
    while (n--) emit(buf[n]);
}

static void emit_hex32(u32 v) {
    emit('0'); emit('x');
    for (int i = 28; i >= 0; i -= 4) {
        u32 d = (v >> i) & 0xFu;
        emit((char)(d < 10 ? '0' + d : 'a' + d - 10));
    }
}

/* --- software libcalls (no M extension, no compiler-rt) --- */
int __mulsi3(int a, int b) {
    u32 ua = (u32)a, ub = (u32)b;
    u32 r = 0;
    while (ub) {
        if (ub & 1u) r = r + ua;
        ua <<= 1;
        ub >>= 1;
    }
    return (int)r;
}
unsigned __udivsi3(unsigned n, unsigned d) {
    if (d == 0) return 0xFFFFFFFFu;
    unsigned q = 0;
    for (int i = 31; i >= 0; i--) {
        if ((n >> i) >= d) { n -= (d << i); q |= (1u << i); }
    }
    return q;
}
unsigned __umodsi3(unsigned n, unsigned d) {
    if (d == 0) return n;
    for (int i = 31; i >= 0; i--) {
        if ((n >> i) >= d) n -= (d << i);
    }
    return n;
}
int __divsi3(int n, int d) {
    int sn = (n < 0), sd = (d < 0);
    unsigned un = sn ? (unsigned)(-n) : (unsigned)n;
    unsigned ud = sd ? (unsigned)(-d) : (unsigned)d;
    unsigned q = __udivsi3(un, ud);
    return (sn ^ sd) ? -(int)q : (int)q;
}
int __modsi3(int n, int d) {
    int sn = (n < 0);
    unsigned un = sn ? (unsigned)(-n) : (unsigned)n;
    unsigned ud = (d < 0) ? (unsigned)(-d) : (unsigned)d;
    unsigned r = __umodsi3(un, ud);
    return sn ? -(int)r : (int)r;
}

/* --- TEST 1: first 100 primes via trial division by subtraction --- */
static int is_prime(u32 n) {
    if (n < 2) return 0;
    if (n == 2) return 1;
    if ((n & 1u) == 0) return 0;
    for (u32 i = 3; i * i <= n; i += 2) {
        u32 r = n;
        while (r >= i) r -= i;
        if (r == 0) return 0;
    }
    return 1;
}

static void test_primes(void) {
    emit_str("--- Test 1: first 100 primes ---\n");
    int count = 0, col = 0;
    u32 n = 2;
    u32 last = 0;
    while (count < 100) {
        if (is_prime(n)) {
            emit_dec(n); emit(' ');
            count++; col++;
            if (col >= 10) { emit('\n'); col = 0; }
            last = n;
        }
        n++;
    }
    if (col) emit('\n');
    emit_str("100th prime = ");
    emit_dec(last);
    emit_str(" (expect 541) ");
    emit_str(last == 541 ? "PASS\n" : "FAIL\n");
}

/* --- TEST 2: 3x3 integer matrix multiply with known expected result --- */
static void test_matmul(void) {
    emit_str("--- Test 2: 3x3 integer matmul ---\n");
    int A[3][3] = {{1,2,3},{4,5,6},{7,8,9}};
    int B[3][3] = {{9,8,7},{6,5,4},{3,2,1}};
    int C[3][3];
    static const int E[3][3] = {{30,24,18},{84,69,54},{138,114,90}};

    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) {
            int s = 0;
            for (int k = 0; k < 3; k++) s += A[i][k] * B[k][j];
            C[i][j] = s;
        }
    }

    int ok = 1;
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) {
            emit_dec((u32)C[i][j]); emit(' ');
            if (C[i][j] != E[i][j]) ok = 0;
        }
        emit('\n');
    }
    emit_str(ok ? "MATMUL PASS\n" : "MATMUL FAIL\n");
}

/* --- TEST 3: CRC32 of a known string (exercises XOR/SHIFT/AND/loops) --- */
static u32 crc32(const char* s) {
    u32 crc = 0xFFFFFFFFu;
    while (*s) {
        crc ^= (u32)(u8)*s++;
        for (int i = 0; i < 8; i++) {
            if (crc & 1u) crc = (crc >> 1) ^ 0xEDB88320u;
            else          crc >>= 1;
        }
    }
    return ~crc;
}

static void test_crc(void) {
    emit_str("--- Test 3: CRC32 ---\n");
    const char* s = "Hello, RV32I!";
    u32 c = crc32(s);
    emit_str("crc32(\""); emit_str(s); emit_str("\") = ");
    emit_hex32(c);
    emit_str("  (host will verify)\n");
}

/* --- TEST 4: Fibonacci recurrence (memory + 32-bit add behavior) --- */
static void test_fib(void) {
    emit_str("--- Test 4: Fibonacci(0..15) ---\n");
    u32 fib[16];
    fib[0] = 0; fib[1] = 1;
    for (int i = 2; i < 16; i++) fib[i] = fib[i-1] + fib[i-2];
    static const u32 expected[16] = {
        0,1,1,2,3,5,8,13,21,34,55,89,144,233,377,610
    };
    int ok = 1;
    for (int i = 0; i < 16; i++) {
        emit_dec(fib[i]); emit(' ');
        if (fib[i] != expected[i]) ok = 0;
    }
    emit('\n');
    emit_str(ok ? "FIB PASS\n" : "FIB FAIL\n");
}

/* --- TEST 5: ASCII art banner "RV6" + pipeline diagram --- */
static void test_ascii(void) {
    emit_str("--- Test 5: ASCII art ---\n");
    static const char* art[] = {
        "  ####    #   #    ###  ",
        "  #   #   #   #   #     ",
        "  ####     # #    ####  ",
        "  # #      # #    #   # ",
        "  #   #     #      ###  ",
        "",
        "  +---------+      +--------+",
        "  |  fetch  |----->| decode |",
        "  +----+----+      +---+----+",
        "       |               |     ",
        "       v               v     ",
        "  +----+----+      +---+----+",
        "  |   mem   |<---->|  ALU   |",
        "  +---------+      +--------+",
        0
    };
    for (int i = 0; art[i]; i++) { emit_str(art[i]); emit('\n'); }
}

int main(void) {
    out_init();
    emit_str("RV32I emulator self-test\n");
    emit_str("========================\n\n");
    test_primes(); emit('\n');
    test_matmul(); emit('\n');
    test_crc();    emit('\n');
    test_fib();    emit('\n');
    test_ascii();  emit('\n');
    emit_str("--- ALL TESTS COMPLETE ---\n");
    emit('\0');  /* terminator for host */
    return 0;
}
