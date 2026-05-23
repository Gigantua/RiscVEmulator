// RV32I emulator — baseline reference core.
//
// ============================================================
// What this file is
// ============================================================
// The integer datapath, written directly. No microcode, no
// primitive decomposition, no uop counter games. Every RV32I
// instruction is one switch arm doing exactly what the spec says.
//
// Pure integer datapath: no traps, no privilege, no interrupts.
// Whatever the base ISA cannot execute is reported back to the
// host as a CpuException, leaving pc on the offending instruction
// so the surrounding hardware can act on it.
//
// This file is the yardstick the *-op cores are measured against.
// ============================================================

#include <cstdint>

#if defined(_WIN32)
  #define EXPORT extern "C" __declspec(dllexport)
#else
  #define EXPORT extern "C" __attribute__((visibility("default")))
#endif

#if defined(_MSC_VER)
  #define FORCEINLINE __forceinline
#else
  #define FORCEINLINE inline __attribute__((always_inline))
#endif

// What cpu_step hands back. EXC_NONE: instruction retired. EXC_ILLEGAL:
// the encoding is not RV32I. EXC_SYSTEM: a SYSTEM-opcode instruction,
// which the integer datapath does not execute — it belongs to the
// environment.
enum : uint32_t { EXC_NONE = 0, EXC_ILLEGAL = 1, EXC_SYSTEM = 2 };
struct CpuException { uint32_t kind, instr; };

struct Core {
    uint32_t  x[32]{};      // architectural registers (x0 = 0)
    uint32_t  pc      = 0;
    int32_t   budget  = 0;  // JIT-only: instructions left in this run
    int       halted  = 0;  // run/stop bit driven by the host
    uint8_t*  mem     = nullptr;
    uint64_t  uops    = 0;  // retired-instruction count (one per step)
};

// ============================================================
// Memory access — byte-addressed, host base + guest addr.
// ============================================================

template<typename T>
static FORCEINLINE T mem_read(Core& c, uint32_t addr) {
    return *(volatile T*)(c.mem + addr);
}
template<typename T>
static FORCEINLINE void mem_write(Core& c, uint32_t addr, T v) {
    *(volatile T*)(c.mem + addr) = v;
}

// ============================================================
// Immediate decoders
// ============================================================

static constexpr uint32_t j_imm(uint32_t i) {
    uint32_t v = ((i>>31)&1u)<<20 | ((i>>12)&0xFFu)<<12 | ((i>>20)&1u)<<11 | ((i>>21)&0x3FFu)<<1;
    return (v & 0x100000u) ? v | 0xFFE00000u : v;
}
static constexpr uint32_t b_imm(uint32_t i) {
    uint32_t v = ((i>>31)&1u)<<12 | ((i>>7)&1u)<<11 | ((i>>25)&0x3Fu)<<5 | ((i>>8)&0xFu)<<1;
    return (v & 0x1000u) ? v | 0xFFFFE000u : v;
}
static constexpr int32_t  i_imm(uint32_t i) { return (int32_t)i >> 20; }
static constexpr int32_t  s_imm(uint32_t i) {
    return ((int32_t)(i & 0xFE000000) >> 20) | (int32_t)((i >> 7) & 0x1Fu);
}

// ============================================================
// Decoder + executor — one base RV32I instruction per call.
//
// On success advances pc and returns {EXC_NONE}. On a SYSTEM
// opcode or a non-RV32I encoding it returns the exception and
// leaves pc on the offending instruction.
// ============================================================

static CpuException cpu_step(Core& c) {
    const uint32_t instr = mem_read<uint32_t>(c, c.pc);
    const int      rd    = (instr >>  7) & 0x1F;
    const uint32_t f3    = (instr >> 12) & 0x7;
    const uint32_t f7    = (instr >> 25) & 0x7F;
    const uint32_t u1    = c.x[(instr >> 15) & 0x1F];   // rs1
    const uint32_t u2    = c.x[(instr >> 20) & 0x1F];   // rs2
    const int32_t  s1    = (int32_t)u1;
    const int32_t  s2    = (int32_t)u2;
    uint32_t       nextpc = c.pc + 4;

    switch (instr & 0x7F) {

    case 0x37: c.x[rd] = instr & 0xFFFFF000u;                          break;  // LUI
    case 0x17: c.x[rd] = c.pc + (instr & 0xFFFFF000u);                 break;  // AUIPC
    case 0x6F: c.x[rd] = c.pc + 4; nextpc = c.pc + j_imm(instr);       break;  // JAL
    case 0x67: { uint32_t t = (uint32_t)(s1 + i_imm(instr)) & ~1u;             // JALR
                 c.x[rd] = c.pc + 4; nextpc = t;                       break; }

    case 0x63: {                                                               // BRANCH
        int taken = 0;
        switch (f3) {
            case 0: taken = u1 == u2; break;  case 1: taken = u1 != u2; break;
            case 4: taken = s1 <  s2; break;  case 5: taken = s1 >= s2; break;
            case 6: taken = u1 <  u2; break;  case 7: taken = u1 >= u2; break;
        }
        if (taken) nextpc = c.pc + b_imm(instr);
        break;
    }

    case 0x03: {                                                               // LOAD
        uint32_t addr = (uint32_t)(s1 + i_imm(instr));
        switch (f3) {
            case 0: c.x[rd] = (uint32_t)(int8_t) mem_read<uint8_t> (c, addr); break;
            case 1: c.x[rd] = (uint32_t)(int16_t)mem_read<uint16_t>(c, addr); break;
            case 2: c.x[rd] =                    mem_read<uint32_t>(c, addr); break;
            case 4: c.x[rd] =                    mem_read<uint8_t> (c, addr); break;
            case 5: c.x[rd] =                    mem_read<uint16_t>(c, addr); break;
        }
        break;
    }

    case 0x23: {                                                               // STORE
        uint32_t addr = (uint32_t)(s1 + s_imm(instr));
        switch (f3) {
            case 0: mem_write<uint8_t> (c, addr, (uint8_t) u2); break;
            case 1: mem_write<uint16_t>(c, addr, (uint16_t)u2); break;
            case 2: mem_write<uint32_t>(c, addr,           u2); break;
        }
        break;
    }

    case 0x13: {                                                               // OP-IMM
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
        c.x[rd] = r;
        break;
    }

    case 0x33: {                                                               // OP
        // funct7 must be 0x00, or 0x20 for SUB/SRA (f3 0/5); all else illegal.
        if (f7 != 0x00 && !(f7 == 0x20 && (f3 == 0 || f3 == 5)))
            return { EXC_ILLEGAL, instr };
        const int sh = s2 & 0x1F;
        uint32_t r = 0;
        switch (f3) {
            case 0: r = f7 == 0x20 ? (uint32_t)(s1 - s2) : (uint32_t)(s1 + s2); break;  // SUB/ADD
            case 1: r = u1 << sh;                                               break;  // SLL
            case 2: r = s1 < s2 ? 1u : 0u;                                      break;  // SLT
            case 3: r = u1 < u2 ? 1u : 0u;                                      break;  // SLTU
            case 4: r = u1 ^ u2;                                                break;  // XOR
            case 5: r = f7 == 0x20 ? (uint32_t)(s1 >> sh) : u1 >> sh;           break;  // SRA/SRL
            case 6: r = u1 | u2;                                                break;  // OR
            case 7: r = u1 & u2;                                                break;  // AND
        }
        c.x[rd] = r;
        break;
    }

    case 0x0F: break;                                                          // FENCE → NOP (single-hart)

    case 0x73: return { EXC_SYSTEM,  instr };   // SYSTEM — belongs to the environment
    default:   return { EXC_ILLEGAL, instr };   // not an RV32I encoding
    }

    c.x[0] = 0;          // x0 is hardwired to zero
    c.pc   = nextpc;
    return { EXC_NONE, 0 };
}

// ============================================================
// do_step — host-facing wrapper.
//
// Returns true to keep stepping, false to halt. EBREAK halts;
// any other SYSTEM or any ILLEGAL also halts (and leaves pc on
// the offending instruction so the host can inspect it).
// ============================================================

static bool do_step(Core& c) {
    const uint32_t pc_before = c.pc;
    CpuException e = cpu_step(c);
    if (e.kind == EXC_NONE) { ++c.uops; return true; }

    // EBREAK: instr == 0x00100073 (funct12=1, rs1=0, rd=0, f3=0).
    if (e.kind == EXC_SYSTEM && e.instr == 0x00100073) return false;

    // Anything else: leave pc on the trapping instruction, halt.
    c.pc = pc_before;
    return false;
}

// ============================================================
// C ABI for the host
// ============================================================

EXPORT Core* core_create(uint8_t* mem, uint32_t entry_pc) {
    Core* c = new Core{};
    c->pc  = entry_pc;
    c->mem = mem;
    return c;
}
EXPORT bool     core_step    (Core* c)        { return do_step(*c); }
EXPORT void     core_run     (Core* c)        { while (do_step(*c)) {} }
EXPORT void     core_destroy (Core* c)        { delete c; }
EXPORT uint32_t core_get_pc  (Core* c)        { return c->pc; }
EXPORT uint32_t core_get_reg (Core* c, int i) { return (i >= 0 && i < 32) ? c->x[i] : 0; }
EXPORT uint64_t core_get_uops(Core* c)        { return c->uops; }
