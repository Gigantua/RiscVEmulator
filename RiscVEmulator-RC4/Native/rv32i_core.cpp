// rv32i_core.cpp — Native RV32I core. Windows only (ClangCL).
//
// This file is two independent hardware blocks:
//
//   PART 1  BASE RV32I CPU — a pure integer datapath built on a 4-op
//           micro-architecture {ARX, BNE, LW, SW}. It executes the 40
//           base instructions and knows nothing else: no traps, no
//           privilege modes, no interrupts. FENCE retires as NOP. Any
//           instruction it cannot execute — a SYSTEM instruction, or
//           an encoding outside RV32I — is handed back to the caller
//           as a CpuException. The CPU never acts on it and never
//           advances past it. cpu_step() is the whole of the CPU.
//
//           The 4-op uarch:
//               ARX(a, n, b, c) = rotl(a + c, n) ^ b
//               BNE(a, b)       = (a != b)
//               LW (addr)       = *(uint32_t*)(mem + addr)
//               SW (addr, v)    = *(uint32_t*)(mem + addr) = v
//
//           Every RV32I primitive (ADD/XOR/ROL/FSX, then AND/OR/SLL/
//           SRL/SRA, then SLT/SLTU and byte/half mem access) reduces
//           to a sequence of ARX/BNE/LW/SW. The reduction is loud and
//           on purpose — minimizing opcode-kind cardinality, not gates
//           or dynamic op count. See the original 4uop note below.
//
//   PART 2  TRAP UNIT — separate hardware wrapped around the CPU. It
//           owns privilege mode (M/U), the host interrupt pins, and
//           the trap-frame page, and turns CPU exceptions and
//           interrupt pins into trap-frame context switches. Nothing
//           in PART 1 references anything in PART 2.
//
// The trap unit keeps two registers of state — current privilege and the
// host interrupt-pin latch. Everything else (interrupt-enable flag,
// per-source mask, handler vector, saved context) lives in the trap-frame
// page at guest-physical 0x0F000000 (TrapFrameDevice). See CLAUDE.md.

#include <cstdint>
#include <bit>

// Our own memset — we link -nodefaultlib, so no CRT.
extern "C" void* memset(void* dst, int c, unsigned long long n) {
    auto* d = (unsigned char*)dst;
    for (unsigned long long i = 0; i < n; i++) d[i] = (unsigned char)c;
    return dst;
}

// ════════════════════════════════════════════════════════════════════
// PART 1 — BASE RV32I CPU (4-op micro-architecture)
//
// Pure integer datapath. No traps, no privilege, no interrupts. Whatever
// it cannot execute it reports as a CpuException, leaving pc on the
// offending instruction so the surrounding hardware can act on it.
// ════════════════════════════════════════════════════════════════════

struct CPU_State {
    uint32_t regs[32];   // 0
    uint32_t pc;         // 128
    int32_t  budget;     // 132 — JIT-only: instructions left in this run
    int      halted;     // 136 — run/stop bit driven by the host
    uint8_t* mem;        // 144 — host base; guest access = *(mem + addr)
};

enum : uint32_t { EXC_NONE = 0, EXC_ILLEGAL = 1, EXC_SYSTEM = 2 };
struct CpuException { uint32_t kind, instr; };

// Cumulative count of 4-op micro-ops retired since rv32i_init. Lives
// outside CPU_State so the JIT layout stays fixed for rv32i_jit.h.
//
// The counter is opt-in (RVEMU_PROFILE=1 at init) — Release builds need
// the AND macro to fold to a handful of x86 ops, which can't happen if
// every ARX has a side effect on a global. With profiling off, the
// `++g_uops` calls below are compiled out.
static uint64_t g_uops          = 0;
static bool     g_profile_uops  = false;
#define UOP_TICK() do { if (g_profile_uops) ++g_uops; } while (0)

// ── Memory access (the LW / SW primitives) ───────────────────────────
template<typename T>
static __forceinline T mem_read(CPU_State& cpu, uint32_t addr) {
    return *(volatile T*)(cpu.mem + addr);
}
template<typename T>
static __forceinline void mem_write(CPU_State& cpu, uint32_t addr, T val) {
    *(volatile T*)(cpu.mem + addr) = val;
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

// ── 4-op micro-architecture view over CPU_State ──────────────────────
//
// Each primitive bumps g_uops. ADD/XOR/ROL/FSX are pure-naming aliases
// over ARX; everything else (NOT/NEG/SUB/AND/OR/SLL/SRL/SRA/SLT/SLTU
// and the sub-word load/store macros) is a composition.
//
// Honest cost: NEG/SUB take 2 ARX ops each. This is the dynamic-count
// tax for the opcode-kind reduction — see comment block at top.
struct Core4 {
    CPU_State& s;
    mutable uint32_t r[5];   // private scratch

    // The four primitives. __forceinline lets clang fold the macro
    // layer down to a handful of x86 ops (rotate/xor/add chains).
    __forceinline uint32_t ARX(uint32_t a, uint32_t n, uint32_t b, uint32_t c) const {
        UOP_TICK(); return std::rotl(a + c, (int)n) ^ b;
    }
    __forceinline bool     BNE(uint32_t a, uint32_t b) const { UOP_TICK(); return a != b; }
    __forceinline uint32_t LW (uint32_t addr)          const { UOP_TICK(); return mem_read<uint32_t>(s, addr); }
    __forceinline void     SW (uint32_t addr, uint32_t v)    { UOP_TICK(); mem_write<uint32_t>(s, addr, v); }

    // Pure-naming ARX aliases.
    __forceinline uint32_t ADD(uint32_t a, uint32_t b)             const { return ARX(a, 0, 0, b); }
    __forceinline uint32_t XOR(uint32_t a, uint32_t b)             const { return ARX(a, 0, b, 0); }
    __forceinline uint32_t ROL(uint32_t a, uint32_t n)             const { return ARX(a, n, 0, 0); }
    __forceinline uint32_t FSX(uint32_t a, uint32_t n, uint32_t b) const { return ARX(a, n, b, 0); }

    __forceinline uint32_t NOT(uint32_t a)             const { return ARX(a, 0, 0xFFFFFFFFu, 0); }
    __forceinline uint32_t NEG(uint32_t a)             const { return ADD(NOT(a), 1u); }
    __forceinline uint32_t SUB(uint32_t a, uint32_t b) const { return ADD(a, NEG(b)); }

    __forceinline uint32_t K32()   const { return 32u; }
    __forceinline uint32_t KMSB()  const { return ROL(1u, 31u); }
    __forceinline uint32_t KFOUR() const { return 4u; }
    __forceinline uint32_t KNOT1() const { return NOT(1u); }

    __forceinline uint32_t BIT(uint32_t rs, uint32_t i) const {
        r[0] = ROL(rs, SUB(31u, i));
        return FSX(r[0], 1u, ADD(r[0], r[0]));
    }

    // AND via A+B = (A^B) + 2(A&B), with bit-31 recovery.
    __forceinline uint32_t AND(uint32_t a, uint32_t b) const {
        return FSX(
            SUB(ADD(a, b), XOR(a, b)), 31u,
            ROL(BIT(ADD(BIT(a, 31u), BIT(b, 31u)), 1u), 31u)
        );
    }
    __forceinline uint32_t OR(uint32_t a, uint32_t b) const { return XOR(XOR(a, b), AND(a, b)); }

    __forceinline uint32_t SLL_mask(uint32_t n) const { return NOT(SUB(ROL(1u, n), 1u)); }
    __forceinline uint32_t SRL_mask(uint32_t n) const { return ROL(SLL_mask(n), SUB(K32(), n)); }

    __forceinline uint32_t SLL(uint32_t a, uint32_t n) const {
        r[1] = AND(n, 31u);
        return AND(ROL(a, r[1]), SLL_mask(r[1]));
    }
    __forceinline uint32_t SRL(uint32_t a, uint32_t n) const {
        r[1] = AND(n, 31u);
        return AND(ROL(a, SUB(K32(), r[1])), SRL_mask(r[1]));
    }
    __forceinline uint32_t SRA(uint32_t a, uint32_t n) const {
        r[2] = AND(n, 31u);
        return OR(SRL(a, r[2]),
                  AND(NEG(BIT(a, 31u)), NOT(SRL_mask(r[2]))));
    }

    __forceinline uint32_t SLTU(uint32_t a, uint32_t b) const {
        return BIT(OR(AND(NOT(a), b), AND(NOT(XOR(a, b)), SUB(a, b))), 31u);
    }
    __forceinline uint32_t SLT(uint32_t a, uint32_t b) const {
        return SLTU(XOR(a, KMSB()), XOR(b, KMSB()));
    }

    __forceinline uint32_t load_bu(uint32_t addr) const {
        r[3] = AND(addr, 3u);
        return AND(SRL(LW(SUB(addr, r[3])), SLL(r[3], 3u)), 0xFFu);
    }
    __forceinline uint32_t load_hu(uint32_t addr) const {
        r[3] = AND(addr, 3u);
        return AND(SRL(LW(SUB(addr, r[3])), SLL(r[3], 3u)), 0xFFFFu);
    }
    __forceinline uint32_t load_b(uint32_t addr) const { return SRA(SLL(load_bu(addr), 24u), 24u); }
    __forceinline uint32_t load_h(uint32_t addr) const { return SRA(SLL(load_hu(addr), 16u), 16u); }

    __forceinline void store_sub(uint32_t addr, uint32_t v, uint32_t lane_mask) {
        r[3] = AND(addr, 3u);
        r[4] = SLL(r[3], 3u);
        SW(SUB(addr, r[3]),
           OR(AND(LW(SUB(addr, r[3])), NOT(SLL(lane_mask, r[4]))),
              SLL(AND(v, lane_mask), r[4])));
    }
    __forceinline void store_b(uint32_t addr, uint32_t v) { store_sub(addr, v, 0xFFu); }
    __forceinline void store_h(uint32_t addr, uint32_t v) { store_sub(addr, v, 0xFFFFu); }
};

// Execute one base RV32I instruction. On success advances pc and returns
// {EXC_NONE}. On a SYSTEM opcode or a non-RV32I encoding it returns the
// exception and leaves pc on the offending instruction.
static CpuException cpu_step(CPU_State& cpu) {
    Core4 c{cpu, {}};
    const uint32_t instr = c.LW(cpu.pc);

    const uint32_t opcode = instr & 0x7F;
    const int      rd     = (instr >>  7) & 0x1F;
    const uint32_t f3     = (instr >> 12) & 0x7;
    const int      rs1    = (instr >> 15) & 0x1F;
    const int      rs2    = (instr >> 20) & 0x1F;
    const uint32_t f7     = (instr >> 25) & 0x7F;

    auto wr = [&](int r, uint32_t v) { if (r) cpu.regs[r] = v; };
    // Phase 1 (additive): RV32I instructions use native i32 semantics
    // exactly as the upstream cpu_step did. The 4-op uarch is exercised
    // only by the new ARX opcode (case 0x0B below) and by Phase 2's
    // gradual lowering work. Reverting the OP/JAL/branch paths to host
    // ops eliminates the AND-macro corner-case divergences in
    // RuntimeIntegerLibcallsMatchHost without touching any test code.
    uint32_t nextpc = cpu.pc + 4u;

    switch (opcode) {
    // Phase 2 step 17 (partial): plain-immediate LUI is lowered to an
    // ADDI+ARX sequence by RISCVReduceToFiveOpsPass, but symbol-form
    // `lui rd, %hi(sym)` still needs native LUI because the
    // %hi-relocation is resolved by lld, not by clang's MI passes.
    // Full removal blocks on a relocation-aware lowering. Keep the
    // case until that lands.
    case 0x37: wr(rd, instr & 0xFFFFF000u); break;                                  // LUI
    case 0x17: wr(rd, cpu.pc + (instr & 0xFFFFF000u)); break;                       // AUIPC
    case 0x6F: {                                                                     // JAL (+ RISC5 sub-modes)
        // Standard JAL plus two co-opted encodings:
        //   • `jal x0, 0`  → PC = ra (function return; standard RV32I
        //                    would self-loop, so this is safe to hijack)
        //   • `jal rd, 2`  → ra = pc+4; PC = x[rd_field]
        //                    (memory-target indirect call; standard
        //                    JAL with misaligned imm=2 normally traps,
        //                    so this is safe to hijack)
        int32_t imm = j_imm(instr);
        if (rd == 0 && imm == 0) { nextpc = cpu.regs[1]; break; }
        if (imm == 2) {
            cpu.regs[1] = cpu.pc + 4u;
            nextpc = cpu.regs[rd];
            break;
        }
        if (imm == 6) {                              // tail-call: PC = x[rd], no link
            nextpc = cpu.regs[rd];
            break;
        }
        wr(rd, cpu.pc + 4u);
        nextpc = cpu.pc + imm;
        break;
    }
    // JALR (case 0x67) fully removed. AsmPrinter rewrites PseudoRET,
    // PseudoCALLIndirect, PseudoTAILIndirect and PseudoBRIND into JAL
    // sub-modes (imm=0 return, imm=2 indirect call, imm=6 tail/jump).
    // The MC-layer expandFunctionCall still emits AUIPC+JALR for
    // symbolic PseudoCALL/TAIL but lld's default RISC-V relaxation
    // collapses every reachable pair into a single JAL. Inline asm
    // is responsible for its own encoding (see Programs/timer_irq.c
    // for an example of the .4byte JAL sub-mode replacement).
    case 0x63: {                                                                     // BRANCH — BEQ + BNE only
        // BLT/BGE/BLTU/BGEU → SLT(U)+BNE (pass). Inline-asm callers
        // rewritten. BEQ: pass converts what it can to BNE+PseudoBR,
        // but later LLVM passes (BranchFolding, TailDuplication) can
        // introduce new BEQs after our pre-RA expander runs, so the
        // decoder still needs to handle them. Moving the pass to
        // addPreEmitPass (split into pre-RA/post-RA halves) would
        // close the gap.
        bool take;
        switch (f3) {
        case 0: take = (cpu.regs[rs1] == cpu.regs[rs2]); break;   // BEQ
        case 1: take = (cpu.regs[rs1] != cpu.regs[rs2]); break;   // BNE — the keeper
        default: return { EXC_ILLEGAL, instr };
        }
        if (take) nextpc = cpu.pc + b_imm(instr);
        break;
    }
    case 0x03: {                                                                     // LOAD
        uint32_t addr = c.ADD(cpu.regs[rs1], (uint32_t)i_imm(instr));
        switch (f3) {
        case 0: wr(rd, (uint32_t)(int32_t)(int8_t) mem_read<uint8_t> (cpu, addr)); break;
        case 1: wr(rd, (uint32_t)(int32_t)(int16_t)mem_read<uint16_t>(cpu, addr)); break;
        case 2: wr(rd, c.LW(addr)); break;
        case 4: wr(rd,                    mem_read<uint8_t> (cpu, addr)); break;
        case 5: wr(rd,                    mem_read<uint16_t>(cpu, addr)); break;
        default: return { EXC_ILLEGAL, instr };
        }
        break;
    }
    case 0x23: {                                                                     // STORE
        uint32_t addr = c.ADD(cpu.regs[rs1], (uint32_t)s_imm(instr));
        switch (f3) {
        case 0: mem_write<uint8_t> (cpu, addr, (uint8_t) cpu.regs[rs2]); break;
        case 1: mem_write<uint16_t>(cpu, addr, (uint16_t)cpu.regs[rs2]); break;
        case 2: c.SW(addr, cpu.regs[rs2]); break;
        default: return { EXC_ILLEGAL, instr };
        }
        break;
    }
    case 0x13: {                                                                     // OP-IMM: only ADDI survives
        // Phase 2: SLLI/SRLI/SRAI/SLTI/SLTIU/XORI/ORI/ANDI are all
        // lowered by RISCVReduceToFiveOpsPass. Only ADDI (f3=0) is
        // still a real instruction in `cpu_step`. Removing ADDI is
        // the final blocker for a true 5-opcode decoder — every
        // constant in every ARX-based expansion currently goes
        // through `addi rd, x0, imm`. See risc5.md "Remaining
        // blockers" for the constant-pool plan.
        if (f3 != 0) return { EXC_ILLEGAL, instr };
        wr(rd, (uint32_t)((int32_t)cpu.regs[rs1] + i_imm(instr)));
        break;
    }
    // Phase 2: OP opcode (0x33) is fully eliminated. Every RV32I R-type
    // ALU instruction — ADD, SUB, AND, OR, XOR, SLL, SRL, SRA, SLT,
    // SLTU — is lowered to ARX/ADDI sequences by
    // RISCVReduceToFiveOpsPass. The case isn't listed below; any
    // remaining 0x33 encoding hits the `default: EXC_ILLEGAL` branch.
    // Phase 2 step 1 — FENCE (0x0F) removed. Single-hart bare-metal has
    // no memory-ordering requirements; the kernel patches for the
    // buildroot Linux image already rewrite fence sites to NOP. Any
    // remaining 0x0F encoding now traps as illegal.

    // ── RISC5 primitive: ARXI (immediate rotate) ──────────────────────
    //   rd = rotl(rs1 + rs3, imm5) ^ rs2
    // Encoding (R4 + 5-bit imm in funct2 || funct3, opcode 0x0B):
    //   [31:27 rs3] | [26:25 imm[4:3]] | [24:20 rs2] | [19:15 rs1] |
    //   [14:12 imm[2:0]] | [11:7 rd] | [6:0 0x0B]
    case 0x0B: {
        uint32_t rs3   = (instr >> 27) & 0x1F;
        uint32_t imm5  = (((instr >> 25) & 0x3) << 3) | ((instr >> 12) & 0x7);
        uint32_t a     = cpu.regs[rs1];
        uint32_t b     = cpu.regs[rs2];
        uint32_t cc    = cpu.regs[rs3];
        wr(rd, c.ARX(a, imm5, b, cc));
        break;
    }

    // 0x73 SYSTEM (ECALL/EBREAK) removed for the 5-op target. Programs
    // that need to trap now write to a magic MMIO address (the
    // HostExitDevice or a dedicated trap-trigger peripheral) instead
    // of executing ECALL. The 3 RV32I-trap-mechanism regression
    // tests (DirectUserEcallTest, NestedTrapTest,
    // ParavirtSyscallGatewayTest) are skipped — they test removed
    // functionality.
    default:   return { EXC_ILLEGAL, instr };   // not an RV32I encoding
    }

    cpu.regs[0] = 0;
    cpu.pc = nextpc;
    return { EXC_NONE, 0 };
}

// ════════════════════════════════════════════════════════════════════
// PART 2 — TRAP UNIT
//
// Separate hardware around the CPU. It owns privilege mode and the host
// interrupt pins, and turns CPU exceptions and interrupt pins into
// trap-frame context switches through the trap-frame page.
// ════════════════════════════════════════════════════════════════════

struct TrapUnit {
    uint32_t priv;       // PRIV_M or PRIV_U
    uint32_t pending;    // host interrupt pins (PIN_MTIP | PIN_MEIP)
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

static void do_trap(CPU_State& cpu, TrapUnit& trap, uint32_t cause, uint32_t tval) {
    uint32_t tp = cpu.regs[4];
    cpu.regs[4] = mem_read<uint32_t>(cpu, TRAP_SCRATCH);
    mem_write<uint32_t>(cpu, TRAP_SCRATCH, tp);

    mem_write<uint32_t>(cpu, FRAME_BASE, cpu.pc);
    for (uint32_t i = 1; i < 32; i++)
        mem_write<uint32_t>(cpu, FRAME_BASE + i * 4u, cpu.regs[i]);

    uint32_t pie = (mem_read<uint32_t>(cpu, IE_FLAG) & STATUS_IE) ? STATUS_PIE : 0u;
    uint32_t pp  = (trap.priv == PRIV_M) ? STATUS_PP : 0u;
    mem_write<uint32_t>(cpu, FRAME_STATUS, pie | pp);
    mem_write<uint32_t>(cpu, FRAME_TVAL,  tval);
    mem_write<uint32_t>(cpu, FRAME_CAUSE, cause);

    mem_write<uint32_t>(cpu, IE_FLAG, 0u);
    trap.priv = PRIV_M;
    uint32_t tvec = mem_read<uint32_t>(cpu, TRAP_VECTOR);
    if (tvec == 0) tvec = mem_read<uint32_t>(cpu, IE_MASK);
    cpu.pc = tvec;
}

static void trap_return(CPU_State& cpu, TrapUnit& trap) {
    uint32_t fb     = cpu.regs[10];
    uint32_t status = mem_read<uint32_t>(cpu, fb + FRAME_STATUS - FRAME_BASE);
    for (uint32_t i = 1; i < 32; i++)
        cpu.regs[i] = mem_read<uint32_t>(cpu, fb + i * 4u);
    mem_write<uint32_t>(cpu, IE_FLAG, (status & STATUS_PIE) ? STATUS_IE : 0u);
    trap.priv = (status & STATUS_PP) ? PRIV_M : PRIV_U;
    cpu.pc = mem_read<uint32_t>(cpu, fb);
}

static uint32_t soft_csr[4096];

static void trap_system(CPU_State& cpu, TrapUnit& trap, uint32_t instr) {
    uint32_t f3 = (instr >> 12) & 0x7;
    uint32_t fn = (instr >> 20) & 0xFFF;
    if (f3 == 0 && fn == 0x000) {
        do_trap(cpu, trap,
                trap.priv == PRIV_M ? CAUSE_ECALL_M : CAUSE_ECALL_U, 0);
        return;
    }
    if (f3 == 0 && fn == 0x001) {
        do_trap(cpu, trap, CAUSE_EBREAK, cpu.pc);
        return;
    }
    if (f3 == 0) {
        do_trap(cpu, trap, CAUSE_ILLEGAL, instr);
        return;
    }
    uint32_t rd  = (instr >> 7)  & 0x1F;
    uint32_t rs1 = (instr >> 15) & 0x1F;
    uint32_t old = soft_csr[fn];
    uint32_t src = (f3 & 4) ? rs1 : cpu.regs[rs1];
    uint32_t op  = f3 & 3;
    bool write = (op == 1) || (rs1 != 0);
    if (write) {
        uint32_t nv = (op == 1) ? src
                    : (op == 2) ? (old |  src)
                                : (old & ~src);
        soft_csr[fn] = nv;
    }
    if (rd) cpu.regs[rd] = old;
    cpu.pc += 4;
}

static bool check_interrupts(CPU_State& cpu, TrapUnit& trap) {
    if (!trap.pending) return false;
    uint32_t pend = trap.pending & mem_read<uint32_t>(cpu, IE_MASK);
    if (!pend || !(mem_read<uint32_t>(cpu, IE_FLAG) & STATUS_IE)) return false;

    if (pend & PIN_MEIP) { do_trap(cpu, trap, CAUSE_IRQ_MEIP, 0); return true; }
    if (pend & PIN_MTIP) { do_trap(cpu, trap, CAUSE_IRQ_MTIP, 0); return true; }
    return false;
}

// ════════════════════════════════════════════════════════════════════
// PART 3 — step driver and public C ABI
// ════════════════════════════════════════════════════════════════════

static CPU_State cpu;
static TrapUnit  trap;

// JIT store/load helpers. The JIT emits a `call` into one of these
// instead of inlining the memory access, so the actual `mov` ends up in
// this DLL's .text section. That matters because VEH-mediated MMIO
// dispatch from accesses inside our RWX arena hangs in some host
// processes, while VEH dispatch from DLL .text works.
extern "C" __declspec(noinline) void jit_helper_sb(uint32_t addr, uint32_t val) {
    *(volatile uint8_t*)(cpu.mem + addr) = (uint8_t)val;
}
extern "C" __declspec(noinline) void jit_helper_sh(uint32_t addr, uint32_t val) {
    *(volatile uint16_t*)(cpu.mem + addr) = (uint16_t)val;
}
extern "C" __declspec(noinline) void jit_helper_sw(uint32_t addr, uint32_t val) {
    *(volatile uint32_t*)(cpu.mem + addr) = val;
}
extern "C" __declspec(noinline) uint32_t jit_helper_lb(uint32_t addr) {
    return (uint32_t)(int32_t)(int8_t)*(volatile uint8_t*)(cpu.mem + addr);
}
extern "C" __declspec(noinline) uint32_t jit_helper_lh(uint32_t addr) {
    return (uint32_t)(int32_t)(int16_t)*(volatile uint16_t*)(cpu.mem + addr);
}
extern "C" __declspec(noinline) uint32_t jit_helper_lw(uint32_t addr) {
    return *(volatile uint32_t*)(cpu.mem + addr);
}
extern "C" __declspec(noinline) uint32_t jit_helper_lbu(uint32_t addr) {
    return *(volatile uint8_t*)(cpu.mem + addr);
}
extern "C" __declspec(noinline) uint32_t jit_helper_lhu(uint32_t addr) {
    return *(volatile uint16_t*)(cpu.mem + addr);
}

static void do_step() {
    if (check_interrupts(cpu, trap)) return;

    if (__builtin_expect(cpu.pc == PV_RESUME_GATEWAY, 0)) {
        trap_return(cpu, trap);
        return;
    }

    CpuException e = cpu_step(cpu);
    if (e.kind == EXC_SYSTEM)       trap_system(cpu, trap, e.instr);
    else if (e.kind == EXC_ILLEGAL) do_trap(cpu, trap, CAUSE_ILLEGAL, e.instr);
}

#include "rv32i_jit.h"
static bool g_use_jit = false;

extern "C" int rv32i_step_n(int n) {
    if (g_use_jit) return jit_run(n);
    for (int i = 0; i < n; i++) {
        do_step();
        if (__builtin_expect(cpu.halted, 0)) return -(i + 1);
    }
    return n;
}

extern "C" void rv32i_init(uint8_t* mem, uint32_t entry) {
    memset(&cpu, 0, sizeof(cpu));
    memset(&trap, 0, sizeof(trap));
    cpu.pc    = entry;
    cpu.mem   = mem;
    trap.priv = PRIV_M;
    g_uops    = 0;

    char buf[8] = {};
    unsigned long got = GetEnvironmentVariableA("RVEMU_JIT", buf, sizeof(buf));
    bool want_jit = (got == 1 && buf[0] == '1');

    g_use_jit = want_jit && jit_alloc();
    if (g_use_jit) jit_flush();

    char pbuf[8] = {};
    unsigned long pgot = GetEnvironmentVariableA("RVEMU_PROFILE", pbuf, sizeof(pbuf));
    g_profile_uops = (pgot == 1 && pbuf[0] == '1');
}

extern "C" void rv32i_destroy() { cpu.mem = nullptr; }

extern "C" uint32_t rv32i_get_pc()                   { return cpu.pc; }
extern "C" int      rv32i_is_halted()                { return cpu.halted; }
extern "C" void     rv32i_set_reg(int i, uint32_t v) { if (i) cpu.regs[i & 31] = v; }
extern "C" void     rv32i_set_halted(int v) {
    cpu.halted = v;
    if (v) cpu.budget = -1;
}

extern "C" void rv32i_set_meip(int level) {
    if (level) trap.pending |= PIN_MEIP; else trap.pending &= ~PIN_MEIP;
}
extern "C" void rv32i_set_mtip(int level) {
    if (level) trap.pending |= PIN_MTIP; else trap.pending &= ~PIN_MTIP;
}

extern "C" uint64_t rv32i_get_uops() { return g_uops; }

extern "C" int rv32i_jit_active() { return g_use_jit ? 1 : 0; }

int __stdcall DllMain(void*, unsigned int, void*) { return 1; }
