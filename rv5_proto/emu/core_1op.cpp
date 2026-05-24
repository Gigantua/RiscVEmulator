// RV32I emulator on a 1-op core: {OP}.
//
// ============================================================
// What this file minimizes (and what it does not)
// ============================================================
// METRIC: instruction-set cardinality — exactly one. Every dynamic
// step is a call to OP. OP itself has no branches, no mode flag,
// no dispatch — the same fused gate-level shape every time.
//
// What this file does NOT minimize:
//   - Gates. OP is wider than any 4-op cell: it always touches
//     two cells (one read, one write) on top of the ARX kernel.
//   - Dynamic op count. Unchanged — every 4-op call is one OP call.
//   - Encoding bits per op. OP carries two cell selectors on top of
//     the four ARX operands.
//
// ============================================================
// The primitive — one shape, every time
// ============================================================
//     OP(a, n, b, c, *pl, *ps):
//       m = *pl                                       // unconditional load
//       v = rotl(a + c, n) ^ b ^ m                    // fused ARX-XOR
//       *ps = v                                       // unconditional store
//       return v
//
// No mode flag, no internal branch. Variation is purely operand
// selection — the same kind of choice as picking which value goes
// into `a`. To "skip" the load, pass &x[0] (a cell that is never
// written by anyone). To "skip" the store, pass &sink (a cell that
// is never read by anyone). zero and sink are ordinary cells; OP
// does not know they are special.
//
// Reductions to the previous four primitives:
//     ARX(a, n, b, c)  = OP(a, n, b, c, &x[0], &sink)
//                          ; m=0  -> v = rotl(a+c, n) ^ b
//     LW (addr)        = OP(0, 0, 0, 0, &mem[addr>>2], &sink)
//                          ; v = m = mem[addr>>2]
//     SW (addr, x)     = OP(x, 0, 0, 0, &x[0], &mem[addr>>2])
//                          ; v = x, stored at addr
//     BNE(a, b)        = OP(a, 0, b, 0, &x[0], &sink) != 0
//                          ; v = a ^ b, nonzero iff a != b
//
// K[] is inlined (no constant pool — every literal sits at its use site).

#include <cstdint>

#if defined(_WIN32)
  #define EXPORT extern "C" __declspec(dllexport)
#else
  #define EXPORT extern "C" __attribute__((visibility("default")))
#endif

struct Core {
    uint32_t  x[32]{};      // x[0] is hardwired zero; x[1..31] are general-purpose registers
    uint32_t  pc = 0;
    uint32_t* mem = nullptr;
    uint32_t  r[5]{};
    uint32_t  sink = 0;     // x[0] in hardware
    uint64_t  uops = 0;

    // ============================================================
    // BASE: the single instruction primitive. 
    // ============================================================
    uint32_t OP(uint32_t a, uint32_t n, uint32_t b, uint32_t c, uint32_t* pl, uint32_t* ps) {
        ++uops;
        uint32_t s = a + c;
        uint32_t k = n & 31u;
        return *ps = ((s << k) | (s >> ((-k) & 31u))) ^ b ^ *pl;
    }

    // ============================================================
    // The 4-op surface — operand-selection wrappers around OP. No
    // wrapper does any computation; each is exactly one OP call.
    // ============================================================
    uint32_t ARX(uint32_t a, uint32_t n, uint32_t b, uint32_t c) { return OP(a, n, b, c, &x[0], &sink);}
    bool     BNE(uint32_t a, uint32_t b)         { return OP(a, 0, b, 0, &x[0], &sink) != 0; }
    uint32_t LW (uint32_t addr)                  { return OP(0, 0, 0, 0, &mem[addr >> 2], &sink); }
    void     SW (uint32_t addr, uint32_t v)      { (void)OP(v, 0, 0, 0, &x[0], &mem[addr >> 2]); }

    // ============================================================
    // 4 ISA Layer
    // ============================================================
    uint32_t ADD(uint32_t a, uint32_t b)             { return ARX(a, 0, 0, b); }
    uint32_t XOR(uint32_t a, uint32_t b)             { return ARX(a, 0, b, 0); }
    uint32_t ROL(uint32_t a, uint32_t n)             { return ARX(a, n, 0, 0); }
    uint32_t FSX(uint32_t a, uint32_t n, uint32_t b) { return ARX(a, n, b, 0); }

    // ============================================================
    // Macros for the rest of the ISA layer, built on top of the 4 primitives.
    // ============================================================

    uint32_t NOT(uint32_t a)             { return ARX(a, 0, 0xFFFFFFFFu, 0); }   // a ^ ~0
    uint32_t NEG(uint32_t a)             { return ADD(NOT(a), 1u); }              // ~a + 1
    uint32_t SUB(uint32_t a, uint32_t b) { return ADD(a, NEG(b)); }

    uint32_t K32()   { return ROL(1u, 5u); }    // 1 << 5  = 32
    uint32_t KMSB()  { return ROL(1u, 31u); }   // 1 << 31 = 0x80000000
    uint32_t KFOUR() { return ROL(1u, 2u); }    // 1 << 2  = 4
    uint32_t KNOT1() { return NOT(1u); }

    // BIT: r[0] = ROL(rs, 31-i);  result = ROL(r[0],1) ^ (r[0]+r[0])
    uint32_t BIT(uint32_t rs, uint32_t i) {
        r[0] = ROL(rs, SUB(31u, i));
        return FSX(r[0], 1u, ADD(r[0], r[0]));
    }

    // AND via A+B = (A^B) + 2(A&B), with bit-31 recovery.
    uint32_t AND(uint32_t a, uint32_t b) {
        return FSX(
            SUB(ADD(a, b), XOR(a, b)), 31u,
            ROL(BIT(ADD(BIT(a, 31u), BIT(b, 31u)), 1u), 31u)
        );
    }
    uint32_t OR(uint32_t a, uint32_t b) { return XOR(XOR(a, b), AND(a, b)); }

    uint32_t SLL_mask(uint32_t n) { return NOT(SUB(ROL(1u, n), 1u)); }
    uint32_t SRL_mask(uint32_t n) { return ROL(SLL_mask(n), SUB(K32(), n)); }

    uint32_t SLL(uint32_t a, uint32_t n) {
        r[1] = AND(n, 31u);
        return AND(ROL(a, r[1]), SLL_mask(r[1]));
    }
    uint32_t SRL(uint32_t a, uint32_t n) {
        r[1] = AND(n, 31u);
        return AND(ROL(a, SUB(K32(), r[1])), SRL_mask(r[1]));
    }
    uint32_t SRA(uint32_t a, uint32_t n) {
        r[2] = AND(n, 31u);
        return OR(SRL(a, r[2]),
                  AND(NEG(BIT(a, 31u)), NOT(SRL_mask(r[2]))));
    }

    uint32_t SLTU(uint32_t a, uint32_t b) {
        return BIT(OR(AND(NOT(a), b), AND(NOT(XOR(a, b)), SUB(a, b))), 31u);
    }
    uint32_t SLT(uint32_t a, uint32_t b) {
        return SLTU(XOR(a, KMSB()), XOR(b, KMSB()));
    }

    uint32_t load_bu(uint32_t addr) {
        r[3] = AND(addr, 3u);
        return AND(SRL(LW(SUB(addr, r[3])), SLL(r[3], 3u)), 0xFFu);
    }
    uint32_t load_hu(uint32_t addr) {
        r[3] = AND(addr, 3u);
        return AND(SRL(LW(SUB(addr, r[3])), SLL(r[3], 3u)), 0xFFFFu);
    }
    uint32_t load_b(uint32_t addr) { return SRA(SLL(load_bu(addr), 24u), 24u); }
    uint32_t load_h(uint32_t addr) { return SRA(SLL(load_hu(addr), 16u), 16u); }

    void store_sub(uint32_t addr, uint32_t v, uint32_t lane_mask) {
        r[3] = AND(addr, 3u);
        r[4] = SLL(r[3], 3u);
        SW(SUB(addr, r[3]),
           OR(AND(LW(SUB(addr, r[3])), NOT(SLL(lane_mask, r[4]))),
              SLL(AND(v, lane_mask), r[4])));
    }
    void store_b(uint32_t addr, uint32_t v) { store_sub(addr, v, 0xFFu); }
    void store_h(uint32_t addr, uint32_t v) { store_sub(addr, v, 0xFFFFu); }
};

// ============================================================
// Decoder — translates RV32I instructions to sequences of uArch calls
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
        uint32_t a = c.x[rs1], b = c.x[rs2], sh = c.AND(b, 31u), r = 0;
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
EXPORT int      core_step    (Core* c) { return do_step(*c) ? 1 : 0; }
EXPORT void     core_run     (Core* c) { while (do_step(*c)) {} }
EXPORT void     core_destroy (Core* c) { delete c; }
EXPORT uint32_t core_get_pc  (Core* c) { return c->pc; }
EXPORT uint32_t core_get_reg (Core* c, int i) { return (i >= 0 && i < 32) ? c->x[i] : 0; }
EXPORT uint64_t core_get_uops(Core* c) { return c->uops; }
