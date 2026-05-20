// rv32i_core.cpp — Native RV32I core. Windows only (ClangCL).
//
// Four blocks, each independent of the next:
//
//   PART 1  REAL CORE (6 base ops). Modeled on rv6.md / condensed_rv32i.md:
//             ADD, XOR, ROL, BNE, LW, SW.
//           Every other RV32I primitive (NOT, NEG, SUB, AND, OR, SLL, SRL,
//           SRA, SLT, SLTU, sign-extending sub-word loads) is a macro that
//           composes only those 6 ops. The split is enforced by file layout:
//           the BASE block sits above a divider; everything below the
//           divider only calls upward into BASE. LW/SW carry a width
//           parameter so MMIO peripherals see byte/half accesses at their
//           true width (UART RX FIFO etc).
//
//   PART 2  RV32I DECODER. cpu_step decodes one instruction and dispatches
//           to a RC6 macro. SYSTEM opcodes and unsupported encodings
//           are handed back to the caller as a CpuException.
//
//   PART 3  TRAP UNIT. Owns privilege mode (M/U) and the host interrupt
//           pins, turns CPU exceptions / pins into trap-frame context
//           switches via the trap-frame page (TrapFrameDevice).
//
//   PART 4  Step driver + public C ABI.

#include <cstdint>

// One Win32 API: env var lookup for RVEMU_RC6_STRICT. Hand-declared to keep
// -nodefaultlib in effect (no <windows.h>).
extern "C" __declspec(dllimport) unsigned long __stdcall
    GetEnvironmentVariableA(const char* name, char* buf, unsigned long size);

extern "C" void* memset(void* dst, int c, unsigned long long n) {
    auto* d = (unsigned char*)dst;
    for (unsigned long long i = 0; i < n; i++) d[i] = (unsigned char)c;
    return dst;
}

enum : uint32_t { EXC_NONE = 0, EXC_ILLEGAL = 1, EXC_SYSTEM = 2 };
struct CpuException { uint32_t kind, instr; };

// ════════════════════════════════════════════════════════════════════
// PART 1 — REAL CORE
//
// The core owns its architectural state (regs, pc, halted, mem) directly;
// six base ops + a set of macros built only on those six. The decoder in
// PART 2 is a method on this struct; the trap unit (PART 3) takes the core
// by reference. Nothing else needs to know what's inside.
// ════════════════════════════════════════════════════════════════════

struct RC6 {
    // Architectural state.
    uint32_t regs[32];               // regs[0] hardwired sink
    uint32_t pc;
    int      halted;                 // run/stop bit driven by the host
    uint8_t* mem;                    // host base; guest access = *(mem + addr)

    mutable uint32_t scratch[4];     // microcode scratch

    // ════════════════════════════════════════════════════════════════
    // BASE — the 6 ops the synthesized core actually implements.
    // ════════════════════════════════════════════════════════════════

    static uint32_t ADD(uint32_t a, uint32_t b)  { return a + b; }
    static uint32_t XOR(uint32_t a, uint32_t b)  { return a ^ b; }
    static uint32_t ROL(uint32_t a, uint32_t n)  { n &= 31; return n ? (a << n) | (a >> (32 - n)) : a; }
    static bool     BNE(uint32_t a, uint32_t b)  { return a != b; }

    // LW width: 0=byte zext, 1=half zext, 2=word. The host sees the access
    // at its true width — MMIO byte/half side effects are preserved.
    uint32_t LW(uint32_t addr, int w) const {
        switch (w) {
        case 0:  return          *(volatile uint8_t* )(mem + addr);
        case 1:  return          *(volatile uint16_t*)(mem + addr);
        default: return          *(volatile uint32_t*)(mem + addr);
        }
    }
    // SW width: 0=byte, 1=half, 2=word.
    void SW(uint32_t addr, uint32_t v, int w) const {
        switch (w) {
        case 0:  *(volatile uint8_t* )(mem + addr) = (uint8_t)v;  break;
        case 1:  *(volatile uint16_t*)(mem + addr) = (uint16_t)v; break;
        default: *(volatile uint32_t*)(mem + addr) = v;           break;
        }
    }

    // ════════════════════════════════════════════════════════════════
    // MACROS — each is a chain of calls to the 6 ops above.
    // ════════════════════════════════════════════════════════════════

    uint32_t NOT(uint32_t a) const             { return XOR(a, 0xFFFFFFFFu); }
    uint32_t NEG(uint32_t a) const             { return ADD(NOT(a), 1u); }
    uint32_t SUB(uint32_t a, uint32_t b) const { return ADD(a, NEG(b)); }

    // Isolate bit i of rs into bit 0.  Identity:
    //   r = ROL(rs, 31-i) puts bit i into bit 31.
    //   ADD(r,r) drops bit 31; ROL(r,1) keeps it in bit 0.
    //   XOR exposes exactly that bit; all others cancel.
    uint32_t BIT(uint32_t rs, uint32_t i) const {
        scratch[0] = ROL(rs, SUB(31u, i));
        return XOR(ADD(scratch[0], scratch[0]), ROL(scratch[0], 1u));
    }

    // AND via (A+B) − (A^B) = 2(A&B), with bit-31 recovered separately
    // (the doubling drops the top bit).
    uint32_t AND(uint32_t a, uint32_t b) const {
        return XOR(
            ROL(SUB(ADD(a, b), XOR(a, b)), 31u),
            ROL(BIT(ADD(BIT(a, 31u), BIT(b, 31u)), 1u), 31u)
        );
    }
    uint32_t OR(uint32_t a, uint32_t b) const { return XOR(XOR(a, b), AND(a, b)); }

    // Shift-mask helpers built from ROL only.
    uint32_t SLL_mask(uint32_t n) const { return NOT(SUB(ROL(1u, n), 1u)); }
    uint32_t SRL_mask(uint32_t n) const { return ROL(SLL_mask(n), SUB(32u, n)); }

    uint32_t SLL(uint32_t a, uint32_t n) const {
        scratch[1] = AND(n, 31u);
        return AND(ROL(a, scratch[1]), SLL_mask(scratch[1]));
    }
    uint32_t SRL(uint32_t a, uint32_t n) const {
        scratch[1] = AND(n, 31u);
        return AND(ROL(a, SUB(32u, scratch[1])), SRL_mask(scratch[1]));
    }
    uint32_t SRA(uint32_t a, uint32_t n) const {
        scratch[2] = AND(n, 31u);
        return OR(SRL(a, scratch[2]),
                  AND(NEG(BIT(a, 31u)), NOT(SRL_mask(scratch[2]))));
    }

    uint32_t SLTU(uint32_t a, uint32_t b) const {
        return BIT(OR(AND(NOT(a), b), AND(NOT(XOR(a, b)), SUB(a, b))), 31u);
    }
    uint32_t SLT(uint32_t a, uint32_t b) const {
        return SLTU(XOR(a, 0x80000000u), XOR(b, 0x80000000u));
    }

    // Sign-extending sub-word loads — composed from LW+SLL+SRA.
    uint32_t LB(uint32_t addr) const { return SRA(SLL(LW(addr, 0), 24u), 24u); }
    uint32_t LH(uint32_t addr) const { return SRA(SLL(LW(addr, 1), 16u), 16u); }
};

// ════════════════════════════════════════════════════════════════════
// PART 2 — RV32I DECODER
//
// One pure decode + dispatch on the core itself. Each RV32I instruction
// maps to one or more RC6 macro calls; nothing here touches the host
// directly.
// ════════════════════════════════════════════════════════════════════

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

// Set by rv32i_init from $RVEMU_RC6_STRICT. When on, cpu_step rejects every
// instruction that is not in the RC6 accept set (the 6 base ops plus
// JAL/JALR, the small immediate forms, LW/SW widths, FENCE, ECALL/EBREAK).
// Lets us measure the gap between today's RV32I-emitting compiler and a
// future RC6-clean compiler (RC6_LLVM_PLAN.md, Phase 0).
static bool g_rc6_strict = false;

// True iff `instr` is in the RC6 accept set.
static bool is_rc6_instr(uint32_t instr) {
    uint32_t opcode = instr & 0x7F;
    uint32_t f3     = (instr >> 12) & 0x7;
    uint32_t f7     = (instr >> 25) & 0x7F;
    switch (opcode) {
    case 0x03: return f3 == 0 || f3 == 1 || f3 == 2 || f3 == 4 || f3 == 5;   // LOAD all widths
    case 0x23: return f3 == 0 || f3 == 1 || f3 == 2;                          // STORE all widths
    case 0x13:                                                                // OP-IMM: ADDI, XORI, RORI
        if (f3 == 0 || f3 == 4) return true;
        if (f3 == 5 && f7 == 0x30) return true;                               // RORI (Zbb)
        return false;
    case 0x33:                                                                // OP: ADD, XOR, ROL, ROR
        if (f3 == 0 && f7 == 0x00) return true;                               // ADD
        if (f3 == 4 && f7 == 0x00) return true;                               // XOR
        if (f3 == 1 && f7 == 0x30) return true;                               // ROL  (Zbb)
        if (f3 == 5 && f7 == 0x30) return true;                               // ROR  (Zbb)
        return false;
    case 0x63: return f3 == 1;                                                // BRANCH: BNE only
    case 0x6F: return true;                                                   // JAL
    case 0x67: return f3 == 0;                                                // JALR
    case 0x0F: return true;                                                   // FENCE
    case 0x73: return true;                                                   // SYSTEM (ECALL/EBREAK trap)
    default:   return false;                                                  // LUI(0x37), AUIPC(0x17), …
    }
}

static CpuException cpu_step(RC6& c) {
    const uint32_t pc     = c.pc;
    const uint32_t instr  = c.LW(pc, 2);
    if (g_rc6_strict && !is_rc6_instr(instr))
        return { EXC_ILLEGAL, instr };
    const uint32_t opcode =  instr        & 0x7F;
    const uint32_t rd     = (instr >>  7) & 0x1F;
    const uint32_t f3     = (instr >> 12) & 0x7;
    const uint32_t rs1    = (instr >> 15) & 0x1F;
    const uint32_t rs2i   = (instr >> 20) & 0x1F;
    const uint32_t f7     = (instr >> 25) & 0x7F;

    uint32_t next_pc = c.ADD(pc, 4u);
    auto wr = [&](uint32_t i, uint32_t v) { if (i) c.regs[i] = v; };

    switch (opcode) {

    case 0x37:                                                                       // LUI
        wr(rd, instr & 0xFFFFF000u);
        break;

    case 0x17:                                                                       // AUIPC
        wr(rd, c.ADD(pc, instr & 0xFFFFF000u));
        break;

    case 0x6F:                                                                       // JAL
        wr(rd, c.ADD(pc, 4u));
        next_pc = c.ADD(pc, j_imm(instr));
        break;

    case 0x67: {                                                                     // JALR
        if (f3 != 0) return { EXC_ILLEGAL, instr };
        uint32_t t = c.AND(c.ADD(c.regs[rs1], (uint32_t)i_imm(instr)), 0xFFFFFFFEu);
        wr(rd, c.ADD(pc, 4u));
        next_pc = t;
        break;
    }

    case 0x63: {                                                                     // BRANCH
        uint32_t a = c.regs[rs1], b = c.regs[rs2i];
        bool take;
        switch (f3) {
        case 0: take = !c.BNE(a, b);                            break;  // BEQ
        case 1: take =  c.BNE(a, b);                            break;  // BNE
        case 4: take =  c.BNE(c.SLT (a, b), c.regs[0]);         break;  // BLT
        case 5: take = !c.BNE(c.SLT (a, b), c.regs[0]);         break;  // BGE
        case 6: take =  c.BNE(c.SLTU(a, b), c.regs[0]);         break;  // BLTU
        case 7: take = !c.BNE(c.SLTU(a, b), c.regs[0]);         break;  // BGEU
        default: return { EXC_ILLEGAL, instr };
        }
        if (take) next_pc = c.ADD(pc, b_imm(instr));
        break;
    }

    case 0x03: {                                                                     // LOAD
        uint32_t addr = c.ADD(c.regs[rs1], (uint32_t)i_imm(instr));
        switch (f3) {
        case 0:  wr(rd, c.LB(addr));        break;   // LB
        case 1:  wr(rd, c.LH(addr));        break;   // LH
        case 2:  wr(rd, c.LW(addr, 2));     break;   // LW
        case 4:  wr(rd, c.LW(addr, 0));     break;   // LBU
        case 5:  wr(rd, c.LW(addr, 1));     break;   // LHU
        default: return { EXC_ILLEGAL, instr };
        }
        break;
    }

    case 0x23: {                                                                     // STORE
        uint32_t addr = c.ADD(c.regs[rs1], (uint32_t)s_imm(instr));
        switch (f3) {
        case 0:  c.SW(addr, c.regs[rs2i], 0); break;  // SB
        case 1:  c.SW(addr, c.regs[rs2i], 1); break;  // SH
        case 2:  c.SW(addr, c.regs[rs2i], 2); break;  // SW
        default: return { EXC_ILLEGAL, instr };
        }
        break;
    }

    case 0x13: {                                                                     // OP-IMM
        if ((f3 == 1 && f7 != 0x00) || (f3 == 5 && f7 != 0x00 && f7 != 0x20))
            return { EXC_ILLEGAL, instr };
        uint32_t a   = c.regs[rs1];
        uint32_t imm = (uint32_t)i_imm(instr);
        uint32_t v   = 0;
        switch (f3) {
        case 0:  v = c.ADD (a, imm);                                  break;  // ADDI
        case 1:  v = c.SLL (a, rs2i);                                 break;  // SLLI
        case 2:  v = c.SLT (a, imm);                                  break;  // SLTI
        case 3:  v = c.SLTU(a, imm);                                  break;  // SLTIU
        case 4:  v = c.XOR (a, imm);                                  break;  // XORI
        case 5:  v = (f7 == 0x20) ? c.SRA(a, rs2i) : c.SRL(a, rs2i);  break;  // SRAI/SRLI
        case 6:  v = c.OR  (a, imm);                                  break;  // ORI
        case 7:  v = c.AND (a, imm);                                  break;  // ANDI
        }
        wr(rd, v);
        break;
    }

    case 0x33: {                                                                     // OP
        if (f7 != 0x00 && !(f7 == 0x20 && (f3 == 0 || f3 == 5)))
            return { EXC_ILLEGAL, instr };
        uint32_t a  = c.regs[rs1];
        uint32_t b  = c.regs[rs2i];
        uint32_t sh = c.AND(b, 31u);
        uint32_t v  = 0;
        switch (f3) {
        case 0:  v = (f7 == 0x20) ? c.SUB(a, b) : c.ADD(a, b);  break;  // ADD/SUB
        case 1:  v = c.SLL (a, sh);                             break;  // SLL
        case 2:  v = c.SLT (a, b);                              break;  // SLT
        case 3:  v = c.SLTU(a, b);                              break;  // SLTU
        case 4:  v = c.XOR (a, b);                              break;  // XOR
        case 5:  v = (f7 == 0x20) ? c.SRA(a, sh) : c.SRL(a, sh);break;  // SRA/SRL
        case 6:  v = c.OR  (a, b);                              break;  // OR
        case 7:  v = c.AND (a, b);                              break;  // AND
        }
        wr(rd, v);
        break;
    }

    case 0x0F: break;                                                                // FENCE → NOP

    case 0x73: return { EXC_SYSTEM,  instr };
    default:   return { EXC_ILLEGAL, instr };
    }

    c.regs[0] = 0;
    c.pc      = next_pc;
    return { EXC_NONE, 0 };
}

// ════════════════════════════════════════════════════════════════════
// PART 3 — TRAP UNIT
// ════════════════════════════════════════════════════════════════════

struct TrapUnit {
    uint32_t priv;
    uint32_t pending;
};

static constexpr uint32_t PRIV_U = 0u, PRIV_M = 3u;
static constexpr uint32_t STATUS_IE = 1u<<3, STATUS_PIE = 1u<<7, STATUS_PP = 3u<<11;
static constexpr uint32_t CAUSE_ILLEGAL = 2u, CAUSE_EBREAK = 3u, CAUSE_ECALL_U = 8u, CAUSE_ECALL_M = 11u;
static constexpr uint32_t CAUSE_IRQ_MTIP = 0x80000007u, CAUSE_IRQ_MEIP = 0x8000000Bu;
static constexpr uint32_t PIN_MTIP = 1u<<7, PIN_MEIP = 1u<<11;

static constexpr uint32_t TRAP_PAGE    = 0x0F000000u;
static constexpr uint32_t IE_FLAG      = TRAP_PAGE + 0x000u;
static constexpr uint32_t TRAP_VECTOR  = TRAP_PAGE + 0x004u;
static constexpr uint32_t IE_MASK      = TRAP_PAGE + 0x008u;
static constexpr uint32_t TRAP_SCRATCH = TRAP_PAGE + 0x00Cu;
static constexpr uint32_t FRAME_BASE   = TRAP_PAGE + 0x100u;
static constexpr uint32_t FRAME_STATUS = FRAME_BASE + 32u*4u;
static constexpr uint32_t FRAME_TVAL   = FRAME_BASE + 33u*4u;
static constexpr uint32_t FRAME_CAUSE  = FRAME_BASE + 34u*4u;
static constexpr uint32_t PV_RESUME_GATEWAY = 0xFFFF0004u;

static void do_trap(RC6& c, TrapUnit& trap, uint32_t cause, uint32_t tval) {
    uint32_t tp = c.regs[4];
    c.regs[4] = c.LW(TRAP_SCRATCH, 2);
    c.SW(TRAP_SCRATCH, tp, 2);

    c.SW(FRAME_BASE, c.pc, 2);
    for (uint32_t i = 1; i < 32; i++)
        c.SW(FRAME_BASE + i * 4u, c.regs[i], 2);

    uint32_t pie = (c.LW(IE_FLAG, 2) & STATUS_IE) ? STATUS_PIE : 0u;
    uint32_t pp  = (trap.priv == PRIV_M) ? STATUS_PP : 0u;
    c.SW(FRAME_STATUS, pie | pp, 2);
    c.SW(FRAME_TVAL,   tval,     2);
    c.SW(FRAME_CAUSE,  cause,    2);

    c.SW(IE_FLAG, 0u, 2);
    trap.priv = PRIV_M;
    c.pc = c.LW(TRAP_VECTOR, 2);
}

static void trap_return(RC6& c, TrapUnit& trap) {
    uint32_t fb     = c.regs[10];
    uint32_t status = c.LW(fb + FRAME_STATUS - FRAME_BASE, 2);
    for (uint32_t i = 1; i < 32; i++)
        c.regs[i] = c.LW(fb + i * 4u, 2);
    c.SW(IE_FLAG, (status & STATUS_PIE) ? STATUS_IE : 0u, 2);
    trap.priv = (status & STATUS_PP) ? PRIV_M : PRIV_U;
    c.pc = c.LW(fb, 2);
}

static void trap_system(RC6& c, TrapUnit& trap, uint32_t instr) {
    uint32_t f3 = (instr >> 12) & 0x7;
    uint32_t fn = (instr >> 20) & 0xFFF;
    if      (f3 == 0 && fn == 0x000) do_trap(c, trap,
                 trap.priv == PRIV_M ? CAUSE_ECALL_M : CAUSE_ECALL_U, 0);
    else if (f3 == 0 && fn == 0x001) do_trap(c, trap, CAUSE_EBREAK, c.pc);
    else                             do_trap(c, trap, CAUSE_ILLEGAL, instr);
}

static bool check_interrupts(RC6& c, TrapUnit& trap) {
    if (!trap.pending) return false;
    uint32_t pend = trap.pending & c.LW(IE_MASK, 2);
    if (!pend || !(c.LW(IE_FLAG, 2) & STATUS_IE)) return false;

    if (pend & PIN_MEIP) { do_trap(c, trap, CAUSE_IRQ_MEIP, 0); return true; }
    if (pend & PIN_MTIP) { do_trap(c, trap, CAUSE_IRQ_MTIP, 0); return true; }
    return false;
}

// ════════════════════════════════════════════════════════════════════
// PART 4 — step driver and public C ABI
// ════════════════════════════════════════════════════════════════════

static RC6 core;
static TrapUnit  trap;

static void do_step() {
    if (check_interrupts(core, trap)) return;

    if (__builtin_expect(core.pc == PV_RESUME_GATEWAY, 0)) {
        trap_return(core, trap);
        return;
    }

    CpuException e = cpu_step(core);
    if (e.kind == EXC_SYSTEM)       trap_system(core, trap, e.instr);
    else if (e.kind == EXC_ILLEGAL) do_trap(core, trap, CAUSE_ILLEGAL, e.instr);
}

extern "C" int rv32i_step_n(int n) {
    for (int i = 0; i < n; i++) {
        do_step();
        if (__builtin_expect(core.halted, 0)) return -(i + 1);
    }
    return n;
}

extern "C" void rv32i_init(uint8_t* mem, uint32_t entry) {
    memset(&core, 0, sizeof(core));
    memset(&trap, 0, sizeof(trap));
    core.pc    = entry;
    core.mem   = mem;
    trap.priv  = PRIV_M;

    // RVEMU_RC6_STRICT=1 → reject every non-RC6 opcode as illegal.
    char buf[8] = {};
    unsigned long got = GetEnvironmentVariableA("RVEMU_RC6_STRICT", buf, sizeof(buf));
    g_rc6_strict = (got == 1 && buf[0] == '1');
}

extern "C" void rv32i_destroy() { core.mem = nullptr; }

extern "C" uint32_t rv32i_get_pc()                   { return core.pc; }
extern "C" int      rv32i_is_halted()                { return core.halted; }
extern "C" void     rv32i_set_reg(int i, uint32_t v) { if (i) core.regs[i & 31] = v; }
extern "C" void     rv32i_set_halted(int v)          { core.halted = v; }

extern "C" void rv32i_set_meip(int level) {
    if (level) trap.pending |= PIN_MEIP; else trap.pending &= ~PIN_MEIP;
}
extern "C" void rv32i_set_mtip(int level) {
    if (level) trap.pending |= PIN_MTIP; else trap.pending &= ~PIN_MTIP;
}

int __stdcall DllMain(void*, unsigned int, void*) { return 1; }
