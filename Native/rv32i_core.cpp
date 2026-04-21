// rv32i_core.cpp — Native RV32I + M/A/F + optional M/S/U privilege hot path.
// Windows only (ClangCL). Exports controlled by rv32i_core.def.
//
// Structure:
//   1. Types, state, memory helpers (CLINT routing)
//   2. Immediate decoders, F/M execution helpers
//   3. Privileged-mode CSR / trap / interrupt
//   4. Single templated do_step<MExt,FExt,AExt,Priv>() — the only ISA switch
//   5. rv32i_step_n dispatches to the right specialisation
//   6. Public C ABI

typedef signed char        int8_t;
typedef unsigned char      uint8_t;
typedef signed short       int16_t;
typedef unsigned short     uint16_t;
typedef unsigned int       uint32_t;
typedef signed   int       int32_t;
typedef unsigned long long uint64_t;
typedef signed   long long int64_t;
#define INT32_MIN (-2147483647 - 1)

extern "C" int _fltused = 0;  // required for float use without CRT

#include <intrin.h>
static __forceinline float host_sqrtf(float x) {
    return _mm_cvtss_f32(_mm_sqrt_ss(_mm_set_ss(x)));
}

typedef uint32_t (*mmio_read_fn) (uint32_t addr, int width);
typedef void     (*mmio_write_fn)(uint32_t addr, int width, uint32_t val);
typedef void     (*ecall_fn)     (uint32_t* regs);

// ── CPU state ────────────────────────────────────────────────────────
// Hot fields first.
static uint32_t regs[32];
static uint32_t fregs[32];
static uint32_t pc;
static int      halted;
static uint64_t mtime;
static uint8_t* ram;
static uint32_t ram_size;
static uint32_t ram_offset = 0;             // physical→ram base (e.g. 0x80000000 Linux)

static int             exit_code   = 0;
static int             enable_m    = 0;
static int             enable_f    = 0;
static int             enable_a    = 0;
static int             enable_priv = 0;
static uint64_t        mtimecmp    = ~0ULL;
static uint32_t        rsv_addr    = ~0u;   // A-ext LR/SC reservation
static mmio_read_fn    mmio_read   = nullptr;
static mmio_write_fn   mmio_write  = nullptr;
static ecall_fn        on_ecall    = nullptr;

// Diagnostics
static uint64_t umode_count  = 0;
static uint32_t last_trap    = 0;
static uint32_t trap_count   = 0;
static uint32_t mret_to_u    = 0;
static uint32_t mret_to_m    = 0;
static uint32_t uecall_count = 0;

// Privileged state (used iff enable_priv)
static uint32_t priv_mode    = 3;            // 0=U, 1=S, 3=M
static int      wfi_pending  = 0;
static uint32_t csr_mstatus, csr_mtvec, csr_mie, csr_mip;
static uint32_t csr_mepc, csr_mtval, csr_mcause, csr_mscratch;
static uint32_t csr_medeleg, csr_mideleg;
static uint32_t csr_stvec, csr_sscratch, csr_sepc, csr_scause, csr_stval, csr_satp;

// ── Memory access ────────────────────────────────────────────────────
// CLINT mtime/mtimecmp live natively. Two layouts are supported (standard
// SiFive 0x02000000 and mini-rv32ima 0x11000000) — handled by a tiny
// cold-path helper to keep mem_read/write minimal in the hot loop.

static __forceinline bool clint_read(uint32_t addr, uint32_t& out) {
    uint32_t lo = addr & 0x00FFFFFFu;
    uint32_t hi = addr & 0xFF000000u;
    if (hi != 0x02000000u && hi != 0x11000000u) return false;
    switch (lo) {
        case 0x0BFF8: out = (uint32_t) mtime;          return true;
        case 0x0BFFC: out = (uint32_t)(mtime    >>32); return true;
        case 0x04000: out = (uint32_t) mtimecmp;       return true;
        case 0x04004: out = (uint32_t)(mtimecmp >>32); return true;
        default:      return false;
    }
}
static __forceinline bool clint_write(uint32_t addr, uint32_t val) {
    uint32_t lo = addr & 0x00FFFFFFu;
    uint32_t hi = addr & 0xFF000000u;
    if (hi != 0x02000000u && hi != 0x11000000u) return false;
    switch (lo) {
        case 0x0BFF8: mtime    = (mtime    & 0xFFFFFFFF00000000ULL) |  val;                   return true;
        case 0x0BFFC: mtime    = (mtime    & 0x00000000FFFFFFFFULL) | ((uint64_t)val << 32);  return true;
        case 0x04000: mtimecmp = (mtimecmp & 0xFFFFFFFF00000000ULL) |  val;                   return true;
        case 0x04004: mtimecmp = (mtimecmp & 0x00000000FFFFFFFFULL) | ((uint64_t)val << 32);  return true;
        default:      return false;
    }
}

template<typename T>
static __forceinline T mem_read(uint32_t addr) {
    uint32_t raddr = addr - ram_offset;
    if (__builtin_expect(raddr < ram_size, 1)) {
        T v; __builtin_memcpy(&v, ram + raddr, sizeof(T)); return v;
    }
    if (sizeof(T) == 4) {
        uint32_t v;
        if (clint_read(addr, v)) return (T)v;
    }
    return mmio_read ? (T)mmio_read(addr, (int)sizeof(T)) : (T)0;
}
template<typename T>
static __forceinline void mem_write(uint32_t addr, T val) {
    uint32_t raddr = addr - ram_offset;
    if (__builtin_expect(raddr < ram_size, 1)) {
        __builtin_memcpy(ram + raddr, &val, sizeof(T)); return;
    }
    if (sizeof(T) == 4 && clint_write(addr, (uint32_t)val)) return;
    if (mmio_write) mmio_write(addr, (int)sizeof(T), (uint32_t)val);
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
static constexpr int32_t s_imm(uint32_t i) {
    return ((int32_t)(i & 0xFE000000) >> 20) | (int32_t)((i >> 7) & 0x1Fu);
}

// ── M-extension (MUL/DIV/REM) ────────────────────────────────────────
static inline uint32_t exec_m(uint32_t f3, int32_t s1, uint32_t u1, int32_t s2, uint32_t u2) {
    switch (f3) {
        case 0: return (uint32_t)(s1 * s2);
        case 1: return (uint32_t)((int64_t)s1 * s2 >> 32);
        case 2: return (uint32_t)((int64_t)s1 * (int64_t)u2 >> 32);
        case 3: return (uint32_t)((uint64_t)u1 * u2 >> 32);
        case 4: return s2 == 0 ? 0xFFFFFFFFu : (s1 == INT32_MIN && s2 == -1 ? (uint32_t)s1 : (uint32_t)(s1 / s2));
        case 5: return u2 == 0 ? 0xFFFFFFFFu : u1 / u2;
        case 6: return s2 == 0 ? u1 : (s1 == INT32_MIN && s2 == -1 ? 0u : (uint32_t)(s1 % s2));
        case 7: return u2 == 0 ? u1 : u1 % u2;
        default: return 0;
    }
}

// ── F-extension (single-precision) ───────────────────────────────────
static inline float f_get(int r)          { float v; __builtin_memcpy(&v, &fregs[r], 4); return v; }
static inline void  f_set(int r, float v) { __builtin_memcpy(&fregs[r], &v, 4); }
static inline int   f_isnan(int r) {
    return (fregs[r] & 0x7F800000u) == 0x7F800000u && (fregs[r] & 0x007FFFFFu);
}

// OP-FP (opcode 0x53). Split out so the main switch stays compact.
static void exec_fp_opfp(uint32_t instr, int rd, int rs1, int rs2, uint32_t f3, uint32_t f7) {
    switch (f7) {
        case 0x00: f_set(rd, f_get(rs1) + f_get(rs2)); return;  // FADD.S
        case 0x04: f_set(rd, f_get(rs1) - f_get(rs2)); return;  // FSUB.S
        case 0x08: f_set(rd, f_get(rs1) * f_get(rs2)); return;  // FMUL.S
        case 0x0C: f_set(rd, f_get(rs1) / f_get(rs2)); return;  // FDIV.S
        case 0x2C: f_set(rd, host_sqrtf(f_get(rs1)));  return;  // FSQRT.S
        case 0x10: {  // FSGNJ / FSGNJN / FSGNJX
            uint32_t a = fregs[rs1], b = fregs[rs2], sgn;
            switch (f3) {
                case 0: sgn =  b;     break;
                case 1: sgn = ~b;     break;
                default: sgn = a ^ b; break;
            }
            fregs[rd] = (sgn & 0x80000000u) | (a & 0x7FFFFFFFu);
            return;
        }
        case 0x14: {  // FMIN / FMAX
            float fa = f_get(rs1), fb = f_get(rs2);
            int   na = f_isnan(rs1), nb = f_isnan(rs2);
            if      (na && nb) fregs[rd] = 0x7FC00000u;
            else if (na)       f_set(rd, fb);
            else if (nb)       f_set(rd, fa);
            else               f_set(rd, (f3 == 0) ? (fa < fb ? fa : fb) : (fa > fb ? fa : fb));
            return;
        }
        case 0x50: {  // FEQ / FLT / FLE
            int valid = !f_isnan(rs1) && !f_isnan(rs2);
            float fa = f_get(rs1), fb = f_get(rs2);
            uint32_t r = 0;
            if (valid) {
                if      (f3 == 2) r = fa == fb;
                else if (f3 == 1) r = fa <  fb;
                else if (f3 == 0) r = fa <= fb;
            }
            regs[rd] = r;
            return;
        }
        case 0x60: {  // FCVT.W.S / FCVT.WU.S
            float fa = f_get(rs1);
            regs[rd] = (rs2 == 0)
                ? (f_isnan(rs1) ? 0x7FFFFFFFu : (uint32_t)(int32_t)fa)
                : ((f_isnan(rs1) || fa < 0.0f) ? 0u : (uint32_t)fa);
            return;
        }
        case 0x68:  // FCVT.S.W / FCVT.S.WU
            f_set(rd, rs2 == 0 ? (float)(int32_t)regs[rs1] : (float)regs[rs1]);
            return;
        case 0x70:  // FMV.X.W (f3==0) / FCLASS.S (f3==1)
            if (f3 == 0) { regs[rd] = fregs[rs1]; return; }
            if (f3 == 1) {
                uint32_t b = fregs[rs1], sgn = b >> 31, exp = (b >> 23) & 0xFF, m = b & 0x7FFFFF;
                uint32_t r;
                if      (exp == 0xFF) r = (m == 0) ? (sgn ? 1u : 128u) : ((m & 0x400000) ? 512u : 256u);
                else if (exp == 0)    r = (m == 0) ? (sgn ? 8u :  16u) : (sgn ? 4u : 32u);
                else                  r = sgn ? 2u : 64u;
                regs[rd] = r;
            }
            return;
        case 0x78:  // FMV.W.X
            fregs[rd] = regs[rs1];
            return;
        default:
            (void)instr;
            return;
    }
}

// ── Privileged-mode CSR access ───────────────────────────────────────
// sstatus is a restricted S-mode view of mstatus.
#define SSTATUS_MASK 0x800DE122u

static uint32_t priv_csr_read(uint32_t csrno) {
    switch (csrno) {
        // M-mode
        case 0x300: return csr_mstatus;
        case 0x301: return 0x40401101u;          // misa: RV32IMA+X
        case 0x302: return csr_medeleg;
        case 0x303: return csr_mideleg;
        case 0x304: return csr_mie;
        case 0x305: return csr_mtvec;
        case 0x340: return csr_mscratch;
        case 0x341: return csr_mepc;
        case 0x342: return csr_mcause;
        case 0x343: return csr_mtval;
        case 0x344: return csr_mip;
        case 0xF11: return 0xFF0FF0FFu;          // mvendorid
        case 0xF12: case 0xF13: case 0xF14: return 0u;  // marchid/mimpid/mhartid
        case 0x3A0: case 0x3B0:               return 0u;  // pmp stubs
        // S-mode (restricted view of M-mode state)
        case 0x100: return csr_mstatus & SSTATUS_MASK;   // sstatus
        case 0x104: return csr_mie & 0x333u;             // sie
        case 0x105: return csr_stvec;
        case 0x140: return csr_sscratch;
        case 0x141: return csr_sepc;
        case 0x142: return csr_scause;
        case 0x143: return csr_stval;
        case 0x144: return csr_mip & 0x333u;             // sip
        case 0x180: return csr_satp;
        // Counters: mtime acts as cycle/time/instret proxy.
        case 0xC00: case 0xB00: case 0xC01: case 0xB01: case 0xC02: case 0xB02:
            return (uint32_t)mtime;
        case 0xC80: case 0xB80: case 0xC81: case 0xB81: case 0xC82: case 0xB82:
            return (uint32_t)(mtime >> 32);
        default: return 0;
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
        case 0x344: csr_mip = (csr_mip & (1u<<7)) | (val & ~(1u<<7)); break; // MTIP read-only
        case 0x3A0: case 0x3B0: break;
        case 0x100: csr_mstatus = (csr_mstatus & ~SSTATUS_MASK) | (val & SSTATUS_MASK); break;
        case 0x104: csr_mie = (csr_mie & ~0x333u) | (val & 0x333u); break;
        case 0x105: csr_stvec    = val; break;
        case 0x140: csr_sscratch = val; break;
        case 0x141: csr_sepc     = val & ~1u; break;
        case 0x142: csr_scause   = val; break;
        case 0x143: csr_stval    = val; break;
        case 0x144: csr_mip = (csr_mip & ~0x333u) | (val & 0x333u); break;
        case 0x180: csr_satp     = val; break;
        default: break;
    }
}

// ── Trap dispatch ────────────────────────────────────────────────────
// Delegates to S-mode (stvec) when origin is < M and the cause is in
// medeleg/mideleg, else to M-mode (mtvec).
static void do_trap(uint32_t cause, uint32_t tval) {
    bool     is_intr = (cause & 0x80000000u) != 0u;
    uint32_t cidx    = cause & 0x1Fu;
    uint32_t bit     = 1u << cidx;
    bool     to_s    = (priv_mode < 3) &&
                       (is_intr ? (csr_mideleg & bit) : (csr_medeleg & bit));

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
        csr_mstatus  = (csr_mstatus & ~0x122u) | (spp << 8) | (sie << 5);
        priv_mode    = 1;
        pc = (is_intr && (csr_stvec & 1u)) ? (csr_stvec & ~3u) + cidx*4u : (csr_stvec & ~3u);
    } else {
        csr_mepc   = pc;
        csr_mcause = cause;
        csr_mtval  = tval;
        // mstatus: MPIE=MIE, MPP=current_priv, MIE=0
        uint32_t mie_b = (csr_mstatus >> 3) & 1u;
        csr_mstatus    = (csr_mstatus & ~0x1888u) | (priv_mode << 11) | (mie_b << 7);
        priv_mode      = 3;
        pc = (is_intr && (csr_mtvec & 1u)) ? (csr_mtvec & ~3u) + cidx*4u : (csr_mtvec & ~3u);
    }
}

// Returns true if an interrupt was dispatched. Priority: MEI>MSI>MTI>SEI>SSI>STI.
static bool check_interrupts() {
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
        bool fire = delegated
            ? (priv_mode == 0 || (priv_mode == 1 && sie_b))
            : (priv_mode <  3 || mie_b);
        if (fire) { do_trap(0x80000000u | prio[i], 0); return true; }
    }
    return false;
}

// SYSTEM (opcode 0x73). Returns trap cause (0 if none); writes nextpc by ref.
template<bool Priv>
static __forceinline uint32_t exec_system(uint32_t instr, int rd, uint32_t f3s,
                                           uint32_t& nextpc, uint32_t& trap_tval) {
    uint32_t fn = (instr >> 20) & 0xFFF;
    if (f3s == 0) {
        // ECALL / EBREAK / MRET / SRET / WFI
        if (fn == 0) {                              // ECALL
            if constexpr (Priv) {
                if (priv_mode == 0) uecall_count++;
                return (priv_mode == 3) ? 11u : (priv_mode == 1) ? 9u : 8u;
            } else if (on_ecall) on_ecall(regs);
        } else if (fn == 1) {                       // EBREAK
            if constexpr (Priv) { trap_tval = pc; return 3u; }
            else                 halted = 1;
        } else if constexpr (Priv) {
            if (fn == 0x102) {                      // SRET
                uint32_t spie = (csr_mstatus >> 5) & 1u;
                uint32_t spp  = (csr_mstatus >> 8) & 1u;
                csr_mstatus   = (csr_mstatus & ~0x122u) | (1u << 5) | (spie << 1);
                priv_mode = spp;
                nextpc    = csr_sepc;
            } else if (fn == 0x302) {               // MRET
                uint32_t mpie = (csr_mstatus >> 7) & 1u;
                uint32_t mpp  = (csr_mstatus >> 11) & 3u;
                csr_mstatus   = (csr_mstatus & ~0x1888u) | (1u << 7) | (mpie << 3);
                priv_mode = mpp;
                nextpc    = csr_mepc;
                if (mpp == 0) mret_to_u++; else mret_to_m++;
            } else if (fn == 0x105) {               // WFI
                // Linux disables MIE before WFI; spec allows wake regardless.
                csr_mstatus |= 8u;
                wfi_pending  = 1;
            }
        }
        return 0;
    }
    // CSR ops (Zicsr)
    if constexpr (Priv) {
        int      rs1imm = (instr >> 15) & 0x1F;
        uint32_t rs1v   = regs[rs1imm];
        uint32_t old    = priv_csr_read(fn);
        regs[rd] = old;
        uint32_t nval;
        switch (f3s) {
            case 1: nval = rs1v;                    break;  // CSRRW
            case 2: nval = old |  rs1v;             break;  // CSRRS
            case 3: nval = old & ~rs1v;             break;  // CSRRC
            case 5: nval = (uint32_t)rs1imm;        break;  // CSRRWI
            case 6: nval = old |  (uint32_t)rs1imm; break;  // CSRRSI
            case 7: nval = old & ~(uint32_t)rs1imm; break;  // CSRRCI
            default: nval = old;                    break;
        }
        if (f3s == 1 || f3s == 5 || rs1imm != 0) priv_csr_write(fn, nval);
    } else {
        regs[rd] = 0;  // bare-metal: CSR reads return 0
    }
    return 0;
}

// ── Single templated step ────────────────────────────────────────────
// Compile-time flags collapse to dead-code elimination — one source, many
// specialisations. Hot integer path is the same as the original fast
// paths since unused branches vanish.
template<bool MExt, bool FExt, bool AExt, bool Priv>
static __forceinline void do_step() {
    if constexpr (Priv) {
        if (check_interrupts()) { regs[0] = 0; mtime++; return; }
        if (wfi_pending)        { regs[0] = 0; mtime++; return; }
    }

    const uint32_t instr  = mem_read<uint32_t>(pc);
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
    uint32_t nextpc       = pc + 4;
    uint32_t trap_cause   = 0;
    uint32_t trap_tval    = 0;

    switch (opcode) {

    case 0x37: regs[rd] = instr & 0xFFFFF000u;                           break;  // LUI
    case 0x17: regs[rd] = pc + (instr & 0xFFFFF000u);                    break;  // AUIPC
    case 0x6F: regs[rd] = pc + 4; nextpc = pc + j_imm(instr);            break;  // JAL
    case 0x67: { uint32_t t = (uint32_t)(s1 + i_imm(instr)) & ~1u;               // JALR
                 regs[rd] = pc + 4; nextpc = t;                          break; }

    case 0x63: {  // BRANCH
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

    case 0x03: {  // LOAD
        uint32_t addr = (uint32_t)(s1 + i_imm(instr));
        switch (f3) {
            case 0: regs[rd] = (uint32_t)(int8_t) mem_read<uint8_t> (addr); break;
            case 1: regs[rd] = (uint32_t)(int16_t)mem_read<uint16_t>(addr); break;
            case 2: regs[rd] =                    mem_read<uint32_t>(addr); break;
            case 4: regs[rd] =                    mem_read<uint8_t> (addr); break;
            case 5: regs[rd] =                    mem_read<uint16_t>(addr); break;
        }
        break;
    }

    case 0x23: {  // STORE
        uint32_t addr = (uint32_t)(s1 + s_imm(instr));
        switch (f3) {
            case 0: mem_write<uint8_t> (addr, (uint8_t) u2); break;
            case 1: mem_write<uint16_t>(addr, (uint16_t)u2); break;
            case 2: mem_write<uint32_t>(addr,            u2); break;
        }
        break;
    }

    case 0x13: {  // OP-IMM
        const int32_t imm = i_imm(instr);
        const int     sh  = (instr >> 20) & 0x1F;
        uint32_t r;
        switch (f3) {
            case 0: r = (uint32_t)(s1 + imm);                            break;
            case 1: r = u1 << sh;                                         break;
            case 2: r = s1 < imm           ? 1u : 0u;                    break;
            case 3: r = u1 < (uint32_t)imm ? 1u : 0u;                    break;
            case 4: r = u1 ^ (uint32_t)imm;                              break;
            case 5: r = f7 == 0x20 ? (uint32_t)(s1 >> sh) : u1 >> sh;    break;
            case 6: r = u1 | (uint32_t)imm;                              break;
            case 7: r = u1 & (uint32_t)imm;                              break;
            default: r = 0;
        }
        regs[rd] = r;
        break;
    }

    case 0x33: {  // OP
        uint32_t r;
        if (MExt && f7 == 0x01) {
            r = exec_m(f3, s1, u1, s2, u2);
        } else {
            const int sh = s2 & 0x1F;
            switch (f3) {
                case 0: r = f7 == 0x20 ? (uint32_t)(s1 - s2) : (uint32_t)(s1 + s2); break;
                case 1: r = u1 << sh;                                                break;
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

    case 0x2F:  // AMO (RV32A)
        if constexpr (AExt) {
            const uint32_t irmid = (instr >> 27) & 0x1F;
            const uint32_t addr  = u1;
            if (irmid == 2) {                                            // LR.W
                regs[rd] = mem_read<uint32_t>(addr);
                rsv_addr = addr;
            } else if (irmid == 3) {                                     // SC.W
                if (rsv_addr == addr) { mem_write<uint32_t>(addr, u2); regs[rd] = 0; }
                else                                                       regs[rd] = 1;
                rsv_addr = ~0u;
            } else {
                uint32_t old = mem_read<uint32_t>(addr);
                regs[rd]     = old;
                uint32_t nw;
                switch (irmid) {
                    case  1: nw = u2;                                       break;  // SWAP
                    case  0: nw = old + u2;                                 break;  // ADD
                    case  4: nw = old ^ u2;                                 break;  // XOR
                    case 12: nw = old & u2;                                 break;  // AND
                    case  8: nw = old | u2;                                 break;  // OR
                    case 16: nw = (int32_t)u2 < (int32_t)old ? u2 : old;    break;  // MIN
                    case 20: nw = (int32_t)u2 > (int32_t)old ? u2 : old;    break;  // MAX
                    case 24: nw = u2 < old ? u2 : old;                      break;  // MINU
                    case 28: nw = u2 > old ? u2 : old;                      break;  // MAXU
                    default: nw = old;                                       break;
                }
                mem_write<uint32_t>(addr, nw);
            }
        }
        break;

    case 0x07:  // FLW
        if constexpr (FExt) if (f3 == 2) fregs[rd] = mem_read<uint32_t>((uint32_t)(s1 + i_imm(instr)));
        break;

    case 0x27:  // FSW
        if constexpr (FExt) if (f3 == 2) mem_write<uint32_t>((uint32_t)(s1 + s_imm(instr)), fregs[rs2]);
        break;

    case 0x43: case 0x47: case 0x4B: case 0x4F:  // FMADD/FMSUB/FNMSUB/FNMADD
        if constexpr (FExt) {
            const int rs3 = (int)((instr >> 27) & 0x1F);
            const float fa = f_get(rs1), fb = f_get(rs2), fc = f_get(rs3);
            float fr;
            switch (opcode) {
                case 0x43: fr =  fa*fb + fc; break;
                case 0x47: fr =  fa*fb - fc; break;
                case 0x4B: fr = -fa*fb + fc; break;
                default:   fr = -fa*fb - fc; break;
            }
            f_set(rd, fr);
        }
        break;

    case 0x53:  // OP-FP
        if constexpr (FExt) exec_fp_opfp(instr, rd, rs1, rs2, f3, f7);
        break;

    case 0x0F: break;  // FENCE / FENCE.I — NOP

    case 0x73: {  // SYSTEM
        const uint32_t f3s = (instr >> 12) & 0x7;
        trap_cause = exec_system<Priv>(instr, rd, f3s, nextpc, trap_tval);
        break;
    }

    }  // switch (opcode)

    if constexpr (Priv) {
        if (trap_cause) { do_trap(trap_cause, trap_tval); regs[0] = 0; mtime++; return; }
    }

    regs[0] = 0;
    pc = nextpc;
    if constexpr (Priv) { if (priv_mode == 0) umode_count++; }
    mtime++;
}

// ── rv32i_step_n: pick the right specialisation ──────────────────────
// Bare-metal RV32IM (Doom/Craft/tests):     <1,0,0,0>
// Bare-metal RV32IMF (Mp4Player):           <1,1,0,0>
// Anything else (priv, A-ext, mixed):       <1,1,1,1>
template<bool M, bool F, bool A, bool P>
static int run_loop(int n) {
    for (int i = 0; i < n; i++) {
        do_step<M,F,A,P>();
        if (__builtin_expect(halted, 0)) return -(i + 1);
    }
    return n;
}

// 16 specialisations indexed by (M<<3)|(F<<2)|(A<<1)|P. Each is a tight
// fully-inlined loop with all `if constexpr` branches resolved.
typedef int (*loop_fn)(int);
#define R(m,f,a,p) run_loop<m,f,a,p>
static const loop_fn loop_table[16] = {
    R(0,0,0,0), R(0,0,0,1), R(0,0,1,0), R(0,0,1,1),
    R(0,1,0,0), R(0,1,0,1), R(0,1,1,0), R(0,1,1,1),
    R(1,0,0,0), R(1,0,0,1), R(1,0,1,0), R(1,0,1,1),
    R(1,1,0,0), R(1,1,0,1), R(1,1,1,0), R(1,1,1,1),
};
#undef R

extern "C" int rv32i_step_n(int n) {
    int idx = (enable_m ? 8 : 0) | (enable_f ? 4 : 0) | (enable_a ? 2 : 0) | (enable_priv ? 1 : 0);
    return loop_table[idx](n);
}

// ── Public C ABI ─────────────────────────────────────────────────────

extern "C" void rv32i_init(uint8_t* ram_ptr, uint32_t ram_sz, uint32_t entry, int m_ext) {
    ram = ram_ptr; ram_size = ram_sz;
    for (int i = 0; i < 32; i++) { regs[i] = 0; fregs[i] = 0; }
    pc = entry; halted = 0; exit_code = 0;
    enable_m = m_ext; enable_f = 0; enable_a = 0; enable_priv = 0;
    ram_offset = 0; rsv_addr = ~0u;
    umode_count = 0; last_trap = 0; trap_count = 0;
    mret_to_u = 0; mret_to_m = 0; uecall_count = 0;
    mtime = 0; mtimecmp = ~0ULL;
    mmio_read = nullptr; mmio_write = nullptr; on_ecall = nullptr;
    priv_mode = 3; wfi_pending = 0;
    csr_mstatus = csr_mtvec = csr_mie = csr_mip = 0;
    csr_mepc = csr_mtval = csr_mcause = csr_mscratch = 0;
    csr_medeleg = csr_mideleg = 0;
    csr_stvec = csr_sscratch = csr_sepc = csr_scause = csr_stval = csr_satp = 0;
}

extern "C" void rv32i_destroy()                                    { ram = nullptr; ram_size = 0; }
extern "C" void rv32i_set_mmio(mmio_read_fn r, mmio_write_fn w)    { mmio_read = r; mmio_write = w; }
extern "C" void rv32i_set_ecall(ecall_fn h)                        { on_ecall = h; }

// Compact getter/setter trampolines (mirrors rv32i_core.def).
#define GET(name, expr)   extern "C" uint32_t name() { return (uint32_t)(expr); }
#define GETI(name, expr)  extern "C" int      name() { return (int)(expr); }
#define SETV(name, T, stmt) extern "C" void name(T v) { stmt; }

GET (rv32i_get_pc,           pc)
GETI(rv32i_is_halted,        halted)
GETI(rv32i_exit_code,        exit_code)
GET (rv32i_get_mtime_lo,     (uint32_t) mtime)
GET (rv32i_get_mtime_hi,     (uint32_t)(mtime >> 32))
GET (rv32i_get_priv_mode,    priv_mode)
GET (rv32i_get_umode_lo,     (uint32_t) umode_count)
GET (rv32i_get_umode_hi,     (uint32_t)(umode_count >> 32))
GET (rv32i_get_last_trap,    last_trap)
GET (rv32i_get_trap_count,   trap_count)
GET (rv32i_get_mret_to_u,    mret_to_u)
GET (rv32i_get_mret_to_m,    mret_to_m)
GET (rv32i_get_uecall_count, uecall_count)

extern "C" void rv32i_set_reg(int i, uint32_t v)   { if (i) regs[i & 31] = v; }
SETV(rv32i_set_halted,     int,      halted = v)
SETV(rv32i_set_exit_code,  int,      exit_code = v)
SETV(rv32i_set_m_ext,      int,      enable_m = v)
SETV(rv32i_set_f_ext,      int,      enable_f = v)
SETV(rv32i_set_a_ext,      int,      enable_a = v)
SETV(rv32i_set_priv_mode,  int,      enable_priv = v; if (v) priv_mode = 3)
SETV(rv32i_set_ram_offset, uint32_t, ram_offset = v)

int __stdcall DllMain(void*, unsigned int, void*) { return 1; }
