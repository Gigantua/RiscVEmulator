#include <cstdint>
#include <bit>

struct Core {
    uint32_t  x[32]{};
    uint32_t  pc = 0;
    uint32_t* mem = nullptr;
    uint32_t  r0 = 0, r1 = 0, r2 = 0;
    uint32_t  zero = 0;
    uint32_t  sink = 0;
    uint64_t  uops = 0;

    // ============================================================
    // BASE: the single instruction primitive.
    // ============================================================
    uint32_t OP(uint32_t a, uint32_t n, uint32_t b, uint32_t c, uint32_t* pl, uint32_t* ps) {
        ++uops;
        return *ps = std::rotl(a + c, n) ^ b ^ *pl;
    }

    // ============================================================
    // Wrappers around OP — each is exactly one OP call.
    // ============================================================
    uint32_t ARX(uint32_t a, uint32_t n, uint32_t b, uint32_t c) { return OP(a, n, b, c, &zero, &sink); }
    uint32_t LW (uint32_t widx)                                  { return OP(0, 0, 0, 0, &mem[widx], &sink); }
    void     SW (uint32_t widx, uint32_t v)                      { (void)OP(v, 0, 0, 0, &zero, &mem[widx]); }
    void     WX (uint32_t i, uint32_t v)                         { (void)OP(v, 0, 0, 0, &zero, &x[i]); }
    void     WPC(uint32_t v)                                     { (void)OP(v, 0, 0, 0, &zero, &pc); }

    // ============================================================
    // Operand-selection aliases for ARX.
    // ============================================================
    uint32_t ADD(uint32_t a, uint32_t b)             { return ARX(a, 0, 0, b); }
    uint32_t XOR(uint32_t a, uint32_t b)             { return ARX(a, 0, b, 0); }
    uint32_t ROL(uint32_t a, uint32_t n)             { return ARX(a, n, 0, 0); }
    uint32_t FSX(uint32_t a, uint32_t n, uint32_t b) { return ARX(a, n, b, 0); }

    // ============================================================
    // Microcode macros
    // ============================================================
    uint32_t NOT(uint32_t a)             { return ARX(a, 0, 0xFFFFFFFFu, 0); }
    uint32_t NEG(uint32_t a)             { return ADD(NOT(a), 1u); }
    uint32_t SUB(uint32_t a, uint32_t b) { return ADD(a, NEG(b)); }

    uint32_t BIT(uint32_t rs, uint32_t i) {
        r0 = ROL(rs, SUB(31u, i));
        return FSX(r0, 1u, ADD(r0, r0));
    }

    uint32_t AND(uint32_t a, uint32_t b) {
        return FSX(
            SUB(ADD(a, b), XOR(a, b)), 31u,
            ROL(BIT(ADD(BIT(a, 31u), BIT(b, 31u)), 1u), 31u)
        );
    }
    uint32_t OR(uint32_t a, uint32_t b) { return XOR(XOR(a, b), AND(a, b)); }

    uint32_t SLL(uint32_t a, uint32_t n) {
        r1 = AND(n, 31u);
        return AND(ROL(a, r1), NOT(SUB(ROL(1u, r1), 1u)));
    }
    uint32_t SRL(uint32_t a, uint32_t n) {
        r1 = AND(n, 31u);
        uint32_t inv = SUB(32u, r1);
        return AND(ROL(a, inv), ROL(NOT(SUB(ROL(1u, r1), 1u)), inv));
    }
    uint32_t SRA(uint32_t a, uint32_t n) {
        r2 = AND(n, 31u);
        uint32_t inv = SUB(32u, r2);
        return OR(SRL(a, r2),
                  AND(NEG(BIT(a, 31u)),
                      NOT(ROL(NOT(SUB(ROL(1u, r2), 1u)), inv))));
    }

    uint32_t SLTU(uint32_t a, uint32_t b) { return BIT(OR(AND(NOT(a), b), AND(NOT(XOR(a, b)), SUB(a, b))), 31u); }
    uint32_t SLT (uint32_t a, uint32_t b) { return SLTU(XOR(a, 0x80000000u), XOR(b, 0x80000000u)); }

    uint32_t SEXT(uint32_t v, uint32_t shift) { return SRA(SLL(v, shift), shift); }

    uint32_t EQ (uint32_t a, uint32_t b) { return SLTU(XOR(a, b), 1u); }

    uint32_t SEL(uint32_t cond, uint32_t t, uint32_t f) { return XOR(f, AND(XOR(t, f), NEG(cond))); }

    uint32_t SR(uint32_t a, uint32_t sh, uint32_t fn7) { return SEL(BIT(fn7, 5u), SRA(a, sh), SRL(a, sh)); }

    uint32_t WORD_OF(uint32_t addr) { return SRL(addr, 2u); }

    // ============================================================
    // Register read — needs SEL and EQ from above.
    // ============================================================
    uint32_t RX(uint32_t i) { return SEL(EQ(i, 0u), 0u, OP(0, 0, 0, 0, &x[i], &sink)); }

    // ============================================================
    // Instruction-field decoders.
    // ============================================================
    uint32_t RD    (uint32_t i) { return AND(SRL(i,  7u), 0x1Fu); }
    uint32_t FN3   (uint32_t i) { return AND(SRL(i, 12u), 0x7u);  }
    uint32_t RS1   (uint32_t i) { return AND(SRL(i, 15u), 0x1Fu); }
    uint32_t RS2   (uint32_t i) { return AND(SRL(i, 20u), 0x1Fu); }
    uint32_t FN7   (uint32_t i) { return AND(SRL(i, 25u), 0x7Fu); }

    uint32_t IMM_I(uint32_t i) { return SRA(i, 20u); }
    uint32_t IMM_U(uint32_t i) { return AND(i, 0xFFFFF000u); }

    // ============================================================
    // Sub-word memory helpers.
    // ============================================================
    uint32_t load_sub(uint32_t addr, uint32_t mask) {
        uint32_t bshift = SLL(AND(addr, 3u), 3u);
        return AND(SRL(LW(WORD_OF(addr)), bshift), mask);
    }
    uint32_t load_bu(uint32_t addr) { return load_sub(addr, 0xFFu);   }
    uint32_t load_hu(uint32_t addr) { return load_sub(addr, 0xFFFFu); }

    void store_sub(uint32_t addr, uint32_t v, uint32_t lane_mask) {
        uint32_t bshift = SLL(AND(addr, 3u), 3u);
        uint32_t widx   = WORD_OF(addr);
        SW(widx, OR(AND(LW(widx), NOT(SLL(lane_mask, bshift))),
                    SLL(AND(v, lane_mask), bshift)));
    }

    // ============================================================
    // PC advance.
    // ============================================================
    void STEP_PC(uint32_t cur) { WPC(ADD(cur, 4u)); }

    // ============================================================
    // Dispatch — one RV32I instruction per call.
    // ============================================================
    uint32_t step() {
        uint32_t cur    = OP(0, 0, 0, 0, &pc, &sink);
        uint32_t inst   = LW(WORD_OF(cur));
        uint32_t opcode = AND(inst, 0x7Fu);

        switch (opcode) {
        case 0x37:
            WX(RD(inst), IMM_U(inst));
            STEP_PC(cur);
            break;
        case 0x17:
            WX(RD(inst), ADD(cur, IMM_U(inst)));
            STEP_PC(cur);
            break;
        case 0x6F: {
            uint32_t imm_j = OR(SRA(AND(inst, 0x80000000u), 11u),
                             OR(AND(inst, 0xFF000u),
                             OR(AND(SRL(inst,  9u), 0x800u),
                                AND(SRL(inst, 20u), 0x7FEu))));
            WX(RD(inst), ADD(cur, 4u));
            WPC(ADD(cur, imm_j));
            break;
        }
        case 0x67: {
            uint32_t t = AND(ADD(RX(RS1(inst)), IMM_I(inst)), 0xFFFFFFFEu);
            WX(RD(inst), ADD(cur, 4u));
            WPC(t);
            break;
        }
        case 0x63: {
            uint32_t imm_b = OR(SRA(AND(inst, 0x80000000u), 19u),
                             OR(SLL(AND(inst,        0x80u),  4u),
                             OR(AND(SRL(inst, 20u), 0x7E0u),
                                AND(SRL(inst,  7u), 0x1Eu))));
            uint32_t a = RX(RS1(inst)), b = RX(RS2(inst)), cond = 0;
            switch (FN3(inst)) {
            case 0: cond =       EQ  (a, b);     break;
            case 1: cond = SLTU(0u, XOR(a, b));  break;
            case 4: cond =       SLT (a, b);     break;
            case 5: cond = XOR(SLT (a, b), 1u);  break;
            case 6: cond =       SLTU(a, b);     break;
            case 7: cond = XOR(SLTU(a, b), 1u);  break;
            }
            WPC(SEL(cond, ADD(cur, imm_b), ADD(cur, 4u)));
            break;
        }
        case 0x03: {
            uint32_t rd = RD(inst), addr = ADD(RX(RS1(inst)), IMM_I(inst));
            switch (FN3(inst)) {
            case 0: WX(rd, SEXT(load_bu(addr), 24u)); break;
            case 1: WX(rd, SEXT(load_hu(addr), 16u)); break;
            case 2: WX(rd, LW(WORD_OF(addr)));        break;
            case 4: WX(rd, load_bu(addr));            break;
            case 5: WX(rd, load_hu(addr));            break;
            }
            STEP_PC(cur);
            break;
        }
        case 0x23: {
            uint32_t imm_s = OR(SRA(AND(inst, 0xFE000000u), 20u),
                                AND(SRL(inst,  7u), 0x1Fu));
            uint32_t rs2v = RX(RS2(inst)), addr = ADD(RX(RS1(inst)), imm_s);
            switch (FN3(inst)) {
            case 0: store_sub(addr, rs2v, 0xFFu);     break;
            case 1: store_sub(addr, rs2v, 0xFFFFu);   break;
            case 2: SW(WORD_OF(addr), rs2v);          break;
            }
            STEP_PC(cur);
            break;
        }
        case 0x13: {
            uint32_t rd = RD(inst), a = RX(RS1(inst)), imm = IMM_I(inst), res = 0;
            switch (FN3(inst)) {
            case 0: res = ADD (a, imm);                     break;
            case 1: res = SLL (a, RS2(inst));               break;
            case 2: res = SLT (a, imm);                     break;
            case 3: res = SLTU(a, imm);                     break;
            case 4: res = XOR (a, imm);                     break;
            case 5: res = SR  (a, RS2(inst), FN7(inst));    break;
            case 6: res = OR  (a, imm);                     break;
            case 7: res = AND (a, imm);                     break;
            }
            WX(rd, res);
            STEP_PC(cur);
            break;
        }
        case 0x33: {
            uint32_t rd = RD(inst), a = RX(RS1(inst)), b = RX(RS2(inst)), res = 0;
            switch (FN3(inst)) {
            case 0: res = SEL(BIT(FN7(inst), 5u), SUB(a, b), ADD(a, b)); break;
            case 1: res = SLL (a, b);                break;
            case 2: res = SLT (a, b);                break;
            case 3: res = SLTU(a, b);                break;
            case 4: res = XOR (a, b);                break;
            case 5: res = SR  (a, b, FN7(inst));     break;
            case 6: res = OR  (a, b);                break;
            case 7: res = AND (a, b);                break;
            }
            WX(rd, res);
            STEP_PC(cur);
            break;
        }
        case 0x0F: STEP_PC(cur); break;
        case 0x73: break;
        }

        return XOR(EQ(opcode, 0x73u), 1u);
    }
};

extern "C" {
    __declspec(dllexport) Core* core_create(uint32_t* mem, uint32_t entry_pc) {
        Core* c = new Core{};
        c->mem = mem;
        c->pc  = entry_pc;
        return c;
    }
    __declspec(dllexport) int      core_step    (Core* c)        { return (int)c->step(); }
    __declspec(dllexport) void     core_run     (Core* c)        { while (c->step()) {} }
    __declspec(dllexport) void     core_destroy (Core* c)        { delete c; }
    __declspec(dllexport) uint32_t core_get_pc  (Core* c)        { return c->pc; }
    __declspec(dllexport) uint32_t core_get_reg (Core* c, int i) { return (i >= 0 && i < 32) ? (i == 0 ? 0u : c->x[i]) : 0; }
    __declspec(dllexport) uint64_t core_get_uops(Core* c)        { return c->uops; }
}
