// RV32I emulator on a 5-op core {ADD, FSX, BNE, LW, SW}.
//
// FSX(a, n, b) = rotl(a, n) ^ b   is the fused XOR+ROL primitive.
// Plain XOR and ROL fall out as special cases:
//     XOR(a, b) = FSX(a, 0, b)
//     ROL(a, n) = FSX(a, n, 0)
// The recurring XOR(ROL(x, n), y) pattern in BIT/AND/SRA collapses from
// two primitive calls into one FSX.
//
// Capability classes (irreducible by deletion):
//     ADD : integer-carry         FSX : bit-rotation + bitwise-logic
//     BNE : control               LW  : memory-read        SW : memory-write

#include <cstdint>
#include <bit>

#if defined(_WIN32)
  #define EXPORT extern "C" __declspec(dllexport)
#else
  #define EXPORT extern "C" __attribute__((visibility("default")))
#endif

class Core {
    uint32_t  x[32]{};
    uint32_t  pc = 0;
    uint32_t* mem = nullptr;
    uint32_t  r[5]{};
    uint64_t  uops = 0;

public:
    static constexpr uint32_t K[8] = {
        0xFFFFFFFFu, 1u, 31u, 3u, 16u, 24u, 0xFFu, 0xFFFFu,
    };

    void     attach   (uint32_t* m, uint32_t entry) { mem = m; pc = entry; }
    uint32_t peek_pc  ()           const { return pc; }
    uint32_t peek_reg (uint32_t i) const { return x[i & 31u]; }
    uint64_t op_count ()           const { return uops; }

    uint32_t RX (uint32_t i)             { return x[i & 31u]; }
    void     WX (uint32_t i, uint32_t v) { if (i & 31u) x[i & 31u] = v; }
    uint32_t RPC()                       { return pc; }
    void     WPC(uint32_t v)             { pc = v; }

    // ============================================================
    // BASE: the 5 primitives — each call bumps the uop counter.
    // ============================================================
    uint32_t ADD(uint32_t a, uint32_t b)             { ++uops; return a + b; }
    uint32_t FSX(uint32_t a, uint32_t n, uint32_t b) { ++uops; return std::rotl(a, (int)(n & 31)) ^ b; }
    bool     BNE(uint32_t a, uint32_t b)             { ++uops; return a != b; }
    uint32_t LW (uint32_t addr)                      { ++uops; return mem[addr >> 2]; }
    void     SW (uint32_t addr, uint32_t v)          { ++uops; mem[addr >> 2] = v; }

    // Aliases for the two FSX special cases.
    uint32_t XOR(uint32_t a, uint32_t b) { return FSX(a, 0, b); }
    uint32_t ROL(uint32_t a, uint32_t n) { return FSX(a, n, 0); }

    // ============================================================
    // MACRO layer — every XOR(ROL(x,n), y) site fuses into one FSX.
    // ============================================================
    uint32_t NOT(uint32_t a)             { return FSX(a, 0, K[0]); }
    uint32_t NEG(uint32_t a)             { return ADD(NOT(a), K[1]); }
    uint32_t SUB(uint32_t a, uint32_t b) { return ADD(a, NEG(b)); }

    // Derived constants — built from primitives, so they bump uops too.
    uint32_t K32()   { return ADD(K[2], K[1]); }
    uint32_t KMSB()  { return ROL(K[1], K[2]); }
    uint32_t KFOUR() { return ADD(K[3], K[1]); }
    uint32_t KNOT1() { return NOT(K[1]); }

    uint32_t BIT(uint32_t rs, uint32_t i) {
        r[0] = ROL(rs, SUB(K[2], i));
        return FSX(r[0], K[1], ADD(r[0], r[0]));
    }

    uint32_t AND(uint32_t a, uint32_t b) {
        return FSX(
            SUB(ADD(a, b), XOR(a, b)), K[2],
            ROL(BIT(ADD(BIT(a, K[2]), BIT(b, K[2])), K[1]), K[2])
        );
    }
    uint32_t OR(uint32_t a, uint32_t b) { return XOR(XOR(a, b), AND(a, b)); }

    uint32_t SLL_mask(uint32_t n) { return NOT(SUB(ROL(K[1], n), K[1])); }
    uint32_t SRL_mask(uint32_t n) { return ROL(SLL_mask(n), SUB(K32(), n)); }

    uint32_t SLL(uint32_t a, uint32_t n) {
        r[1] = AND(n, K[2]);
        return AND(ROL(a, r[1]), SLL_mask(r[1]));
    }
    uint32_t SRL(uint32_t a, uint32_t n) {
        r[1] = AND(n, K[2]);
        return AND(ROL(a, SUB(K32(), r[1])), SRL_mask(r[1]));
    }
    uint32_t SRA(uint32_t a, uint32_t n) {
        r[2] = AND(n, K[2]);
        return OR(SRL(a, r[2]),
                  AND(NEG(BIT(a, K[2])), NOT(SRL_mask(r[2]))));
    }

    uint32_t SLTU(uint32_t a, uint32_t b) {
        return BIT(OR(AND(NOT(a), b), AND(NOT(XOR(a, b)), SUB(a, b))), K[2]);
    }
    uint32_t SLT(uint32_t a, uint32_t b) {
        return SLTU(XOR(a, KMSB()), XOR(b, KMSB()));
    }

    uint32_t load_bu(uint32_t addr) {
        r[3] = AND(addr, K[3]);
        return AND(SRL(LW(SUB(addr, r[3])), SLL(r[3], K[3])), K[6]);
    }
    uint32_t load_hu(uint32_t addr) {
        r[3] = AND(addr, K[3]);
        return AND(SRL(LW(SUB(addr, r[3])), SLL(r[3], K[3])), K[7]);
    }
    uint32_t load_b(uint32_t addr) { return SRA(SLL(load_bu(addr), K[5]), K[5]); }
    uint32_t load_h(uint32_t addr) { return SRA(SLL(load_hu(addr), K[4]), K[4]); }

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
// Decoder
// ============================================================

static bool do_step(Core& c) {
    uint32_t pc   = c.RPC();
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

    uint32_t next_pc = c.ADD(pc, c.KFOUR());

    switch (opcode) {
    case 0x37: c.WX(rd, imm_u); break;
    case 0x17: c.WX(rd, c.ADD(pc, imm_u)); break;
    case 0x6F:
        c.WX(rd, c.ADD(pc, c.KFOUR()));
        next_pc = c.ADD(pc, (uint32_t)imm_j);
        break;
    case 0x67: {
        uint32_t t = c.AND(c.ADD(c.RX(rs1), (uint32_t)imm_i), c.KNOT1());
        c.WX(rd, c.ADD(pc, c.KFOUR()));
        next_pc = t;
        break;
    }
    case 0x63: {
        uint32_t a = c.RX(rs1), b = c.RX(rs2);
        bool take = false;
        switch (fn3) {
        case 0: take = !c.BNE(a, b); break;
        case 1: take =  c.BNE(a, b); break;
        case 4: take =  c.BNE(c.SLT (a, b), 0u); break;
        case 5: take = !c.BNE(c.SLT (a, b), 0u); break;
        case 6: take =  c.BNE(c.SLTU(a, b), 0u); break;
        case 7: take = !c.BNE(c.SLTU(a, b), 0u); break;
        }
        if (take) next_pc = c.ADD(pc, (uint32_t)imm_b);
        break;
    }
    case 0x03: {
        uint32_t addr = c.ADD(c.RX(rs1), (uint32_t)imm_i);
        switch (fn3) {
        case 0: c.WX(rd, c.load_b (addr)); break;
        case 1: c.WX(rd, c.load_h (addr)); break;
        case 2: c.WX(rd, c.LW     (addr)); break;
        case 4: c.WX(rd, c.load_bu(addr)); break;
        case 5: c.WX(rd, c.load_hu(addr)); break;
        }
        break;
    }
    case 0x23: {
        uint32_t addr = c.ADD(c.RX(rs1), (uint32_t)imm_s);
        switch (fn3) {
        case 0: c.store_b(addr, c.RX(rs2)); break;
        case 1: c.store_h(addr, c.RX(rs2)); break;
        case 2: c.SW    (addr, c.RX(rs2)); break;
        }
        break;
    }
    case 0x13: {
        uint32_t a = c.RX(rs1), imm = (uint32_t)imm_i, sh = rs2, res = 0;
        switch (fn3) {
        case 0: res = c.ADD (a, imm); break;
        case 1: res = c.SLL (a, sh);  break;
        case 2: res = c.SLT (a, imm); break;
        case 3: res = c.SLTU(a, imm); break;
        case 4: res = c.XOR (a, imm); break;
        case 5: res = (fn7 == 0x20) ? c.SRA(a, sh) : c.SRL(a, sh); break;
        case 6: res = c.OR  (a, imm); break;
        case 7: res = c.AND (a, imm); break;
        }
        c.WX(rd, res);
        break;
    }
    case 0x33: {
        uint32_t a = c.RX(rs1), b = c.RX(rs2), sh = c.AND(b, c.K[2]), res = 0;
        switch (fn3) {
        case 0: res = (fn7 == 0x20) ? c.SUB(a, b) : c.ADD(a, b); break;
        case 1: res = c.SLL (a, sh); break;
        case 2: res = c.SLT (a, b);  break;
        case 3: res = c.SLTU(a, b);  break;
        case 4: res = c.XOR (a, b);  break;
        case 5: res = (fn7 == 0x20) ? c.SRA(a, sh) : c.SRL(a, sh); break;
        case 6: res = c.OR  (a, b);  break;
        case 7: res = c.AND (a, b);  break;
        }
        c.WX(rd, res);
        break;
    }
    case 0x0F: break;
    case 0x73:
        if ((inst >> 20) == 1 && rd == 0 && rs1 == 0 && fn3 == 0) return false;
        break;
    }

    c.WPC(next_pc);
    return true;
}

// ============================================================
// C ABI
// ============================================================

EXPORT Core* core_create(uint32_t* mem, uint32_t entry_pc) {
    Core* c = new Core{};
    c->attach(mem, entry_pc);
    return c;
}
EXPORT int      core_step    (Core* c) { return do_step(*c) ? 1 : 0; }
EXPORT void     core_run     (Core* c) { while (do_step(*c)) {} }
EXPORT void     core_destroy (Core* c) { delete c; }
EXPORT uint32_t core_get_pc  (Core* c) { return c->peek_pc(); }
EXPORT uint32_t core_get_reg (Core* c, int i) { return (i >= 0 && i < 32) ? c->peek_reg((uint32_t)i) : 0; }
EXPORT uint64_t core_get_uops(Core* c) { return c->op_count(); }
