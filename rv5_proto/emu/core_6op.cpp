// RV32I emulator on a 6-op core {ADD, XOR, ROL, BNE, LW, SW}.
// Microcoded implementation: micro-ISA is the 6, microcode constants
// live in K[], microcode scratch in r[]. The decoder dispatches each
// RV32I instruction to a microcode macro on the Core.
//
// This is the explicit 6-op core (vs. core.cpp which is the same shape).
// Tracks `uops` = count of base-primitive calls, exported via core_get_uops.

#include <cstdint>
#include <bit>

#if defined(_WIN32)
  #define EXPORT extern "C" __declspec(dllexport)
#else
  #define EXPORT extern "C" __attribute__((visibility("default")))
#endif

struct Core {
    uint32_t          x[32]{};        // architectural registers (x0 = 0)
    uint32_t          pc = 0;
    uint32_t*         mem = nullptr;
    mutable uint32_t  r[5]{};          // microcode scratch
    mutable uint64_t  uops = 0;        // count of base-primitive invocations

    // microcode constant ROM:
    //  K[0]=0xFFFFFFFF  K[1]=1   K[2]=31  K[3]=3
    //  K[4]=16          K[5]=24  K[6]=0xFF  K[7]=0xFFFF
    static constexpr uint32_t K[8] = {
        0xFFFFFFFFu, 1u, 31u, 3u, 16u, 24u, 0xFFu, 0xFFFFu,
    };

    // ============================================================
    // BASE: the 6 primitives — each call bumps the uop counter.
    // ============================================================
    uint32_t ADD(uint32_t a, uint32_t b) const { ++uops; return a + b; }
    uint32_t XOR(uint32_t a, uint32_t b) const { ++uops; return a ^ b; }
    uint32_t ROL(uint32_t a, uint32_t n) const { ++uops; return std::rotl(a, static_cast<int>(n & 31)); }
    bool     BNE(uint32_t a, uint32_t b) const { ++uops; return a != b; }
    uint32_t LW (uint32_t addr) const          { ++uops; return mem[addr >> 2]; }
    void     SW (uint32_t addr, uint32_t v)    { ++uops; mem[addr >> 2] = v; }

    // ============================================================
    // MACRO: chains of primitive calls
    // ============================================================

    uint32_t NOT(uint32_t a)             const { return XOR(a, K[0]); }                 // a XOR all-ones
    uint32_t NEG(uint32_t a)             const { return ADD(NOT(a), K[1]); }            // ~a + 1
    uint32_t SUB(uint32_t a, uint32_t b) const { return ADD(a, NEG(b)); }

    // derived constants (not in ROM):
    uint32_t K32()   const { return ADD(K[2], K[1]); }     // 32 = 31 + 1
    uint32_t KMSB()  const { return ROL(K[1], K[2]); }     // 0x80000000 = 1 << 31
    uint32_t KFOUR() const { return ADD(K[3], K[1]); }     // 4 = 3 + 1
    uint32_t KNOT1() const { return NOT(K[1]); }            // 0xFFFFFFFE = ~1

    // Isolate bit i of rs into bit 0.   Scratch: r[0].
    uint32_t BIT(uint32_t rs, uint32_t i) const {
        r[0] = ROL(rs, SUB(K[2], i));
        return XOR(ADD(r[0], r[0]), ROL(r[0], K[1]));
    }

    // AND via A+B = (A^B) + 2(A&B) + bit-31 recovery.
    uint32_t AND(uint32_t a, uint32_t b) const {
        return XOR(
            ROL(SUB(ADD(a, b), XOR(a, b)), K[2]),
            ROL(BIT(ADD(BIT(a, K[2]), BIT(b, K[2])), K[1]), K[2])
        );
    }
    uint32_t OR(uint32_t a, uint32_t b) const { return XOR(XOR(a, b), AND(a, b)); }

    // SLL_mask(n) = 0xFFFFFFFF << n  =  NOT(2^n - 1).
    uint32_t SLL_mask(uint32_t n) const { return NOT(SUB(ROL(K[1], n), K[1])); }
    // SRL_mask(n) = 0xFFFFFFFF >> n  =  ROL(SLL_mask(n), 32-n).
    uint32_t SRL_mask(uint32_t n) const { return ROL(SLL_mask(n), SUB(K32(), n)); }

    // Shifts.  Scratch: r[1] (SLL/SRL masked_n), r[2] (SRA masked_n).
    uint32_t SLL(uint32_t a, uint32_t n) const {
        r[1] = AND(n, K[2]);
        return AND(ROL(a, r[1]), SLL_mask(r[1]));
    }
    uint32_t SRL(uint32_t a, uint32_t n) const {
        r[1] = AND(n, K[2]);
        return AND(ROL(a, SUB(K32(), r[1])), SRL_mask(r[1]));
    }
    uint32_t SRA(uint32_t a, uint32_t n) const {
        r[2] = AND(n, K[2]);
        return OR(SRL(a, r[2]),
                  AND(NEG(BIT(a, K[2])), NOT(SRL_mask(r[2]))));
    }

    uint32_t SLTU(uint32_t a, uint32_t b) const {
        return BIT(OR(AND(NOT(a), b), AND(NOT(XOR(a, b)), SUB(a, b))), K[2]);
    }
    uint32_t SLT(uint32_t a, uint32_t b) const {
        return SLTU(XOR(a, KMSB()), XOR(b, KMSB()));
    }

    // ---- Sub-word memory helpers.  Scratch: r[3] (k), r[4] (shift). ----
    uint32_t load_bu(uint32_t addr) const {
        r[3] = AND(addr, K[3]);
        return AND(SRL(LW(SUB(addr, r[3])), SLL(r[3], K[3])), K[6]);
    }
    uint32_t load_hu(uint32_t addr) const {
        r[3] = AND(addr, K[3]);
        return AND(SRL(LW(SUB(addr, r[3])), SLL(r[3], K[3])), K[7]);
    }
    uint32_t load_b(uint32_t addr) const { return SRA(SLL(load_bu(addr), K[5]), K[5]); }
    uint32_t load_h(uint32_t addr) const { return SRA(SLL(load_hu(addr), K[4]), K[4]); }

    void store_sub(uint32_t addr, uint32_t v, uint32_t lane_mask) {
        r[3] = AND(addr, K[3]);                // k
        r[4] = SLL(r[3], K[3]);                // byte shift = 8k
        SW(SUB(addr, r[3]),
           OR(AND(LW(SUB(addr, r[3])), NOT(SLL(lane_mask, r[4]))),
              SLL(AND(v, lane_mask), r[4])));
    }
    void store_b(uint32_t addr, uint32_t v) { store_sub(addr, v, K[6]); }
    void store_h(uint32_t addr, uint32_t v) { store_sub(addr, v, K[7]); }
};

// ============================================================
// Decoder: parses RV32I encoding, dispatches to Core methods.
// ============================================================

static bool do_step(Core& c) {
    uint32_t pc   = c.pc;
    uint32_t inst = c.LW(pc);

    uint32_t opcode = inst & 0x7F;
    uint32_t rd     = (inst >> 7)  & 0x1F;
    uint32_t fn3    = (inst >> 12) & 0x7;
    uint32_t rs1    = (inst >> 15) & 0x1F;
    uint32_t rs2    = (inst >> 20) & 0x1F;
    uint32_t fn7    = (inst >> 25) & 0x7F;

    int32_t imm_i = static_cast<int32_t>(inst) >> 20;
    int32_t imm_s = (static_cast<int32_t>(inst & 0xFE000000) >> 20) | static_cast<int32_t>((inst >> 7) & 0x1F);
    int32_t imm_b = ((static_cast<int32_t>(inst) & static_cast<int32_t>(0x80000000)) >> 19)
                  | static_cast<int32_t>((inst & 0x80) << 4)
                  | static_cast<int32_t>((inst >> 20) & 0x7E0)
                  | static_cast<int32_t>((inst >> 7)  & 0x1E);
    int32_t imm_j = ((static_cast<int32_t>(inst) & static_cast<int32_t>(0x80000000)) >> 11)
                  | static_cast<int32_t>(inst & 0xFF000)
                  | static_cast<int32_t>((inst >> 9)  & 0x800)
                  | static_cast<int32_t>((inst >> 20) & 0x7FE);
    uint32_t imm_u = inst & 0xFFFFF000;

    auto wr = [&](uint32_t r, uint32_t v) { if (r) c.x[r] = v; };
    uint32_t next_pc = c.ADD(pc, c.KFOUR());

    switch (opcode) {
    case 0x37: wr(rd, imm_u); break;                                                  // LUI
    case 0x17: wr(rd, c.ADD(pc, imm_u)); break;                                       // AUIPC
    case 0x6F:                                                                         // JAL
        wr(rd, c.ADD(pc, c.KFOUR()));
        next_pc = c.ADD(pc, static_cast<uint32_t>(imm_j));
        break;
    case 0x67: {                                                                       // JALR
        uint32_t t = c.AND(c.ADD(c.x[rs1], static_cast<uint32_t>(imm_i)), c.KNOT1());
        wr(rd, c.ADD(pc, c.KFOUR()));
        next_pc = t;
        break;
    }
    case 0x63: {                                                                       // BRANCH
        uint32_t a = c.x[rs1], b = c.x[rs2];
        bool take = false;
        switch (fn3) {
        case 0: take = !c.BNE(a, b); break;
        case 1: take =  c.BNE(a, b); break;
        case 4: take =  c.BNE(c.SLT (a, b), c.x[0]); break;
        case 5: take = !c.BNE(c.SLT (a, b), c.x[0]); break;
        case 6: take =  c.BNE(c.SLTU(a, b), c.x[0]); break;
        case 7: take = !c.BNE(c.SLTU(a, b), c.x[0]); break;
        }
        if (take) next_pc = c.ADD(pc, static_cast<uint32_t>(imm_b));
        break;
    }
    case 0x03: {                                                                       // LOAD
        uint32_t addr = c.ADD(c.x[rs1], static_cast<uint32_t>(imm_i));
        switch (fn3) {
        case 0: wr(rd, c.load_b (addr)); break;
        case 1: wr(rd, c.load_h (addr)); break;
        case 2: wr(rd, c.LW     (addr)); break;
        case 4: wr(rd, c.load_bu(addr)); break;
        case 5: wr(rd, c.load_hu(addr)); break;
        }
        break;
    }
    case 0x23: {                                                                       // STORE
        uint32_t addr = c.ADD(c.x[rs1], static_cast<uint32_t>(imm_s));
        switch (fn3) {
        case 0: c.store_b(addr, c.x[rs2]); break;
        case 1: c.store_h(addr, c.x[rs2]); break;
        case 2: c.SW    (addr, c.x[rs2]); break;
        }
        break;
    }
    case 0x13: {                                                                       // OP-IMM
        uint32_t a = c.x[rs1], imm = static_cast<uint32_t>(imm_i), sh = rs2, r = 0;
        switch (fn3) {
        case 0: r = c.ADD (a, imm); break;
        case 1: r = c.SLL (a, sh);  break;
        case 2: r = c.SLT (a, imm); break;
        case 3: r = c.SLTU(a, imm); break;
        case 4: r = c.XOR (a, imm); break;
        case 5: r = (fn7 == 0x20) ? c.SRA(a, sh) : c.SRL(a, sh); break;
        case 6: r = c.OR  (a, imm); break;
        case 7: r = c.AND (a, imm); break;
        }
        wr(rd, r);
        break;
    }
    case 0x33: {                                                                       // OP
        uint32_t a = c.x[rs1], b = c.x[rs2], sh = c.AND(b, c.K[2]), r = 0;
        switch (fn3) {
        case 0: r = (fn7 == 0x20) ? c.SUB(a, b) : c.ADD(a, b); break;
        case 1: r = c.SLL (a, sh); break;
        case 2: r = c.SLT (a, b);  break;
        case 3: r = c.SLTU(a, b);  break;
        case 4: r = c.XOR (a, b);  break;
        case 5: r = (fn7 == 0x20) ? c.SRA(a, sh) : c.SRL(a, sh); break;
        case 6: r = c.OR  (a, b);  break;
        case 7: r = c.AND (a, b);  break;
        }
        wr(rd, r);
        break;
    }
    case 0x0F: break;                                                                  // FENCE → NOP
    case 0x73:                                                                         // SYSTEM
        if ((inst >> 20) == 1 && rd == 0 && rs1 == 0 && fn3 == 0) return false;        // EBREAK
        break;
    }

    c.pc = next_pc;
    return true;
}

// ============================================================
// C ABI for the host
// ============================================================

EXPORT Core* core_create(uint32_t* mem, uint32_t entry_pc) {
    Core* c = new Core{};
    c->pc = entry_pc;
    c->mem = mem;
    return c;
}
EXPORT bool     core_step    (Core* c) { return do_step(*c); }
EXPORT void     core_run     (Core* c) { while (do_step(*c)) {} }
EXPORT void     core_destroy (Core* c) { delete c; }
EXPORT uint32_t core_get_pc  (Core* c) { return c->pc; }
EXPORT uint32_t core_get_reg (Core* c, int i) { return (i >= 0 && i < 32) ? c->x[i] : 0; }
EXPORT uint64_t core_get_uops(Core* c) { return c->uops; }
