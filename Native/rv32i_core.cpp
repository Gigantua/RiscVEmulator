// rv32i_core.cpp — Native RISC-V hot path. Windows only (ClangCL).
//
// ── ISA implemented ─────────────────────────────────────────────────
//
//   RV32I            Base integer ISA (all 40 instructions).
//   F                Single-precision float (FLW/FSW, FADD/FSUB/FMUL/FDIV/FSQRT,
//                    FMIN/FMAX, FMADD/FMSUB/FNMADD/FNMSUB, FSGNJ[N|X],
//                    FCVT.W[U].S / FCVT.S.W[U], FEQ/FLT/FLE, FCLASS, FMV.X.W/FMV.W.X).
//                    No exception flags, rounding mode ignored (RNE only).
//   Zicsr            CSR R/W (CSRRW, CSRRS, CSRRC + immediate variants).
//   Zifencei         FENCE / FENCE.I — implemented as NOP (single-hart, no I-cache).
//
// ── Privileged architecture (always on; CPU boots in M-mode) ───────
//
//   Machine, Supervisor, User modes with trap delegation (medeleg/mideleg).
//   MRET / SRET / WFI / ECALL / EBREAK.
//   CSRs: mstatus, misa(RO), medeleg, mideleg, mie, mip, mtvec, mscratch,
//         mepc, mcause, mtval, sstatus(view), sie(view), stvec, sscratch,
//         sepc, scause, stval, sip(view), satp (stored only — no MMU walk),
//         mhartid(RO=0), cycle/time/instret counters (alias mtime).
//   Interrupts: M/S timer (MTIP/STIP), M/S software (MSIP/SSIP),
//               M/S external (MEIP/SEIP). Currently only MTIP is auto-raised
//               (from mtime≥mtimecmp); other bits writable via CSR.
//   No MMU translation — satp is stored but loads/stores use the bare
//   guest-physical address.
//
// ── Host integration model ──────────────────────────────────────────
//
// The CPU knows nothing about peripherals. Every memory access is a single
// pointer dereference into a host-provided base buffer:
//     mem_read<T>(addr) = *(volatile T*)(cpu.mem + addr)
//
// The host (Emulator.cs) reserves one big chunk of VA and commits pages at
// guest offsets — PAGE_READWRITE for plain memory (RAM, FB, PCM) and
// PAGE_NOACCESS for guarded peripherals. Accesses to guarded pages raise
// AVs that the host's vectored exception handler dispatches to peripheral
// Read/Write. From step()'s perspective every access is just memory.
//
// mtime/mtimecmp are CPU-internal counters (like a CSR), accessed by the
// host's CLINT peripheral via the rv32i_get_mtime{,cmp} / rv32i_set_*
// trampolines. step() never special-cases CLINT addresses.

#include <cstdint>
#include <xmmintrin.h>          // _mm_sqrt_ss — avoids -O0 libcall to sqrtf.

extern "C" int _fltused = 0;    // MSVC float-ABI marker; required by the linker.

static inline float fsqrt(float x) { return _mm_cvtss_f32(_mm_sqrt_ss(_mm_set_ss(x))); }

// Provide our own mem{set,cpy} so aggregate value-init (`cpu = {}`) and other
// compiler-emitted copies link without a CRT (we build -nodefaultlib).
extern "C" void* memset(void* dst, int c, unsigned long long n) {
    auto* d = (unsigned char*)dst;
    for (unsigned long long i = 0; i < n; i++) d[i] = (unsigned char)c;
    return dst;
}
extern "C" void* memcpy(void* dst, const void* src, unsigned long long n) {
    auto* d = (unsigned char*)dst;
    auto* s = (const unsigned char*)src;
    for (unsigned long long i = 0; i < n; i++) d[i] = s[i];
    return dst;
}

// ── State ────────────────────────────────────────────────────────────

struct CPU_State {
    uint32_t regs[32];
    uint32_t fregs[32];
    uint32_t pc;
    int      halted;
    uint64_t mtime;
    uint64_t mtimecmp;
    uint8_t* mem;               // host base; every guest load/store hits *(mem + addr)

    uint32_t priv_mode;
    int      wfi_pending;
    uint32_t csr_mstatus, csr_mtvec, csr_mie, csr_mip;
    uint32_t csr_mepc, csr_mtval, csr_mcause, csr_mscratch;
    uint32_t csr_medeleg, csr_mideleg;
    uint32_t csr_stvec, csr_sscratch, csr_sepc, csr_scause, csr_stval, csr_satp;
};

static CPU_State cpu;

// ── Memory access ───────────────────────────────────────────────────

template<typename T>
static __forceinline T mem_read(CPU_State& cpu, uint32_t addr) {
    return *(volatile T*)(cpu.mem + addr);
}
template<typename T>
static __forceinline void mem_write(CPU_State& cpu, uint32_t addr, T val) {
    *(volatile T*)(cpu.mem + addr) = val;
}

// ── Immediate decoders ──────────────────────────────────────────────

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

// ── M-extension removed ─────────────────────────────────────────────
//
// MUL/MULH[SU|U]/DIV[U]/REM[U] are not implemented. Any OP-class
// instruction with funct7 == 0x01 traps as an illegal instruction;
// the guest's compiler lowers * and / to __mulsi3 / __divsi3 libcalls.

// ── F-extension (single-precision) ──────────────────────────────────

static inline float f_get(CPU_State& cpu, int r)          { float v; __builtin_memcpy(&v, &cpu.fregs[r], 4); return v; }
static inline void  f_set(CPU_State& cpu, int r, float v) { __builtin_memcpy(&cpu.fregs[r], &v, 4); }
static inline int   f_isnan(CPU_State& cpu, int r) {
    return (cpu.fregs[r] & 0x7F800000u) == 0x7F800000u && (cpu.fregs[r] & 0x007FFFFFu);
}

static void exec_fp_opfp(CPU_State& cpu, int rd, int rs1, int rs2, uint32_t f3, uint32_t f7) {
    switch (f7) {
        case 0x00: f_set(cpu, rd, f_get(cpu, rs1) + f_get(cpu, rs2));          return;
        case 0x04: f_set(cpu, rd, f_get(cpu, rs1) - f_get(cpu, rs2));          return;
        case 0x08: f_set(cpu, rd, f_get(cpu, rs1) * f_get(cpu, rs2));          return;
        case 0x0C: f_set(cpu, rd, f_get(cpu, rs1) / f_get(cpu, rs2));          return;
        case 0x2C: f_set(cpu, rd, fsqrt(f_get(cpu, rs1)));                     return;
        case 0x10: {
            uint32_t a = cpu.fregs[rs1], b = cpu.fregs[rs2];
            uint32_t sgn = (f3 == 0) ? b : (f3 == 1) ? ~b : a ^ b;
            cpu.fregs[rd] = (sgn & 0x80000000u) | (a & 0x7FFFFFFFu);
            return;
        }
        case 0x14: {
            float fa = f_get(cpu, rs1), fb = f_get(cpu, rs2);
            int   na = f_isnan(cpu, rs1), nb = f_isnan(cpu, rs2);
            if      (na && nb) cpu.fregs[rd] = 0x7FC00000u;
            else if (na)       f_set(cpu, rd, fb);
            else if (nb)       f_set(cpu, rd, fa);
            else               f_set(cpu, rd, (f3 == 0) ? (fa < fb ? fa : fb) : (fa > fb ? fa : fb));
            return;
        }
        case 0x50: {
            int valid = !f_isnan(cpu, rs1) && !f_isnan(cpu, rs2);
            float fa = f_get(cpu, rs1), fb = f_get(cpu, rs2);
            uint32_t r = 0;
            if (valid) {
                if      (f3 == 2) r = fa == fb;
                else if (f3 == 1) r = fa <  fb;
                else if (f3 == 0) r = fa <= fb;
            }
            cpu.regs[rd] = r;
            return;
        }
        case 0x60: {
            float fa = f_get(cpu, rs1);
            cpu.regs[rd] = (rs2 == 0)
                ? (f_isnan(cpu, rs1) ? 0x7FFFFFFFu : (uint32_t)(int32_t)fa)
                : ((f_isnan(cpu, rs1) || fa < 0.0f) ? 0u : (uint32_t)fa);
            return;
        }
        case 0x68:
            f_set(cpu, rd, rs2 == 0 ? (float)(int32_t)cpu.regs[rs1] : (float)cpu.regs[rs1]);
            return;
        case 0x70:
            if (f3 == 0) { cpu.regs[rd] = cpu.fregs[rs1]; return; }
            if (f3 == 1) {
                uint32_t b = cpu.fregs[rs1], sgn = b >> 31, exp = (b >> 23) & 0xFF, m = b & 0x7FFFFF;
                uint32_t r;
                if      (exp == 0xFF) r = (m == 0) ? (sgn ? 1u : 128u) : ((m & 0x400000) ? 512u : 256u);
                else if (exp == 0)    r = (m == 0) ? (sgn ? 8u :  16u) : (sgn ? 4u : 32u);
                else                  r = sgn ? 2u : 64u;
                cpu.regs[rd] = r;
            }
            return;
        case 0x78:
            cpu.fregs[rd] = cpu.regs[rs1];
            return;
        default:
            return;
    }
}

// ── Privileged-mode CSR / trap ──────────────────────────────────────
//
// Single CSR accessor returning a reference to the backing slot. Reads dereference
// the result; writes assign through it. RO/computed CSRs (misa, mvendorid, mtime
// views, unknown numbers) stage their value in a static scratch slot — writes
// there are discarded, which is exactly RO semantics. S-mode views (sstatus,
// sie, sip) alias the M-mode storage directly.

static uint32_t& priv_csr(CPU_State& cpu, uint32_t csrno) {
    static uint32_t scratch;
    switch (csrno) {
        case 0x300: case 0x100: return cpu.csr_mstatus;     // mstatus / sstatus
        case 0x302:             return cpu.csr_medeleg;
        case 0x303:             return cpu.csr_mideleg;
        case 0x304: case 0x104: return cpu.csr_mie;         // mie / sie
        case 0x305:             return cpu.csr_mtvec;
        case 0x340:             return cpu.csr_mscratch;
        case 0x341:             return cpu.csr_mepc;
        case 0x342:             return cpu.csr_mcause;
        case 0x343:             return cpu.csr_mtval;
        case 0x344: case 0x144: return cpu.csr_mip;         // mip / sip
        case 0x105:             return cpu.csr_stvec;
        case 0x140:             return cpu.csr_sscratch;
        case 0x141:             return cpu.csr_sepc;
        case 0x142:             return cpu.csr_scause;
        case 0x143:             return cpu.csr_stval;
        case 0x180:             return cpu.csr_satp;
        case 0x301:             scratch = 0x40401101u;            return scratch;  // misa (RO)
        case 0xF11:             scratch = 0xFF0FF0FFu;            return scratch;  // mvendorid (RO)
        case 0xC00: case 0xB00: case 0xC01: case 0xB01: case 0xC02: case 0xB02:
                                scratch = (uint32_t) cpu.mtime;        return scratch;
        case 0xC80: case 0xB80: case 0xC81: case 0xB81: case 0xC82: case 0xB82:
                                scratch = (uint32_t)(cpu.mtime >> 32); return scratch;
        default:                scratch = 0;                  return scratch;
    }
}

static void do_trap(CPU_State& cpu, uint32_t cause, uint32_t tval) {
    bool     is_intr = (cause & 0x80000000u) != 0u;
    uint32_t cidx    = cause & 0x1Fu;
    uint32_t bit     = 1u << cidx;
    bool     to_s    = (cpu.priv_mode < 3) &&
                       (is_intr ? (cpu.csr_mideleg & bit) : (cpu.csr_medeleg & bit));

    cpu.wfi_pending = 0;

    if (to_s) {
        cpu.csr_sepc   = cpu.pc;
        cpu.csr_scause = cause;
        cpu.csr_stval  = tval;
        uint32_t sie = (cpu.csr_mstatus >> 1) & 1u;
        uint32_t spp = cpu.priv_mode & 1u;
        cpu.csr_mstatus = (cpu.csr_mstatus & ~0x122u) | (spp << 8) | (sie << 5);
        cpu.priv_mode   = 1;
        cpu.pc = (is_intr && (cpu.csr_stvec & 1u)) ? (cpu.csr_stvec & ~3u) + cidx*4u : (cpu.csr_stvec & ~3u);
    } else {
        cpu.csr_mepc   = cpu.pc;
        cpu.csr_mcause = cause;
        cpu.csr_mtval  = tval;
        uint32_t mie_b = (cpu.csr_mstatus >> 3) & 1u;
        cpu.csr_mstatus  = (cpu.csr_mstatus & ~0x1888u) | (cpu.priv_mode << 11) | (mie_b << 7);
        cpu.priv_mode    = 3;
        cpu.pc = (is_intr && (cpu.csr_mtvec & 1u)) ? (cpu.csr_mtvec & ~3u) + cidx*4u : (cpu.csr_mtvec & ~3u);
    }
}

static bool check_interrupts(CPU_State& cpu) {
    if (cpu.mtime >= cpu.mtimecmp) cpu.csr_mip |=  (1u << 7);
    else                           cpu.csr_mip &= ~(1u << 7);

    uint32_t pending = cpu.csr_mip & cpu.csr_mie;
    if (!pending) return false;

    uint32_t mie_b = (cpu.csr_mstatus >> 3) & 1u;
    uint32_t sie_b = (cpu.csr_mstatus >> 1) & 1u;
    static const uint32_t prio[6] = { 11, 3, 7, 9, 1, 5 };
    for (int i = 0; i < 6; i++) {
        uint32_t b = 1u << prio[i];
        if (!(pending & b)) continue;
        bool delegated = (cpu.csr_mideleg & b) != 0u;
        bool fire = delegated
            ? (cpu.priv_mode == 0 || (cpu.priv_mode == 1 && sie_b))
            : (cpu.priv_mode <  3 || mie_b);
        if (fire) { do_trap(cpu, 0x80000000u | prio[i], 0); return true; }
    }
    return false;
}

static uint32_t exec_system(CPU_State& cpu, uint32_t instr, int rd, uint32_t f3s,
                            uint32_t& nextpc, uint32_t& trap_tval) {
    uint32_t fn = (instr >> 20) & 0xFFF;
    if (f3s == 0) {
        if (fn == 0)      return (cpu.priv_mode == 3) ? 11u : (cpu.priv_mode == 1) ? 9u : 8u;
        if (fn == 1)      { trap_tval = cpu.pc; return 3u; }
        if (fn == 0x102) {
            uint32_t spie = (cpu.csr_mstatus >> 5) & 1u;
            uint32_t spp  = (cpu.csr_mstatus >> 8) & 1u;
            cpu.csr_mstatus = (cpu.csr_mstatus & ~0x122u) | (1u << 5) | (spie << 1);
            cpu.priv_mode   = spp;
            nextpc          = cpu.csr_sepc;
        } else if (fn == 0x302) {
            uint32_t mpie = (cpu.csr_mstatus >> 7) & 1u;
            uint32_t mpp  = (cpu.csr_mstatus >> 11) & 3u;
            cpu.csr_mstatus = (cpu.csr_mstatus & ~0x1888u) | (1u << 7) | (mpie << 3);
            cpu.priv_mode   = mpp;
            nextpc          = cpu.csr_mepc;
        } else if (fn == 0x105) {
            cpu.csr_mstatus |= 8u;
            cpu.wfi_pending  = 1;
        }
        return 0;
    }
    int       rs1imm = (instr >> 15) & 0x1F;
    uint32_t  rs1v   = cpu.regs[rs1imm];
    uint32_t& slot   = priv_csr(cpu, fn);
    uint32_t  old    = slot;
    cpu.regs[rd] = old;
    uint32_t nval = old;
    switch (f3s) {
        case 1: nval = rs1v;                    break;
        case 2: nval = old |  rs1v;             break;
        case 3: nval = old & ~rs1v;             break;
        case 5: nval = (uint32_t)rs1imm;        break;
        case 6: nval = old |  (uint32_t)rs1imm; break;
        case 7: nval = old & ~(uint32_t)rs1imm; break;
    }
    if (f3s == 1 || f3s == 5 || rs1imm != 0) slot = nval;
    return 0;
}

// ── do_step ─────────────────────────────────────────────────────────

static constexpr void do_step(CPU_State& cpu) {
    if (check_interrupts(cpu)) { cpu.regs[0] = 0; return; }
    if (cpu.wfi_pending)       { cpu.regs[0] = 0; return; }

    const uint32_t instr  = mem_read<uint32_t>(cpu, cpu.pc);
    const uint32_t opcode = instr & 0x7F;
    const int      rd     = (instr >>  7) & 0x1F;
    const int      rs1    = (instr >> 15) & 0x1F;
    const int      rs2    = (instr >> 20) & 0x1F;
    const uint32_t f3     = (instr >> 12) & 0x7;
    const uint32_t f7     = (instr >> 25) & 0x7F;
    const int32_t  s1     = (int32_t)cpu.regs[rs1];
    const uint32_t u1     = cpu.regs[rs1];
    const int32_t  s2     = (int32_t)cpu.regs[rs2];
    const uint32_t u2     = cpu.regs[rs2];
    uint32_t nextpc       = cpu.pc + 4;
    uint32_t trap_cause   = 0;
    uint32_t trap_tval    = 0;

    switch (opcode) {

    case 0x37: cpu.regs[rd] = instr & 0xFFFFF000u;                       break;
    case 0x17: cpu.regs[rd] = cpu.pc + (instr & 0xFFFFF000u);            break;
    case 0x6F: cpu.regs[rd] = cpu.pc + 4; nextpc = cpu.pc + j_imm(instr); break;
    case 0x67: { uint32_t t = (uint32_t)(s1 + i_imm(instr)) & ~1u;
                 cpu.regs[rd] = cpu.pc + 4; nextpc = t;                  break; }

    case 0x63: {
        int taken = 0;
        switch (f3) {
            case 0: taken = u1 == u2; break;  case 1: taken = u1 != u2; break;
            case 4: taken = s1 <  s2; break;  case 5: taken = s1 >= s2; break;
            case 6: taken = u1 <  u2; break;  case 7: taken = u1 >= u2; break;
        }
        if (taken) nextpc = cpu.pc + b_imm(instr);
        break;
    }

    case 0x03: {
        uint32_t addr = (uint32_t)(s1 + i_imm(instr));
        switch (f3) {
            case 0: cpu.regs[rd] = (uint32_t)(int8_t) mem_read<uint8_t> (cpu, addr); break;
            case 1: cpu.regs[rd] = (uint32_t)(int16_t)mem_read<uint16_t>(cpu, addr); break;
            case 2: cpu.regs[rd] =                    mem_read<uint32_t>(cpu, addr); break;
            case 4: cpu.regs[rd] =                    mem_read<uint8_t> (cpu, addr); break;
            case 5: cpu.regs[rd] =                    mem_read<uint16_t>(cpu, addr); break;
        }
        break;
    }

    case 0x23: {
        uint32_t addr = (uint32_t)(s1 + s_imm(instr));
        switch (f3) {
            case 0: mem_write<uint8_t> (cpu, addr, (uint8_t) u2); break;
            case 1: mem_write<uint16_t>(cpu, addr, (uint16_t)u2); break;
            case 2: mem_write<uint32_t>(cpu, addr,            u2); break;
        }
        break;
    }

    case 0x13: {
        const int32_t imm = i_imm(instr);
        const int     sh  = (instr >> 20) & 0x1F;
        uint32_t r = 0;
        switch (f3) {
            case 0: r = (uint32_t)(s1 + imm);                            break;
            case 1: r = u1 << sh;                                         break;
            case 2: r = s1 < imm           ? 1u : 0u;                    break;
            case 3: r = u1 < (uint32_t)imm ? 1u : 0u;                    break;
            case 4: r = u1 ^ (uint32_t)imm;                              break;
            case 5: r = f7 == 0x20 ? (uint32_t)(s1 >> sh) : u1 >> sh;    break;
            case 6: r = u1 | (uint32_t)imm;                              break;
            case 7: r = u1 & (uint32_t)imm;                              break;
        }
        cpu.regs[rd] = r;
        break;
    }

    case 0x33: {
        uint32_t r = 0;
        if (f7 == 0x01) {                        // M-extension removed — MUL/MULH*/DIV*/REM* all trap
            trap_cause = 2; trap_tval = instr;
            break;
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
            }
        }
        cpu.regs[rd] = r;
        break;
    }

    case 0x2F: {
        trap_cause = 2; trap_tval = instr;      // A-extension removed — LR/SC/AMO all trap
        break;
    }

    case 0x07:
        if (f3 == 2) cpu.fregs[rd] = mem_read<uint32_t>(cpu, (uint32_t)(s1 + i_imm(instr)));
        break;

    case 0x27:
        if (f3 == 2) mem_write<uint32_t>(cpu, (uint32_t)(s1 + s_imm(instr)), cpu.fregs[rs2]);
        break;

    case 0x43: case 0x47: case 0x4B: case 0x4F: {
        const int rs3 = (int)((instr >> 27) & 0x1F);
        const float fa = f_get(cpu, rs1), fb = f_get(cpu, rs2), fc = f_get(cpu, rs3);
        float fr = (opcode == 0x43) ?  fa*fb + fc
                 : (opcode == 0x47) ?  fa*fb - fc
                 : (opcode == 0x4B) ? -fa*fb + fc
                 :                    -fa*fb - fc;
        f_set(cpu, rd, fr);
        break;
    }

    case 0x53:
        exec_fp_opfp(cpu, rd, rs1, rs2, f3, f7);
        break;

    case 0x0F: break;

    case 0x73:
        trap_cause = exec_system(cpu, instr, rd, f3, nextpc, trap_tval);
        break;

    }

    if (trap_cause) {
        do_trap(cpu, trap_cause, trap_tval);
        cpu.regs[0] = 0;
        return;
    }

    cpu.regs[0] = 0;
    cpu.pc = nextpc;
}

// ── Public C ABI ────────────────────────────────────────────────────

// ── Wall-clock-derived mtime ──────────────────────────────────────────
// mtime *used to* increment once per instruction, which means the guest
// kernel's notion of time tracked CPU speed: at 300 MIPS (Release) the
// clock raced 5x ahead of wall time, at 70 MIPS (Debug) ~20% fast.
// Instead we refresh mtime from a host monotonic clock at the START of
// every step_n batch, so the guest sees ticks advance at exactly
// TIMEBASE_HZ (matching the value we patch into the DTB at boot)
// regardless of how fast we actually emulate.
//
// The per-instruction `mtime++` calls are gone; the only place mtime
// changes is here. CPU code that reads cpu.mtime sees a snapshot that's
// fresh-at-batch-start — interrupt latency is bounded by the batch size
// (small enough that timers fire within a frame's worth of wallclock).

static constexpr uint64_t TIMEBASE_HZ = 60'000'000ULL;   // matches Linux/Program.cs DTB patch

#ifdef _WIN32
  #include <windows.h>
  static LARGE_INTEGER s_qpc_epoch, s_qpc_freq;
  static inline uint64_t wallclock_ticks() {
      LARGE_INTEGER now;
      QueryPerformanceCounter(&now);
      // (now - epoch) * TIMEBASE_HZ / qpc_freq, done with 128-bit safe math.
      uint64_t d = (uint64_t)(now.QuadPart - s_qpc_epoch.QuadPart);
      return (d / s_qpc_freq.QuadPart) * TIMEBASE_HZ
           + (d % s_qpc_freq.QuadPart) * TIMEBASE_HZ / s_qpc_freq.QuadPart;
  }
  static inline void wallclock_reset() {
      QueryPerformanceFrequency(&s_qpc_freq);
      QueryPerformanceCounter(&s_qpc_epoch);
  }
#else
  #include <time.h>
  static struct timespec s_epoch;
  static inline uint64_t wallclock_ticks() {
      struct timespec now;
      clock_gettime(CLOCK_MONOTONIC, &now);
      uint64_t s  = (uint64_t)(now.tv_sec  - s_epoch.tv_sec);
      int64_t  ns = (int64_t) (now.tv_nsec - s_epoch.tv_nsec);
      return s * TIMEBASE_HZ + (uint64_t)(ns * (int64_t)TIMEBASE_HZ / 1'000'000'000LL);
  }
  static inline void wallclock_reset() { clock_gettime(CLOCK_MONOTONIC, &s_epoch); }
#endif

extern "C" int rv32i_step_n(int n) {
    cpu.mtime = wallclock_ticks();
    for (int i = 0; i < n; i++) {
        do_step(cpu);
        if (__builtin_expect(cpu.halted, 0)) return -(i + 1);
    }
    return n;
}

extern "C" void rv32i_init(uint8_t* mem, uint32_t entry) {
    cpu           = {};
    cpu.pc        = entry;
    cpu.mtimecmp  = ~0ULL;
    cpu.mem       = mem;
    cpu.priv_mode = 3;          // start in M-mode
    wallclock_reset();
    cpu.mtime = 0;
}

extern "C" void rv32i_destroy() { cpu.mem = nullptr; }

extern "C" uint32_t rv32i_get_pc()                 { return cpu.pc; }
extern "C" int      rv32i_is_halted()              { return cpu.halted; }
// CLINT MMIO reads — return the LIVE wall-clock value, not the batch-snapshot
// stored in cpu.mtime. The kernel reads CLINT in a tight loop while a WFI is
// waiting for mtime ≥ mtimecmp; if we returned the cached value the kernel
// would spin a whole batch worth of instructions before noticing the time
// advanced.
extern "C" uint32_t rv32i_get_mtime_lo()           { uint64_t t = wallclock_ticks(); return (uint32_t) t; }
extern "C" uint32_t rv32i_get_mtime_hi()           { uint64_t t = wallclock_ticks(); return (uint32_t)(t >> 32); }
extern "C" uint32_t rv32i_get_priv_mode()          { return cpu.priv_mode; }

extern "C" uint64_t rv32i_get_mtime()              { return wallclock_ticks(); }
extern "C" void     rv32i_set_mtime(uint64_t v)    { cpu.mtime = v; /* guest writes are advisory; epoch stays */ }
extern "C" uint64_t rv32i_get_mtimecmp()           { return cpu.mtimecmp; }
extern "C" void     rv32i_set_mtimecmp(uint64_t v) { cpu.mtimecmp = v; }

extern "C" void rv32i_set_reg(int i, uint32_t v)   { if (i) cpu.regs[i & 31] = v; }
extern "C" void rv32i_set_halted(int v)            { cpu.halted = v; }

// External interrupt injection from the host (PLIC peripheral routes through here).
// MEIP = bit 11 in mip → M-mode external IRQ. SEIP = bit 9 → S-mode external IRQ.
// check_interrupts() already walks these in its priority array; once the bit is set,
// the trap path takes over on the next do_step().
extern "C" void rv32i_set_meip(int level) {
    if (level) cpu.csr_mip |=  (1u << 11);
    else       cpu.csr_mip &= ~(1u << 11);
}
extern "C" void rv32i_set_seip(int level) {
    if (level) cpu.csr_mip |=  (1u << 9);
    else       cpu.csr_mip &= ~(1u << 9);
}

int __stdcall DllMain(void*, unsigned int, void*) { return 1; }
