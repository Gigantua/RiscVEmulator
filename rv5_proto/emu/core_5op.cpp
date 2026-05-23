// RV32I emulator on a 5-op core {ADD, FSX, BNE, LW, SW}.
//
// FSX(a, n, b) = rotl(a, n) ^ b   is the fused XOR+ROL primitive.
// Plain XOR and ROL fall out as special cases:
//     XOR(a, b) = FSX(a, 0, b)
//     ROL(a, n) = FSX(a, n, 0)
// The recurring XOR(ROL(x, n), y) pattern in BIT/AND/SRA collapses from
// two primitive calls into one FSX.
//
// Capability classes (each provably the only representative of its class —
// the 5 are irreducible by deletion):
//     ADD : integer-carry         FSX : bit-rotation + bitwise-logic
//     BNE : control               LW  : memory-read        SW : memory-write

#include <cstdint>
#include <bit>

#if defined(_WIN32)
  #define EXPORT extern "C" __declspec(dllexport)
#else
  #define EXPORT extern "C" __attribute__((visibility("default")))
#endif

struct Core {
    uint32_t          x[32]{};
    uint32_t          pc = 0;
    uint32_t*         mem = nullptr;
    mutable uint32_t  r[5]{};
    mutable uint64_t  uops = 0;

    static constexpr uint32_t K[8] = {
        0xFFFFFFFFu, 1u, 31u, 3u, 16u, 24u, 0xFFu, 0xFFFFu,
    };

    // ============================================================
    // BASE: the 5 primitives — each call bumps the uop counter.
    // ============================================================
    uint32_t ADD(uint32_t a, uint32_t b)                const { ++uops; return a + b; }
    uint32_t FSX(uint32_t a, uint32_t n, uint32_t b)    const {
        ++uops; return std::rotl(a, (int)(n & 31)) ^ b;
    }
    bool     BNE(uint32_t a, uint32_t b)                const { ++uops; return a != b; }
    uint32_t LW (uint32_t addr)                         const { ++uops; return mem[addr >> 2]; }
    void     SW (uint32_t addr, uint32_t v)                   { ++uops; mem[addr >> 2] = v; }

    // ============================================================
    // Convenience aliases for the two FSX special cases. Same primitive,
    // same dynamic op — pure naming so the macros below read like the original.
    // ============================================================
    uint32_t XOR(uint32_t a, uint32_t b) const { return FSX(a, 0,    b); }
    uint32_t ROL(uint32_t a, uint32_t n) const { return FSX(a, n,    0); }

    // ============================================================
    // MACRO layer — identical algebra to the 6-op version, but every
    // XOR(ROL(x,n), y) site is rewritten as one FSX.
    // ============================================================

    uint32_t NOT(uint32_t a)             const { return FSX(a, 0, K[0]); }       // a ^ ~0
    uint32_t NEG(uint32_t a)             const { return ADD(NOT(a), K[1]); }
    uint32_t SUB(uint32_t a, uint32_t b) const { return ADD(a, NEG(b)); }

    uint32_t K32()   const { return ADD(K[2], K[1]); }
    uint32_t KMSB()  const { return ROL(K[1], K[2]); }
    uint32_t KFOUR() const { return ADD(K[3], K[1]); }
    uint32_t KNOT1() const { return NOT(K[1]); }

    // BIT: r[0] = ROL(rs, 31-i);  result = ROL(r[0],1) ^ (r[0]+r[0])
    //   ==> one FSX folds the rotate-and-xor.
    uint32_t BIT(uint32_t rs, uint32_t i) const {
        r[0] = ROL(rs, SUB(K[2], i));
        return FSX(r[0], K[1], ADD(r[0], r[0]));
    }

    // AND via A+B = (A^B) + 2(A&B), with bit-31 recovery.
    //   Original: XOR( ROL(SUB(...),31), ROL(BIT(...),31) )
    //   ==> one FSX at the top; inner ROL stays as a FSX(_,31,0).
    uint32_t AND(uint32_t a, uint32_t b) const {
        return FSX(
            SUB(ADD(a, b), XOR(a, b)), K[2],
            ROL(BIT(ADD(BIT(a, K[2]), BIT(b, K[2])), K[1]), K[2])
        );
    }
    uint32_t OR(uint32_t a, uint32_t b) const { return XOR(XOR(a, b), AND(a, b)); }

    uint32_t SLL_mask(uint32_t n) const { return NOT(SUB(ROL(K[1], n), K[1])); }
    uint32_t SRL_mask(uint32_t n) const { return ROL(SLL_mask(n), SUB(K32(), n)); }

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
        r[3] = AND(addr, K[3]);
        r[4] = SLL(r[3], K[3]);
        SW(SUB(addr, r[3]),
           OR(AND(LW(SUB(addr, r[3])), NOT(SLL(lane_mask, r[4]))),
              SLL(AND(v, lane_mask), r[4])));
    }
    void store_b(uint32_t addr, uint32_t v) { store_sub(addr, v, K[6]); }
    void store_h(uint32_t addr, uint32_t v) { store_sub(addr, v, K[7]); }
};

// ============================================================
// Decoder — unchanged from the 6-op version (the XOR/ROL it used
// are now aliases for FSX, so its dynamic op count drops everywhere
// it hits AND/BIT/SRA/etc).
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

    int32_t imm_i = (int32_t)inst >> 20;
    int32_t imm_s = ((int32_t)(inst & 0xFE000000) >> 20) | (int32_t)((inst >> 7) & 0x1F);
    int32_t imm_b = (((int32_t)inst & (int32_t)(0x80000000)) >> 19)
                  | (int32_t)((inst & 0x80) << 4)
                  | (int32_t)((inst >> 20) & 0x7E0)
                  | (int32_t)((inst >> 7)  & 0x1E);
    int32_t imm_j = (((int32_t)inst & (int32_t)(0x80000000)) >> 11)
                  | (int32_t)(inst & 0xFF000)
                  | (int32_t)((inst >> 9)  & 0x800)
                  | (int32_t)((inst >> 20) & 0x7FE);
    uint32_t imm_u = inst & 0xFFFFF000;

    auto wr = [&](uint32_t r, uint32_t v) { if (r) c.x[r] = v; };
    uint32_t next_pc = c.ADD(pc, c.KFOUR());

    switch (opcode) {
    case 0x37: wr(rd, imm_u); break;
    case 0x17: wr(rd, c.ADD(pc, imm_u)); break;
    case 0x6F:
        wr(rd, c.ADD(pc, c.KFOUR()));
        next_pc = c.ADD(pc, (uint32_t)imm_j);
        break;
    case 0x67: {
        uint32_t t = c.AND(c.ADD(c.x[rs1], (uint32_t)imm_i), c.KNOT1());
        wr(rd, c.ADD(pc, c.KFOUR()));
        next_pc = t;
        break;
    }
    case 0x63: {
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
        if (take) next_pc = c.ADD(pc, (uint32_t)imm_b);
        break;
    }
    case 0x03: {
        uint32_t addr = c.ADD(c.x[rs1], (uint32_t)imm_i);
        switch (fn3) {
        case 0: wr(rd, c.load_b (addr)); break;
        case 1: wr(rd, c.load_h (addr)); break;
        case 2: wr(rd, c.LW     (addr)); break;
        case 4: wr(rd, c.load_bu(addr)); break;
        case 5: wr(rd, c.load_hu(addr)); break;
        }
        break;
    }
    case 0x23: {
        uint32_t addr = c.ADD(c.x[rs1], (uint32_t)imm_s);
        switch (fn3) {
        case 0: c.store_b(addr, c.x[rs2]); break;
        case 1: c.store_h(addr, c.x[rs2]); break;
        case 2: c.SW    (addr, c.x[rs2]); break;
        }
        break;
    }
    case 0x13: {
        uint32_t a = c.x[rs1], imm = (uint32_t)imm_i, sh = rs2, r = 0;
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
    case 0x33: {
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
    case 0x0F: break;
    case 0x73:
        if ((inst >> 20) == 1 && rd == 0 && rs1 == 0 && fn3 == 0) return false;
        break;
    }

    c.pc = next_pc;
    return true;
}

// ============================================================
// C ABI
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
