// rv32i_core.cpp — Native RV32I+M+A+F hot path with optional M-mode privilege support.
// Windows only (ClangCL). Exports controlled by rv32i_core.def.

typedef signed char        int8_t;
typedef unsigned char      uint8_t;
typedef signed short       int16_t;
typedef unsigned short     uint16_t;
typedef unsigned int       uint32_t;
typedef unsigned long long uint64_t;
typedef signed int         int32_t;
typedef signed long long   int64_t;
#define INT32_MIN (-2147483647 - 1)

// Required when using floats without the CRT
extern "C" int _fltused = 0;

// sqrtf without CRT — use SSE2 sqrtss intrinsic
#include <intrin.h>
static __forceinline float host_sqrtf(float x) {
    return _mm_cvtss_f32(_mm_sqrt_ss(_mm_set_ss(x)));
}

typedef uint32_t (*mmio_read_fn) (uint32_t addr, int width);
typedef void     (*mmio_write_fn)(uint32_t addr, int width, uint32_t val);
typedef void     (*ecall_fn)     (uint32_t* regs);

// ── CPU state — hot fields first ─────────────────────────────────────
static uint32_t     regs[32];
static uint32_t     pc;
static int          halted;
static uint64_t     mtime;
static uint8_t*     ram;
static uint32_t     ram_size;
// cold
static int          exit_code;
static int          enable_m;
static uint64_t     mtimecmp    = 0xFFFFFFFFFFFFFFFFULL;
static mmio_read_fn  mmio_read;
static mmio_write_fn mmio_write;
static ecall_fn      on_ecall;

// ── Extension flags ───────────────────────────────────────────────────
static int          enable_f    = 0;
static int          enable_a    = 0;
static int          enable_priv = 0;

// ── RAM offset (e.g. 0x80000000 for Linux boot) ───────────────────────
// All physical addresses subtract ram_offset before indexing RAM.
static uint32_t     ram_offset  = 0;

// ── A-extension: LR/SC reservation ───────────────────────────────────
static uint32_t     rsv_addr    = ~0u;

// ── Diagnostics ───────────────────────────────────────────────────────
static uint64_t     umode_count  = 0;   // instructions executed at U-mode (priv=0)
static uint32_t     last_trap    = 0;   // most recent do_trap cause
static uint32_t     trap_count   = 0;   // total do_trap invocations
static uint32_t     mret_to_u    = 0;   // MRET instructions with mpp=0 (→ U-mode)
static uint32_t     mret_to_m    = 0;   // MRET instructions with mpp=3 (→ M-mode)
static uint32_t     uecall_count = 0;   // ECALL instructions from U-mode (Linux syscalls)

// ── Privileged mode state (used only when enable_priv != 0) ──────────
// Privilege level: 0 = U-mode, 1 = S-mode, 3 = M-mode
static uint32_t     priv_mode   = 3;
static int          wfi_pending = 0;

// M-mode CSRs
static uint32_t     csr_mstatus  = 0;
static uint32_t     csr_mtvec    = 0;
static uint32_t     csr_mie      = 0;
static uint32_t     csr_mip      = 0;
static uint32_t     csr_mepc     = 0;
static uint32_t     csr_mtval    = 0;
static uint32_t     csr_mcause   = 0;
static uint32_t     csr_mscratch = 0;
static uint32_t     csr_medeleg  = 0;   // exception delegation to S-mode
static uint32_t     csr_mideleg  = 0;   // interrupt delegation to S-mode

// S-mode CSRs
static uint32_t     csr_stvec    = 0;
static uint32_t     csr_sscratch = 0;
static uint32_t     csr_sepc     = 0;
static uint32_t     csr_scause   = 0;
static uint32_t     csr_stval    = 0;
static uint32_t     csr_satp     = 0;   // bare mode only (no TLB/MMU)

// ── Memory access ────────────────────────────────────────────────────
// With ram_offset=0 the unsigned subtraction is a no-op and the branch
// behaves identically to the original addr < ram_size check.

static inline uint32_t mem_read_word(uint32_t addr) {
    uint32_t raddr = addr - ram_offset;
    if (__builtin_expect(raddr < ram_size, 1)) {
        uint32_t v; __builtin_memcpy(&v, ram + raddr, 4); return v;
    }
    // Standard RISC-V CLINT (SiFive layout, 0x02000000)
    if (addr == 0x0200BFF8u) return (uint32_t)mtime;
    if (addr == 0x0200BFFCu) return (uint32_t)(mtime >> 32);
    if (addr == 0x02004000u) return (uint32_t)mtimecmp;
    if (addr == 0x02004004u) return (uint32_t)(mtimecmp >> 32);
    // mini-rv32ima CLINT layout (0x11000000)
    if (addr == 0x1100BFF8u) return (uint32_t)mtime;
    if (addr == 0x1100BFFCu) return (uint32_t)(mtime >> 32);
    if (addr == 0x11004000u) return (uint32_t)mtimecmp;
    if (addr == 0x11004004u) return (uint32_t)(mtimecmp >> 32);
    return mmio_read ? mmio_read(addr, 4) : 0;
}

static inline uint16_t mem_read_half(uint32_t addr) {
    uint32_t raddr = addr - ram_offset;
    if (__builtin_expect(raddr < ram_size, 1)) {
        uint16_t v; __builtin_memcpy(&v, ram + raddr, 2); return v;
    }
    return mmio_read ? (uint16_t)mmio_read(addr, 2) : 0;
}

static inline uint8_t mem_read_byte(uint32_t addr) {
    uint32_t raddr = addr - ram_offset;
    if (__builtin_expect(raddr < ram_size, 1)) return ram[raddr];
    return mmio_read ? (uint8_t)mmio_read(addr, 1) : 0;
}

static inline void mem_write_word(uint32_t addr, uint32_t val) {
    uint32_t raddr = addr - ram_offset;
    if (__builtin_expect(raddr < ram_size, 1)) {
        __builtin_memcpy(ram + raddr, &val, 4); return;
    }
    // Standard RISC-V CLINT (0x02000000)
    if (addr == 0x0200BFF8u) { mtime    = (mtime    & 0xFFFFFFFF00000000ULL) | val;                   return; }
    if (addr == 0x0200BFFCu) { mtime    = (mtime    & 0x00000000FFFFFFFFULL) | ((uint64_t)val << 32); return; }
    if (addr == 0x02004000u) { mtimecmp = (mtimecmp & 0xFFFFFFFF00000000ULL) | val;                   return; }
    if (addr == 0x02004004u) { mtimecmp = (mtimecmp & 0x00000000FFFFFFFFULL) | ((uint64_t)val << 32); return; }
    // mini-rv32ima CLINT layout (0x11000000)
    if (addr == 0x1100BFF8u) { mtime    = (mtime    & 0xFFFFFFFF00000000ULL) | val;                   return; }
    if (addr == 0x1100BFFCu) { mtime    = (mtime    & 0x00000000FFFFFFFFULL) | ((uint64_t)val << 32); return; }
    if (addr == 0x11004000u) { mtimecmp = (mtimecmp & 0xFFFFFFFF00000000ULL) | val;                   return; }
    if (addr == 0x11004004u) { mtimecmp = (mtimecmp & 0x00000000FFFFFFFFULL) | ((uint64_t)val << 32); return; }
    if (mmio_write) mmio_write(addr, 4, val);
}

static inline void mem_write_half(uint32_t addr, uint16_t val) {
    uint32_t raddr = addr - ram_offset;
    if (__builtin_expect(raddr < ram_size, 1)) { __builtin_memcpy(ram + raddr, &val, 2); return; }
    if (mmio_write) mmio_write(addr, 2, val);
}

static inline void mem_write_byte(uint32_t addr, uint8_t val) {
    uint32_t raddr = addr - ram_offset;
    if (__builtin_expect(raddr < ram_size, 1)) { ram[raddr] = val; return; }
    if (mmio_write) mmio_write(addr, 1, val);
}

// ── Immediate decoders ───────────────────────────────────────────────

static constexpr uint32_t j_imm(uint32_t i) {
    uint32_t v = ((i>>31)&1u)<<20 | ((i>>12)&0xFFu)<<12 | ((i>>20)&1u)<<11 | ((i>>21)&0x3FFu)<<1;
    return (v & 0x100000u) ? v | 0xFFE00000u : v;
}
static constexpr uint32_t b_imm(uint32_t i) {
    uint32_t v = ((i>>31)&1u)<<12 | ((i>>7)&1u)<<11 | ((i>>25)&0x3Fu)<<5 | ((i>>8)&0xFu)<<1;
    return (v & 0x1000u) ? v | 0xFFFFE000u : v;
}
static constexpr int32_t i_imm(uint32_t i) { return (int32_t)i >> 20; }
static constexpr int32_t s_imm(uint32_t i) { return ((int32_t)(i & 0xFE000000) >> 20) | (int32_t)((i >> 7) & 0x1Fu); }

// ── F-extension (single-precision float, RV32F) ──────────────────────

static uint32_t fregs[32];

static inline float f_get(int r)          { float v; __builtin_memcpy(&v, &fregs[r], 4); return v; }
static inline void  f_set(int r, float v) { __builtin_memcpy(&fregs[r], &v, 4); }
static inline int   f_isnan(int r) {
    return (fregs[r] & 0x7F800000u) == 0x7F800000u && (fregs[r] & 0x007FFFFFu);
}

// ── M-extension (MUL/DIV/REM) ────────────────────────────────────────

static inline uint32_t exec_m(uint32_t f3, int32_t s1, uint32_t u1, int32_t s2, uint32_t u2) {
    switch (f3) {
        case 0: return (uint32_t)(s1 * s2);
        case 1: return (uint32_t)((int64_t)s1 * s2 >> 32);
        case 2: return (uint32_t)((int64_t)s1 * (int64_t)u2 >> 32);
        case 3: return (uint32_t)((uint64_t)u1 * u2 >> 32);
        case 4: return s2 == 0 ? 0xFFFFFFFFu : s1 == INT32_MIN && s2 == -1 ? (uint32_t)s1 : (uint32_t)(s1 / s2);
        case 5: return u2 == 0 ? 0xFFFFFFFFu : u1 / u2;
        case 6: return s2 == 0 ? u1 : s1 == INT32_MIN && s2 == -1 ? 0u : (uint32_t)(s1 % s2);
        case 7: return u2 == 0 ? u1 : u1 % u2;
        default: return 0;
    }
}

// ── Privileged mode: CSR read / write ────────────────────────────────
// sstatus is a restricted S-mode view of mstatus.
// Accessible bits: SIE(1), SPIE(5), SPP(8), FS(13:12), SUM(18), MXR(19), SD(31).
#define SSTATUS_MASK 0x800DE122u

static uint32_t priv_csr_read(uint32_t csrno) {
    switch (csrno) {
    // M-mode
    case 0x300: return csr_mstatus;
    case 0x301: return 0x40401101u;   // misa: MXL=1(RV32), extensions IMA+X
    case 0x302: return csr_medeleg;
    case 0x303: return csr_mideleg;
    case 0x304: return csr_mie;
    case 0x305: return csr_mtvec;
    case 0x340: return csr_mscratch;
    case 0x341: return csr_mepc;
    case 0x342: return csr_mcause;
    case 0x343: return csr_mtval;
    case 0x344: return csr_mip;
    case 0xF11: return 0xFF0FF0FFu;   // mvendorid
    case 0xF12: return 0u;            // marchid
    case 0xF13: return 0u;            // mimpid
    case 0xF14: return 0u;            // mhartid
    case 0x3A0: return 0u;            // pmpcfg0  (stub)
    case 0x3B0: return 0u;            // pmpaddr0 (stub)
    // S-mode (restricted view of M-mode state)
    case 0x100: return csr_mstatus & SSTATUS_MASK;  // sstatus
    case 0x104: return csr_mie  & 0x333u;           // sie
    case 0x105: return csr_stvec;
    case 0x140: return csr_sscratch;
    case 0x141: return csr_sepc;
    case 0x142: return csr_scause;
    case 0x143: return csr_stval;
    case 0x144: return csr_mip  & 0x333u;           // sip
    case 0x180: return csr_satp;
    // Counters (use mtime as proxy for cycle/time/instret)
    case 0xC00: case 0xB00: return (uint32_t)mtime;
    case 0xC80: case 0xB80: return (uint32_t)(mtime >> 32);
    case 0xC01: case 0xB01: return (uint32_t)mtime;
    case 0xC81: case 0xB81: return (uint32_t)(mtime >> 32);
    case 0xC02: case 0xB02: return (uint32_t)mtime;
    case 0xC82: case 0xB82: return (uint32_t)(mtime >> 32);
    default:    return 0;
    }
}

static void priv_csr_write(uint32_t csrno, uint32_t val) {
    switch (csrno) {
    case 0x300: csr_mstatus  = val; break;
    case 0x302: csr_medeleg  = val; break;
    case 0x303: csr_mideleg  = val; break;
    case 0x304: csr_mie      = val; break;
    case 0x305: csr_mtvec    = val; break;
    case 0x340: csr_mscratch = val; break;
    case 0x341: csr_mepc     = val & ~1u; break;
    case 0x342: csr_mcause   = val; break;
    case 0x343: csr_mtval    = val; break;
    case 0x344: csr_mip      = (csr_mip & (1u<<7)) | (val & ~(1u<<7)); break; // MTIP read-only
    case 0x3A0: break;  // pmpcfg0  — ignore
    case 0x3B0: break;  // pmpaddr0 — ignore
    // S-mode writes update the corresponding bits of M-mode state
    case 0x100: csr_mstatus = (csr_mstatus & ~SSTATUS_MASK) | (val & SSTATUS_MASK); break; // sstatus
    case 0x104: csr_mie  = (csr_mie  & ~0x333u) | (val & 0x333u); break;  // sie
    case 0x105: csr_stvec    = val; break;
    case 0x140: csr_sscratch = val; break;
    case 0x141: csr_sepc     = val & ~1u; break;
    case 0x142: csr_scause   = val; break;
    case 0x143: csr_stval    = val; break;
    case 0x144: csr_mip  = (csr_mip  & ~0x333u) | (val & 0x333u); break;  // sip
    case 0x180: csr_satp     = val; break;
    default: break;
    }
}

// ── Privileged mode: trap dispatch ───────────────────────────────────
// Dispatches to S-mode (stvec) if the cause is delegated via medeleg/mideleg
// and the trap originates from a less-privileged mode; otherwise to M-mode (mtvec).

static void do_trap(uint32_t cause, uint32_t tval) {
    bool is_intr  = (cause & 0x80000000u) != 0u;
    uint32_t cidx = cause & 0x1Fu;
    uint32_t bit  = (cidx < 32) ? (1u << cidx) : 0u;

    // Delegate to S-mode only when trap originates from U- or S-mode
    bool to_s = (priv_mode < 3) &&
                (is_intr ? ((csr_mideleg & bit) != 0u)
                         : ((csr_medeleg & bit) != 0u));

    wfi_pending = 0;
    last_trap   = cause;
    trap_count++;

    if (to_s) {
        csr_sepc   = pc;
        csr_scause = cause;
        csr_stval  = tval;
        // sstatus: SPIE=SIE, SPP=current_priv, SIE=0
        uint32_t sie = (csr_mstatus >> 1) & 1u;
        uint32_t spp = priv_mode & 1u;
        csr_mstatus = (csr_mstatus & ~0x122u) | (spp << 8) | (sie << 5);
        priv_mode   = 1;  // switch to S-mode
        pc = (is_intr && (csr_stvec & 1u))
             ? (csr_stvec & ~3u) + (cidx * 4u)
             : (csr_stvec & ~3u);
    } else {
        csr_mepc   = pc;
        csr_mcause = cause;
        csr_mtval  = tval;
        // mstatus: MPIE=MIE, MPP=current_priv, MIE=0
        uint32_t mie_b = (csr_mstatus >> 3) & 1u;
        csr_mstatus = (csr_mstatus & ~0x1888u) | (priv_mode << 11) | (mie_b << 7);
        priv_mode   = 3;  // switch to M-mode
        pc = (is_intr && (csr_mtvec & 1u))
             ? (csr_mtvec & ~3u) + (cidx * 4u)
             : (csr_mtvec & ~3u);
    }
}

// ── Privileged mode: interrupt check ─────────────────────────────────
// Updates MTIP from mtime/mtimecmp. Returns true (and fires trap) if an
// interrupt is taken. Priority: MEI(11) > MSI(3) > MTI(7) > SEI(9) > SSI(1) > STI(5).

static bool check_interrupts() {
    // Hardware sets MTIP when mtime >= mtimecmp
    if (mtime >= mtimecmp) csr_mip |=  (1u << 7);
    else                   csr_mip &= ~(1u << 7);

    uint32_t pending = csr_mip & csr_mie;
    if (!pending) return false;

    uint32_t mie_b = (csr_mstatus >> 3) & 1u;
    uint32_t sie_b = (csr_mstatus >> 1) & 1u;

    static const uint32_t prio[6] = { 11, 3, 7, 9, 1, 5 };
    for (int i = 0; i < 6; i++) {
        uint32_t b = 1u << prio[i];
        if (!(pending & b)) continue;
        bool delegated = (csr_mideleg & b) != 0u;
        if (!delegated) {
            // M-mode interrupt: fires if priv < M, or priv == M with MIE set
            if (priv_mode < 3 || mie_b) { do_trap(0x80000000u | prio[i], 0); return true; }
        } else {
            // S-mode interrupt: fires if priv < S, or priv == S with SIE set
            if (priv_mode == 0 || (priv_mode == 1 && sie_b)) { do_trap(0x80000000u | prio[i], 0); return true; }
        }
    }
    return false;
}

// ── Fast path: RV32IM bare-metal (no F/A/priv extensions) ────────────
// All enable_priv / enable_f / enable_a branches are statically removed.
// Called only when enable_priv=0, enable_f=0, enable_a=0, enable_m=1.

static __forceinline void step_fast_m() {
    const uint32_t instr  = mem_read_word(pc);
    const uint32_t opcode = instr & 0x7F;
    const int      rd     = (instr >>  7) & 0x1F;
    const int      rs1    = (instr >> 15) & 0x1F;
    const int      rs2    = (instr >> 20) & 0x1F;
    const uint32_t f3     = (instr >> 12) & 0x7;
    const uint32_t f7     = (instr >> 25) & 0x7F;
    const int32_t  s1     = (int32_t)regs[rs1];
    const uint32_t u1     = regs[rs1];
    const int32_t  s2     = (int32_t)regs[rs2];
    const uint32_t u2     = regs[rs2];
    uint32_t nextpc = pc + 4;

    switch (opcode) {

    case 0x37: regs[rd] = instr & 0xFFFFF000u; break;                                    // LUI
    case 0x17: regs[rd] = pc + (instr & 0xFFFFF000u); break;                             // AUIPC
    case 0x6F: regs[rd] = pc + 4; nextpc = pc + j_imm(instr); break;                    // JAL
    case 0x67: { uint32_t t = (uint32_t)(s1 + i_imm(instr)) & ~1u;                      // JALR
                 regs[rd] = pc + 4; nextpc = t; break; }

    case 0x63: {                                                                           // BRANCH
        int taken;
        switch (f3) {
            case 0: taken = u1 == u2; break;  case 1: taken = u1 != u2; break;
            case 4: taken = s1 <  s2; break;  case 5: taken = s1 >= s2; break;
            case 6: taken = u1 <  u2; break;  case 7: taken = u1 >= u2; break;
            default: taken = 0;
        }
        if (taken) nextpc = pc + b_imm(instr);
        break;
    }

    case 0x03: {                                                                           // LOAD
        const uint32_t addr = (uint32_t)(s1 + i_imm(instr));
        switch (f3) {
            case 0: regs[rd] = (uint32_t)(int8_t) mem_read_byte(addr); break;
            case 1: regs[rd] = (uint32_t)(int16_t)mem_read_half(addr); break;
            case 2: regs[rd] = mem_read_word(addr);                     break;
            case 4: regs[rd] = mem_read_byte(addr);                     break;
            case 5: regs[rd] = mem_read_half(addr);                     break;
        }
        break;
    }

    case 0x23: {                                                                           // STORE
        const uint32_t addr = (uint32_t)(s1 + s_imm(instr));
        switch (f3) {
            case 0: mem_write_byte(addr, (uint8_t) u2); break;
            case 1: mem_write_half(addr, (uint16_t)u2); break;
            case 2: mem_write_word(addr,             u2); break;
        }
        break;
    }

    case 0x13: {                                                                           // OP-IMM
        const int32_t imm = i_imm(instr);
        const int     sh  = (instr >> 20) & 0x1F;
        uint32_t r;
        switch (f3) {
            case 0: r = (uint32_t)(s1 + imm);                                    break;
            case 1: r = u1 << sh;                                                 break;
            case 2: r = s1 < imm           ? 1u : 0u;                            break;
            case 3: r = u1 < (uint32_t)imm ? 1u : 0u;                            break;
            case 4: r = u1 ^ (uint32_t)imm;                                      break;
            case 5: r = f7 == 0x20 ? (uint32_t)(s1 >> sh) : u1 >> sh;           break;
            case 6: r = u1 | (uint32_t)imm;                                      break;
            case 7: r = u1 & (uint32_t)imm;                                      break;
            default: r = 0;
        }
        regs[rd] = r;
        break;
    }

    case 0x33: {                                                                           // OP (M-ext always enabled)
        if (f7 == 0x01) { regs[rd] = exec_m(f3, s1, u1, s2, u2); break; }
        const int sh = s2 & 0x1F;
        uint32_t r;
        switch (f3) {
            case 0: r = f7 == 0x20 ? (uint32_t)(s1 - s2) : (uint32_t)(s1 + s2); break;
            case 1: r = u1 << sh;                                                 break;
            case 2: r = s1 < s2 ? 1u : 0u;                                       break;
            case 3: r = u1 < u2 ? 1u : 0u;                                       break;
            case 4: r = u1 ^ u2;                                                  break;
            case 5: r = f7 == 0x20 ? (uint32_t)(s1 >> sh) : u1 >> sh;            break;
            case 6: r = u1 | u2;                                                  break;
            case 7: r = u1 & u2;                                                  break;
            default: r = 0;
        }
        regs[rd] = r;
        break;
    }

    // AMO / FP opcodes — not enabled in this fast path, treat as NOP
    case 0x2F: case 0x07: case 0x27:
    case 0x43: case 0x47: case 0x4B: case 0x4F: case 0x53: break;

    case 0x0F: break;                                                                     // FENCE — NOP

    case 0x73: {                                                                           // SYSTEM (bare-metal)
        const uint32_t fn  = (instr >> 20) & 0xFFF;
        const uint32_t f3s = (instr >> 12) & 0x7;
        if (f3s == 0) {
            if      (fn == 0) { if (on_ecall) on_ecall(regs); }                          // ECALL
            else if (fn == 1) { halted = 1; }                                             // EBREAK
        } else {
            regs[rd] = 0;                                                                 // CSR reads → 0 in bare-metal
        }
        break;
    }

    } // end switch (opcode)

    regs[0] = 0;
    pc = nextpc;
    mtime++;
}

// ── Execute one instruction ───────────────────────────────────────────
static inline void step() {
    // Privileged mode: timer interrupt check and WFI sleep
    if (enable_priv) {
        if (check_interrupts()) { regs[0] = 0; mtime++; return; }
        if (wfi_pending)        { regs[0] = 0; mtime++; return; }
    }

    const uint32_t instr  = mem_read_word(pc);
    const uint32_t opcode = instr & 0x7F;
    const int      rd     = (instr >>  7) & 0x1F;
    const int      rs1    = (instr >> 15) & 0x1F;
    const int      rs2    = (instr >> 20) & 0x1F;
    const uint32_t f3     = (instr >> 12) & 0x7;
    const uint32_t f7     = (instr >> 25) & 0x7F;
    const int32_t  s1     = (int32_t)regs[rs1];
    const uint32_t u1     = regs[rs1];
    const int32_t  s2     = (int32_t)regs[rs2];
    const uint32_t u2     = regs[rs2];
    uint32_t nextpc    = pc + 4;
    uint32_t trap_cause = 0, trap_tval = 0;

    switch (opcode) {

    case 0x37: regs[rd] = instr & 0xFFFFF000u; break;                                    // LUI
    case 0x17: regs[rd] = pc + (instr & 0xFFFFF000u); break;                             // AUIPC
    case 0x6F: regs[rd] = pc + 4; nextpc = pc + j_imm(instr); break;                    // JAL
    case 0x67: { uint32_t t = (uint32_t)(s1 + i_imm(instr)) & ~1u;                      // JALR
                 regs[rd] = pc + 4; nextpc = t; break; }

    case 0x63: {                                                                           // BRANCH
        int taken;
        switch (f3) {
            case 0: taken = u1 == u2; break;  case 1: taken = u1 != u2; break;
            case 4: taken = s1 <  s2; break;  case 5: taken = s1 >= s2; break;
            case 6: taken = u1 <  u2; break;  case 7: taken = u1 >= u2; break;
            default: taken = 0;
        }
        if (taken) nextpc = pc + b_imm(instr);
        break;
    }

    case 0x03: {                                                                           // LOAD
        const uint32_t addr = (uint32_t)(s1 + i_imm(instr));
        switch (f3) {
            case 0: regs[rd] = (uint32_t)(int8_t) mem_read_byte(addr); break;
            case 1: regs[rd] = (uint32_t)(int16_t)mem_read_half(addr); break;
            case 2: regs[rd] = mem_read_word(addr);                     break;
            case 4: regs[rd] = mem_read_byte(addr);                     break;
            case 5: regs[rd] = mem_read_half(addr);                     break;
        }
        break;
    }

    case 0x23: {                                                                           // STORE
        const uint32_t addr = (uint32_t)(s1 + s_imm(instr));
        switch (f3) {
            case 0: mem_write_byte(addr, (uint8_t) u2); break;
            case 1: mem_write_half(addr, (uint16_t)u2); break;
            case 2: mem_write_word(addr,             u2); break;
        }
        break;
    }

    case 0x13: {                                                                           // OP-IMM
        const int32_t imm = i_imm(instr);
        const int     sh  = (instr >> 20) & 0x1F;
        uint32_t r;
        switch (f3) {
            case 0: r = (uint32_t)(s1 + imm);                                    break;
            case 1: r = u1 << sh;                                                 break;
            case 2: r = s1 < imm           ? 1u : 0u;                            break;
            case 3: r = u1 < (uint32_t)imm ? 1u : 0u;                            break;
            case 4: r = u1 ^ (uint32_t)imm;                                      break;
            case 5: r = f7 == 0x20 ? (uint32_t)(s1 >> sh) : u1 >> sh;           break;
            case 6: r = u1 | (uint32_t)imm;                                      break;
            case 7: r = u1 & (uint32_t)imm;                                      break;
            default: r = 0;
        }
        regs[rd] = r;
        break;
    }

    case 0x33: {                                                                           // OP
        uint32_t r;
        if (f7 == 0x01 && enable_m) {
            r = exec_m(f3, s1, u1, s2, u2);
        } else {
            const int sh = s2 & 0x1F;
            switch (f3) {
                case 0: r = f7 == 0x20 ? (uint32_t)(s1 - s2) : (uint32_t)(s1 + s2); break;
                case 1: r = u1 << sh;                                                 break;
                case 2: r = s1 < s2 ? 1u : 0u;                                       break;
                case 3: r = u1 < u2 ? 1u : 0u;                                       break;
                case 4: r = u1 ^ u2;                                                  break;
                case 5: r = f7 == 0x20 ? (uint32_t)(s1 >> sh) : u1 >> sh;            break;
                case 6: r = u1 | u2;                                                  break;
                case 7: r = u1 & u2;                                                  break;
                default: r = 0;
            }
        }
        regs[rd] = r;
        break;
    }

    case 0x2F: {                                                                           // AMO (RV32A)
        if (!enable_a) break;
        const uint32_t irmid = (instr >> 27) & 0x1F;
        const uint32_t addr  = u1;
        if (irmid == 2) {                                                                  // LR.W
            regs[rd] = mem_read_word(addr);
            rsv_addr = addr;
        } else if (irmid == 3) {                                                           // SC.W
            if (rsv_addr == addr) { mem_write_word(addr, u2); regs[rd] = 0; }
            else                  {                           regs[rd] = 1; }
            rsv_addr = ~0u;
        } else {                                                                           // RMW AMOs
            uint32_t old = mem_read_word(addr);
            regs[rd]     = old;
            uint32_t nw;
            switch (irmid) {
                case  1: nw = u2;                                              break;  // AMOSWAP
                case  0: nw = old + u2;                                        break;  // AMOADD
                case  4: nw = old ^ u2;                                        break;  // AMOXOR
                case 12: nw = old & u2;                                        break;  // AMOAND
                case  8: nw = old | u2;                                        break;  // AMOOR
                case 16: nw = (int32_t)u2 < (int32_t)old ? u2 : old;         break;  // AMOMIN
                case 20: nw = (int32_t)u2 > (int32_t)old ? u2 : old;         break;  // AMOMAX
                case 24: nw = u2 < old ? u2 : old;                            break;  // AMOMINU
                case 28: nw = u2 > old ? u2 : old;                            break;  // AMOMAXU
                default: nw = old;                                             break;
            }
            mem_write_word(addr, nw);
        }
        break;
    }

    case 0x07:  /* LOAD-FP: FLW (f3==2) */
        if (enable_f && f3 == 2)
            fregs[rd] = mem_read_word((uint32_t)(s1 + i_imm(instr)));
        break;

    case 0x27:  /* STORE-FP: FSW (f3==2) */
        if (enable_f && f3 == 2)
            mem_write_word((uint32_t)(s1 + s_imm(instr)), fregs[rs2]);
        break;

    case 0x43: case 0x47: case 0x4B: case 0x4F:  /* FMADD/FMSUB/FNMSUB/FNMADD */
        if (enable_f) {
            const int rs3 = (int)((instr >> 27) & 0x1F);
            const float fa = f_get(rs1), fb = f_get(rs2), fc = f_get(rs3);
            float fr;
            if      (opcode == 0x43) fr =  fa*fb + fc;
            else if (opcode == 0x47) fr =  fa*fb - fc;
            else if (opcode == 0x4B) fr = -fa*fb + fc;
            else                     fr = -fa*fb - fc;
            f_set(rd, fr);
        }
        break;

    case 0x53:  /* OP-FP */
        if (enable_f) {
            switch (f7) {
                case 0x00: f_set(rd, f_get(rs1) + f_get(rs2)); break;  /* FADD.S  */
                case 0x04: f_set(rd, f_get(rs1) - f_get(rs2)); break;  /* FSUB.S  */
                case 0x08: f_set(rd, f_get(rs1) * f_get(rs2)); break;  /* FMUL.S  */
                case 0x0C: f_set(rd, f_get(rs1) / f_get(rs2)); break;  /* FDIV.S  */
                case 0x2C: f_set(rd, host_sqrtf(f_get(rs1))); break;   /* FSQRT.S */
                case 0x10:  /* FSGNJ.S / FSGNJN.S / FSGNJX.S */
                    switch (f3) {
                        case 0: fregs[rd] = ( fregs[rs2]             & 0x80000000u) | (fregs[rs1] & 0x7FFFFFFFu); break;
                        case 1: fregs[rd] = (~fregs[rs2]             & 0x80000000u) | (fregs[rs1] & 0x7FFFFFFFu); break;
                        case 2: fregs[rd] = ((fregs[rs1]^fregs[rs2]) & 0x80000000u) | (fregs[rs1] & 0x7FFFFFFFu); break;
                    }
                    break;
                case 0x14: {  /* FMIN.S / FMAX.S */
                    const float fa = f_get(rs1), fb = f_get(rs2);
                    const int   na = f_isnan(rs1), nb = f_isnan(rs2);
                    if      (na && nb) fregs[rd] = 0x7FC00000u;
                    else if (na)       f_set(rd, fb);
                    else if (nb)       f_set(rd, fa);
                    else if (f3 == 0)  f_set(rd, fa < fb ? fa : fb);
                    else               f_set(rd, fa > fb ? fa : fb);
                    break;
                }
                case 0x50:  /* FEQ.S / FLT.S / FLE.S */
                    switch (f3) {
                        case 2: regs[rd] = (!f_isnan(rs1)&&!f_isnan(rs2)&&f_get(rs1)==f_get(rs2)) ? 1u : 0u; break;
                        case 1: regs[rd] = (!f_isnan(rs1)&&!f_isnan(rs2)&&f_get(rs1)< f_get(rs2)) ? 1u : 0u; break;
                        case 0: regs[rd] = (!f_isnan(rs1)&&!f_isnan(rs2)&&f_get(rs1)<=f_get(rs2)) ? 1u : 0u; break;
                    }
                    break;
                case 0x60: {  /* FCVT.W.S / FCVT.WU.S */
                    const float fa = f_get(rs1);
                    regs[rd] = (rs2 == 0)
                        ? (f_isnan(rs1) ? 0x7FFFFFFFu : (uint32_t)(int32_t)fa)
                        : ((f_isnan(rs1)||fa<0.0f) ? 0u : (uint32_t)fa);
                    break;
                }
                case 0x68:  /* FCVT.S.W / FCVT.S.WU */
                    f_set(rd, rs2==0 ? (float)(int32_t)regs[rs1] : (float)regs[rs1]);
                    break;
                case 0x70:  /* FMV.X.W (f3==0) / FCLASS.S (f3==1) */
                    if (f3 == 0) regs[rd] = fregs[rs1];
                    break;
                case 0x78:  /* FMV.W.X */
                    fregs[rd] = regs[rs1];
                    break;
            }
        }
        break;

    case 0x0F: break;                                                                     // FENCE / FENCE.I — nop

    case 0x73: {                                                                           // SYSTEM
        const uint32_t fn  = (instr >> 20) & 0xFFF;
        const uint32_t f3s = (instr >> 12) & 0x7;

        if (f3s == 0) {
            // Non-CSR SYSTEM: ECALL / EBREAK / MRET / SRET / WFI
            if (fn == 0) {
                // ECALL
                if (enable_priv) {
                    // In privileged mode ECALL always causes a trap; no C# callback.
                    trap_cause = (priv_mode == 3) ? 11u : (priv_mode == 1) ? 9u : 8u;
                    if (priv_mode == 0) uecall_count++;
                } else {
                    if (on_ecall) on_ecall(regs);
                }
            } else if (fn == 1) {
                // EBREAK
                if (enable_priv) { trap_cause = 3u; trap_tval = pc; }
                else             { halted = 1; }
            } else if (fn == 0x102 && enable_priv) {
                // SRET — return from S-mode trap
                uint32_t spie = (csr_mstatus >> 5) & 1u;
                uint32_t spp  = (csr_mstatus >> 8) & 1u;
                csr_mstatus   = (csr_mstatus & ~0x122u) | (1u << 5) | (spie << 1);
                priv_mode = spp;
                nextpc    = csr_sepc;
            } else if (fn == 0x302 && enable_priv) {
                // MRET — return from M-mode trap
                uint32_t mpie = (csr_mstatus >> 7) & 1u;
                uint32_t mpp  = (csr_mstatus >> 11) & 3u;
                csr_mstatus   = (csr_mstatus & ~0x1888u) | (1u << 7) | (mpie << 3);
                priv_mode = mpp;
                nextpc    = csr_mepc;
                if (mpp == 0) mret_to_u++; else mret_to_m++;
            } else if (fn == 0x105 && enable_priv) {
                // WFI — wait for interrupt.
                // Linux calls local_irq_disable() (clears mstatus.MIE) before WFI in the idle path.
                // Per RISC-V spec, WFI should wake when any interrupt becomes pending,
                // regardless of the MIE gate.  Force MIE=1 here so check_interrupts()
                // will take the pending timer trap on the next step() call, matching
                // mini-rv32ima behaviour.
                csr_mstatus |= 8u;  // set MIE (bit 3)
                wfi_pending = 1;
            }
            // Unknown fn: silently ignored (could raise illegal-instruction trap if strict)
        } else if (enable_priv) {
            // Zicsr: CSRRW / CSRRS / CSRRC / CSRRWI / CSRRSI / CSRRCI
            const int     rs1imm = (instr >> 15) & 0x1F;
            const uint32_t rs1v  = regs[rs1imm];
            const uint32_t old   = priv_csr_read(fn);
            regs[rd] = old;
            uint32_t nval;
            switch (f3s) {
                case 1: nval = rs1v;                    break;  // CSRRW
                case 2: nval = old |  rs1v;             break;  // CSRRS
                case 3: nval = old & ~rs1v;             break;  // CSRRC
                case 5: nval = (uint32_t)rs1imm;        break;  // CSRRWI
                case 6: nval = old |  (uint32_t)rs1imm; break;  // CSRRSI
                case 7: nval = old & ~(uint32_t)rs1imm; break;  // CSRRCI
                default: nval = old; break;
            }
            // Per spec: CSRRS/CSRRC don't write if rs1==x0; CSRRSI/CSRRCI don't write if uimm==0
            if (f3s == 1 || f3s == 5 || rs1imm != 0)
                priv_csr_write(fn, nval);
        } else {
            // Bare-metal fallback: CSR reads return 0
            regs[rd] = 0;
        }
        break;
    }

    } // end switch (opcode)

    // Privileged mode trap dispatch (ECALL/EBREAK raised an explicit trap above)
    if (trap_cause && enable_priv) {
        do_trap(trap_cause, trap_tval);
        regs[0] = 0;
        mtime++;
        return;
    }

    regs[0] = 0;
    pc = nextpc;
    if (priv_mode == 0) umode_count++;
    mtime++;
}

// ── Public API ───────────────────────────────────────────────────────

extern "C" void rv32i_init(uint8_t* ram_ptr, uint32_t ram_sz, uint32_t entry, int m_ext) {
    ram        = ram_ptr;
    ram_size   = ram_sz;
    for (int i = 0; i < 32; i++) regs[i]  = 0;
    for (int i = 0; i < 32; i++) fregs[i] = 0;
    pc          = entry;
    halted      = 0;
    exit_code   = 0;
    enable_m    = m_ext;
    enable_f    = 0;
    enable_a    = 0;
    enable_priv = 0;
    ram_offset  = 0;
    rsv_addr    = ~0u;
    umode_count = 0;
    last_trap   = 0;
    trap_count  = 0;
    mret_to_u   = 0;
    mret_to_m   = 0;
    uecall_count = 0;
    mtime       = 0;
    mtimecmp    = 0xFFFFFFFFFFFFFFFFULL;
    mmio_read   = nullptr;
    mmio_write  = nullptr;
    on_ecall    = nullptr;
    // Privileged state
    priv_mode   = 3;
    wfi_pending = 0;
    csr_mstatus = csr_mtvec   = csr_mie     = csr_mip    = 0;
    csr_mepc    = csr_mtval   = csr_mcause  = csr_mscratch = 0;
    csr_medeleg = csr_mideleg = 0;
    csr_stvec   = csr_sscratch = csr_sepc   = csr_scause = 0;
    csr_stval   = csr_satp    = 0;
}

extern "C" void rv32i_destroy()                                  { ram = nullptr; ram_size = 0; }
extern "C" void rv32i_set_mmio(mmio_read_fn r, mmio_write_fn w) { mmio_read = r; mmio_write = w; }
extern "C" void rv32i_set_ecall(ecall_fn h)                      { on_ecall = h; }

extern "C" int rv32i_step_n(int n) {
    // Fast path: RV32IM bare-metal — the common case for Craft/Doom/tests.
    // Avoids all per-instruction enable_priv/enable_f/enable_a branches.
    if (!enable_priv && !enable_f && !enable_a && enable_m) {
        for (int i = 0; i < n; i++) {
            step_fast_m();
            if (__builtin_expect(halted, 0)) return -(i + 1);
        }
        return n;
    }
    // General path: all extension flags checked at runtime.
    for (int i = 0; i < n; i++) {
        step();
        if (__builtin_expect(halted, 0)) return -(i + 1);
    }
    return n;
}

extern "C" uint32_t rv32i_get_pc()                  { return pc; }
extern "C" void     rv32i_set_reg(int i, uint32_t v){ if (i) regs[i & 31] = v; }
extern "C" int      rv32i_is_halted()               { return halted; }
extern "C" void     rv32i_set_halted(int v)         { halted = v; }
extern "C" int      rv32i_exit_code()               { return exit_code; }
extern "C" void     rv32i_set_exit_code(int c)      { exit_code = c; }
extern "C" void     rv32i_set_m_ext(int v)          { enable_m = v; }
extern "C" void     rv32i_set_f_ext(int v)          { enable_f = v; }
extern "C" void     rv32i_set_a_ext(int v)          { enable_a = v; }
extern "C" void     rv32i_set_priv_mode(int v)      { enable_priv = v; if (v) priv_mode = 3; }
extern "C" void     rv32i_set_ram_offset(uint32_t v){ ram_offset = v; }
extern "C" uint32_t rv32i_get_mtime_lo()            { return (uint32_t)mtime; }
extern "C" uint32_t rv32i_get_mtime_hi()            { return (uint32_t)(mtime >> 32); }
extern "C" uint32_t rv32i_get_priv_mode()           { return priv_mode; }
extern "C" uint32_t rv32i_get_umode_lo()            { return (uint32_t)umode_count; }
extern "C" uint32_t rv32i_get_umode_hi()            { return (uint32_t)(umode_count >> 32); }
extern "C" uint32_t rv32i_get_last_trap()           { return last_trap; }
extern "C" uint32_t rv32i_get_trap_count()          { return trap_count; }
extern "C" uint32_t rv32i_get_mret_to_u()           { return mret_to_u; }
extern "C" uint32_t rv32i_get_mret_to_m()           { return mret_to_m; }
extern "C" uint32_t rv32i_get_uecall_count()        { return uecall_count; }

int __stdcall DllMain(void*, unsigned int, void*) { return 1; }

