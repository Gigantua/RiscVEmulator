// rv32i_core.cpp — Native RV32I core. Windows only (ClangCL).
//
// This file is two independent hardware blocks:
//
//   PART 1  BASE RV32I CPU — a pure integer datapath. It executes the 40
//           base instructions and knows nothing else: no traps, no
//           privilege modes, no interrupts. FENCE retires as NOP. Any
//           instruction it cannot execute — a SYSTEM instruction, or an
//           encoding outside RV32I — is handed back to the caller as a
//           CpuException. The CPU never acts on it and never advances past
//           it. cpu_step() is the whole of the CPU.
//
//   PART 2  TRAP UNIT — separate hardware wrapped around the CPU. It owns
//           privilege mode (M/U), the host interrupt pins, and the
//           trap-frame page, and turns CPU exceptions and interrupt pins
//           into trap-frame context switches. Nothing in PART 1 references
//           anything in PART 2.
//
// The trap unit keeps two registers of state — current privilege and the
// host interrupt-pin latch. Everything else (interrupt-enable flag,
// per-source mask, handler vector, saved context) lives in the trap-frame
// page at guest-physical 0x0F000000 (TrapFrameDevice). See CLAUDE.md.
//
//   +0x000  IE_FLAG      global interrupt enable (bit 3)
//   +0x004  TRAP_VECTOR  handler entry PC
//   +0x008  IE_MASK      per-source enable (bit 7 = timer, bit 11 = external)
//   +0x00C  TRAP_SCRATCH scratch register (trap entry swaps it with tp)
//   +0x100  trap frame   word[0]=epc  word[1..31]=x1..x31  word[32]=status
//                        word[33]=tval  word[34]=cause
//
// Memory: every access is one dereference into a host base buffer,
// *(volatile T*)(cpu.mem + addr). The host commits guarded peripheral pages
// PAGE_NOACCESS and dispatches the resulting AVs from a VEH (see Emulator.cs).

#include <cstdint>

// Our own memset — we link -nodefaultlib, so no CRT.
extern "C" void* memset(void* dst, int c, unsigned long long n) {
    auto* d = (unsigned char*)dst;
    for (unsigned long long i = 0; i < n; i++) d[i] = (unsigned char)c;
    return dst;
}

// ════════════════════════════════════════════════════════════════════
// PART 1 — BASE RV32I CPU
//
// Pure integer datapath. No traps, no privilege, no interrupts. Whatever it
// cannot execute it reports as a CpuException, leaving pc on the offending
// instruction so the surrounding hardware can act on it.
// ════════════════════════════════════════════════════════════════════

struct CPU_State {
    uint32_t regs[32];   // 0
    uint32_t pc;         // 128
    int32_t  budget;     // 132 — JIT-only: instructions left in this run
    int      halted;     // 136 — run/stop bit driven by the host
    uint8_t* mem;        // 144 — host base; guest access = *(mem + addr)
};

// What cpu_step hands back. EXC_NONE: instruction retired. EXC_ILLEGAL: the
// encoding is not RV32I. EXC_SYSTEM: a SYSTEM-opcode instruction, which the
// integer datapath does not execute — it belongs to the environment.
enum : uint32_t { EXC_NONE = 0, EXC_ILLEGAL = 1, EXC_SYSTEM = 2 };
struct CpuException { uint32_t kind, instr; };

// ── Memory access ────────────────────────────────────────────────────
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

// Execute one base RV32I instruction. On success advances pc and returns
// {EXC_NONE}. On a SYSTEM opcode or a non-RV32I encoding it returns the
// exception and leaves pc on the offending instruction.
static CpuException cpu_step(CPU_State& cpu) {
    const uint32_t instr = mem_read<uint32_t>(cpu, cpu.pc);
    const int      rd    = (instr >>  7) & 0x1F;
    const uint32_t f3    = (instr >> 12) & 0x7;
    const uint32_t f7    = (instr >> 25) & 0x7F;
    const uint32_t u1    = cpu.regs[(instr >> 15) & 0x1F];   // rs1
    const uint32_t u2    = cpu.regs[(instr >> 20) & 0x1F];   // rs2
    const int32_t  s1    = (int32_t)u1;
    const int32_t  s2    = (int32_t)u2;
    uint32_t nextpc      = cpu.pc + 4;

    switch (instr & 0x7F) {

    case 0x37: cpu.regs[rd] = instr & 0xFFFFF000u;                        break;  // LUI
    case 0x17: cpu.regs[rd] = cpu.pc + (instr & 0xFFFFF000u);             break;  // AUIPC
    case 0x6F: cpu.regs[rd] = cpu.pc + 4; nextpc = cpu.pc + j_imm(instr); break;  // JAL
    case 0x67: { uint32_t t = (uint32_t)(s1 + i_imm(instr)) & ~1u;               // JALR
                 cpu.regs[rd] = cpu.pc + 4; nextpc = t;                   break; }

    case 0x63: {                                                                 // BRANCH
        int taken = 0;
        switch (f3) {
            case 0: taken = u1 == u2; break;  case 1: taken = u1 != u2; break;
            case 4: taken = s1 <  s2; break;  case 5: taken = s1 >= s2; break;
            case 6: taken = u1 <  u2; break;  case 7: taken = u1 >= u2; break;
        }
        if (taken) nextpc = cpu.pc + b_imm(instr);
        break;
    }

    case 0x03: {                                                                 // LOAD
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

    case 0x23: {                                                                 // STORE
        uint32_t addr = (uint32_t)(s1 + s_imm(instr));
        switch (f3) {
            case 0: mem_write<uint8_t> (cpu, addr, (uint8_t) u2); break;
            case 1: mem_write<uint16_t>(cpu, addr, (uint16_t)u2); break;
            case 2: mem_write<uint32_t>(cpu, addr,           u2); break;
        }
        break;
    }

    case 0x13: {                                                                 // OP-IMM
        const int32_t imm = i_imm(instr);
        const int     sh  = (instr >> 20) & 0x1F;
        // Only SLLI/SRLI/SRAI (f3 1/5) carry a funct7; reserved values illegal.
        if ((f3 == 1 && f7 != 0x00) || (f3 == 5 && f7 != 0x00 && f7 != 0x20))
            return { EXC_ILLEGAL, instr };
        uint32_t r = 0;
        switch (f3) {
            case 0: r = (uint32_t)(s1 + imm);                         break;  // ADDI
            case 1: r = u1 << sh;                                     break;  // SLLI
            case 2: r = s1 < imm           ? 1u : 0u;                 break;  // SLTI
            case 3: r = u1 < (uint32_t)imm ? 1u : 0u;                 break;  // SLTIU
            case 4: r = u1 ^ (uint32_t)imm;                           break;  // XORI
            case 5: r = f7 == 0x20 ? (uint32_t)(s1 >> sh) : u1 >> sh; break;  // SRAI/SRLI
            case 6: r = u1 | (uint32_t)imm;                           break;  // ORI
            case 7: r = u1 & (uint32_t)imm;                           break;  // ANDI
        }
        cpu.regs[rd] = r;
        break;
    }

    case 0x33: {                                                                 // OP
        // funct7 must be 0x00, or 0x20 for SUB/SRA (f3 0/5); all else illegal.
        if (f7 != 0x00 && !(f7 == 0x20 && (f3 == 0 || f3 == 5)))
            return { EXC_ILLEGAL, instr };
        const int sh = s2 & 0x1F;
        uint32_t r = 0;
        switch (f3) {
            case 0: r = f7 == 0x20 ? (uint32_t)(s1 - s2) : (uint32_t)(s1 + s2); break;  // SUB/ADD
            case 1: r = u1 << sh;                                              break;  // SLL
            case 2: r = s1 < s2 ? 1u : 0u;                                     break;  // SLT
            case 3: r = u1 < u2 ? 1u : 0u;                                     break;  // SLTU
            case 4: r = u1 ^ u2;                                               break;  // XOR
            case 5: r = f7 == 0x20 ? (uint32_t)(s1 >> sh) : u1 >> sh;          break;  // SRA/SRL
            case 6: r = u1 | u2;                                               break;  // OR
            case 7: r = u1 & u2;                                               break;  // AND
        }
        cpu.regs[rd] = r;
        break;
    }

    case 0x0F: break;                                                            // FENCE → NOP (single-hart)

    case 0x73: return { EXC_SYSTEM,  instr };   // SYSTEM — belongs to the environment
    default:   return { EXC_ILLEGAL, instr };   // not an RV32I encoding
    }

    cpu.regs[0] = 0;       // x0 is hardwired to zero
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

// Status-word bit fields: interrupt enable, its saved copy, saved privilege.
// Bit positions are fixed trap-frame ABI — the guest reads/writes them too.
static constexpr uint32_t STATUS_IE = 1u<<3, STATUS_PIE = 1u<<7, STATUS_PP = 3u<<11;

// Trap cause codes; host interrupt-pin bits.
static constexpr uint32_t CAUSE_ILLEGAL = 2u, CAUSE_EBREAK = 3u, CAUSE_ECALL_U = 8u, CAUSE_ECALL_M = 11u;
static constexpr uint32_t CAUSE_IRQ_MTIP = 0x80000007u, CAUSE_IRQ_MEIP = 0x8000000Bu;
static constexpr uint32_t PIN_MTIP = 1u<<7, PIN_MEIP = 1u<<11;

// Trap-frame page (TrapFrameDevice): four control words + the trap frame.
static constexpr uint32_t TRAP_PAGE    = 0x0F000000u;
static constexpr uint32_t IE_FLAG      = TRAP_PAGE + 0x000u;
static constexpr uint32_t TRAP_VECTOR  = TRAP_PAGE + 0x004u;
static constexpr uint32_t IE_MASK      = TRAP_PAGE + 0x008u;
static constexpr uint32_t TRAP_SCRATCH = TRAP_PAGE + 0x00Cu;
static constexpr uint32_t FRAME_BASE   = TRAP_PAGE + 0x100u;   // word[0] = epc
static constexpr uint32_t FRAME_STATUS = FRAME_BASE + 32u*4u;
static constexpr uint32_t FRAME_TVAL   = FRAME_BASE + 33u*4u;
static constexpr uint32_t FRAME_CAUSE  = FRAME_BASE + 34u*4u;

// Trap-return resume gateway. The base ISA has no MRET — a handler returns
// from a trap by putting its private frame pointer in a0 and jumping to this
// fixed PC. The step driver detects the fetch address and applies trap-return
// semantics; no privileged instruction is ever decoded.
static constexpr uint32_t PV_RESUME_GATEWAY = 0xFFFF0004u;

// Trap entry: spill the integer file + cause to the trap frame, disable
// interrupts, enter M-mode, vector to the guest handler.
static void do_trap(CPU_State& cpu, TrapUnit& trap, uint32_t cause, uint32_t tval) {
    // Swap tp (x4) with TRAP_SCRATCH so a U-mode handler gets a kernel tp.
    uint32_t tp = cpu.regs[4];
    cpu.regs[4] = mem_read<uint32_t>(cpu, TRAP_SCRATCH);
    mem_write<uint32_t>(cpu, TRAP_SCRATCH, tp);

    mem_write<uint32_t>(cpu, FRAME_BASE, cpu.pc);          // word[0] = epc
    for (uint32_t i = 1; i < 32; i++)
        mem_write<uint32_t>(cpu, FRAME_BASE + i * 4u, cpu.regs[i]);

    // Saved status word: previous-IE <- IE, previous-priv <- priv, IE <- 0.
    uint32_t pie = (mem_read<uint32_t>(cpu, IE_FLAG) & STATUS_IE) ? STATUS_PIE : 0u;
    uint32_t pp  = (trap.priv == PRIV_M) ? STATUS_PP : 0u;
    mem_write<uint32_t>(cpu, FRAME_STATUS, pie | pp);
    mem_write<uint32_t>(cpu, FRAME_TVAL,  tval);
    mem_write<uint32_t>(cpu, FRAME_CAUSE, cause);

    mem_write<uint32_t>(cpu, IE_FLAG, 0u);
    trap.priv = PRIV_M;
    cpu.pc = mem_read<uint32_t>(cpu, TRAP_VECTOR);
}

// Trap return (the PV_RESUME_GATEWAY): reload x1..x31 from the handler's
// private frame (pointed to by a0), restore IE/priv from its saved status
// word, resume at the saved epc.
static void trap_return(CPU_State& cpu, TrapUnit& trap) {
    uint32_t fb     = cpu.regs[10];
    uint32_t status = mem_read<uint32_t>(cpu, fb + FRAME_STATUS - FRAME_BASE);
    for (uint32_t i = 1; i < 32; i++)
        cpu.regs[i] = mem_read<uint32_t>(cpu, fb + i * 4u);
    mem_write<uint32_t>(cpu, IE_FLAG, (status & STATUS_PIE) ? STATUS_IE : 0u);
    trap.priv = (status & STATUS_PP) ? PRIV_M : PRIV_U;
    cpu.pc = mem_read<uint32_t>(cpu, fb);                  // word[0] = epc
}

// Act on a SYSTEM instruction the CPU handed back. The base ISA has only
// ECALL and EBREAK; every other SYSTEM encoding — MRET, SRET, WFI, all CSR
// ops — is illegal. Trap return is the PV_RESUME_GATEWAY, not an instruction.
static void trap_system(CPU_State& cpu, TrapUnit& trap, uint32_t instr) {
    uint32_t f3 = (instr >> 12) & 0x7;
    uint32_t fn = (instr >> 20) & 0xFFF;
    if      (f3 == 0 && fn == 0x000) do_trap(cpu, trap,                            // ECALL
                 trap.priv == PRIV_M ? CAUSE_ECALL_M : CAUSE_ECALL_U, 0);
    else if (f3 == 0 && fn == 0x001) do_trap(cpu, trap, CAUSE_EBREAK, cpu.pc);     // EBREAK
    else                             do_trap(cpu, trap, CAUSE_ILLEGAL, instr);    // illegal
}

// Sample the host interrupt pins. If one is enabled and pending, take the
// trap and return true; the step driver then skips the CPU this step.
static bool check_interrupts(CPU_State& cpu, TrapUnit& trap) {
    if (!trap.pending) return false;   // no pin asserted — skip the trap-page reads
    uint32_t pend = trap.pending & mem_read<uint32_t>(cpu, IE_MASK);
    if (!pend || !(mem_read<uint32_t>(cpu, IE_FLAG) & STATUS_IE)) return false;

    // External (MEIP) outranks timer (MTIP).
    if (pend & PIN_MEIP) { do_trap(cpu, trap, CAUSE_IRQ_MEIP, 0); return true; }
    if (pend & PIN_MTIP) { do_trap(cpu, trap, CAUSE_IRQ_MTIP, 0); return true; }
    return false;
}

// ════════════════════════════════════════════════════════════════════
// PART 3 — step driver and public C ABI
// ════════════════════════════════════════════════════════════════════

static CPU_State cpu;
static TrapUnit  trap;

// ────────────────────────────────────────────────────────────────────
// JIT store helpers. The JIT emits a `call` into one of these instead of
// inlining the store, so the actual `mov [base], reg` ends up in this DLL's
// .text section. That matters because VEH-mediated MMIO dispatch from
// stores inside our RWX arena hangs in some host processes (Mp4Player /
// TinyCC / DOOM), while VEH dispatch from stores in DLL .text works.
// ────────────────────────────────────────────────────────────────────
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

// One emulation step: the trap unit samples interrupts; otherwise the CPU
// runs one instruction and the trap unit handles anything it raised.
static void do_step() {
    if (check_interrupts(cpu, trap)) return;

    // Resume gateway: a handler returns from a trap by jumping here with a0
    // pointing at its private frame. The base ISA has no MRET to decode.
    if (__builtin_expect(cpu.pc == PV_RESUME_GATEWAY, 0)) {
        trap_return(cpu, trap);
        return;
    }

    CpuException e = cpu_step(cpu);
    if (e.kind == EXC_SYSTEM)       trap_system(cpu, trap, e.instr);
    else if (e.kind == EXC_ILLEGAL) do_trap(cpu, trap, CAUSE_ILLEGAL, e.instr);
}

// x86-64 JIT — translated-block fast path. Included here so it can see the
// CPU state, the trap unit and cpu_step(); falls back to the interpreter.
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

    // JIT on unless RVEMU_JIT=0. A fresh program means a fresh memory base,
    // so discard any blocks translated for a previous run.
    char buf[8] = {};
    unsigned long got = GetEnvironmentVariableA("RVEMU_JIT", buf, sizeof(buf));
    bool want_jit = !(got == 1 && buf[0] == '0');

    g_use_jit = want_jit && jit_alloc();
    if (g_use_jit) jit_flush();
}

extern "C" void rv32i_destroy() { cpu.mem = nullptr; }

extern "C" uint32_t rv32i_get_pc()                   { return cpu.pc; }
extern "C" int      rv32i_is_halted()                { return cpu.halted; }
extern "C" void     rv32i_set_reg(int i, uint32_t v) { if (i) cpu.regs[i & 31] = v; }
extern "C" void     rv32i_set_halted(int v) {
    cpu.halted = v;
    // Force the JIT's next per-block budget check (sub/js) to exit to C so
    // chained execution doesn't keep running after a halt MMIO write.
    if (v) cpu.budget = -1;
}

// Host interrupt pins (trap unit) — PLIC drives external, CLINT drives timer.
extern "C" void rv32i_set_meip(int level) {
    if (level) trap.pending |= PIN_MEIP; else trap.pending &= ~PIN_MEIP;
}
extern "C" void rv32i_set_mtip(int level) {
    if (level) trap.pending |= PIN_MTIP; else trap.pending &= ~PIN_MTIP;
}

int __stdcall DllMain(void*, unsigned int, void*) { return 1; }
