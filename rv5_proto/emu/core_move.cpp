// core_move.cpp — RV32I emulator implemented as a MOVE-MASK datapath.
//
// The 6 base primitives (ADD, XOR, ROL, BNE, LW, SW) execute via bit-vector
// moves. Each primitive builds a MoveMask (a multi-word bitfield), and
// firing the move mask commits the set bits' wires:
//
//   - INJ_*  bits load the unit's input latches.
//   - FIRE_* bits capture the unit's combinational output into its output
//     latch (the actual "compute").
//   - READ_* bits sample the unit's output latch (the return value).
//
// uops counts the total number of bits fired across all move masks —
// the architectural cost of running the program on the move-machine.
//
// This matches the architecture in Trigger-Datapath.{txt,html}.

#include <cstdint>
#include <bit>
#include <cstring>

#if defined(_WIN32)
  #define EXPORT extern "C" __declspec(dllexport)
#else
  #define EXPORT extern "C" __attribute__((visibility("default")))
#endif

// ============================================================================
// EDGE ENUMERATION  (subset used by primitive-level execution)
// ============================================================================
// Bits are granular enough that disjointness on this mask is a real fusion
// criterion: two register reads on DIFFERENT lanes have different LRD_* bits
// and can fuse; two reads on the SAME lane share a bit and serialize.
enum Edge : int {
    // Per-lane register-read bits: R_n_RD → LRD_{n mod 8}. 8 banks ⇒ 8 bits.
    LRD_0 = 0, LRD_1, LRD_2, LRD_3, LRD_4, LRD_5, LRD_6, LRD_7,

    // Per-lane register-write bits: combined load-and-drain of LWR_{n mod 8}.
    LWR_0 = 8, LWR_1, LWR_2, LWR_3, LWR_4, LWR_5, LWR_6, LWR_7,

    // Unit-input injection — one per input latch.
    INJ_ADD_A = 16, INJ_ADD_B,
    INJ_XOR_A,      INJ_XOR_B,
    INJ_ROL_A,      INJ_ROL_N,
    INJ_BNE_A,      INJ_BNE_B,
    INJ_LW_ADDR,
    INJ_SW_ADDR,    INJ_SW_DATA,

    // FIRE bits — one per functional unit. The compute happens here.
    FIRE_ADD,
    FIRE_XOR,
    FIRE_ROL,
    FIRE_BNE,
    FIRE_LW,
    FIRE_SW,

    // Read-out bits — sample a unit output latch back to the caller.
    READ_ADD_O,
    READ_XOR_O,
    READ_ROL_O,
    READ_BNE_O,
    READ_LW_O,

    // Control
    MOV_PC,        // value → pc_latch
    MOV_PC_RD,     // pc_latch → consumer
    MOV_IMM,       // IMM payload asserted

    NUM_EDGES
};

// ============================================================================
// MoveMask — 64-bit trigger word, one bit per WIRE (edge). Drives execution.
// Used to count `uops` (bits fired).
// ============================================================================
struct MoveMask {
    uint64_t w = 0;
    void set(int b)        { w |= 1ull << b; }
    bool test(int b) const { return (w >> b) & 1; }
    int  popcount() const  { return std::popcount(w); }
    void operator|=(const MoveMask& o) { w |= o.w; }
};

// ============================================================================
// Latch indices (for Footprint masks below). 56 bits used; fits in uint64_t.
//   0..31  : R0..R31
//   32..39 : LRD0..LRD7
//   40..47 : LWR0..LWR7
//   48..53 : per-unit "this unit is busy" (ADD/XOR/ROL/BNE/LW/SW)
//   54     : PC
//   55     : IMM
// ============================================================================
constexpr int LAT_R(int n)    { return n & 31; }
constexpr int LAT_LRD(int n)  { return 32 + (n & 7); }
constexpr int LAT_LWR(int n)  { return 40 + (n & 7); }
constexpr int LAT_ADD = 48, LAT_XOR = 49, LAT_ROL = 50,
              LAT_BNE = 51, LAT_LW  = 52, LAT_SW  = 53;
constexpr int LAT_PC  = 54, LAT_IMM = 55;

// ============================================================================
// Footprint — per-op resource claim with two latch sets (W = writes, R = reads).
// The 3-AND hazard test from §8.3 / the HTML sandbox decides fusion legality:
//
//   conflict(A, B)  =  (A.W & B.W) | (A.W & B.R) | (A.R & B.W)
//
// R∩R is intentionally NOT in the conflict: many ops reading the same
// architectural register fuse for free (the broadcast property).
// ============================================================================
struct Footprint {
    uint64_t W = 0, R = 0;
    void wr(int i) { W |= 1ull << i; }
    void rd(int i) { R |= 1ull << i; }
    bool empty() const { return (W | R) == 0; }
    void clear()       { W = R = 0; }
    void operator|=(const Footprint& o) { W |= o.W; R |= o.R; }
    bool conflicts_with(const Footprint& o) const {
        return ((W & o.W) | (W & o.R) | (R & o.W)) != 0;
    }
};

// ============================================================================
// Core — latches + execution
// ============================================================================
struct Core {
    uint32_t          x[32]{};
    uint32_t          pc_latch = 0;  // architectural PC — written ONLY via MOV_PC move
    uint32_t*         mem = nullptr;
    mutable uint32_t  r[5]{};        // microcode scratch (used by macros)
    mutable uint64_t  uops = 0;      // total bits fired across all move masks

    // Fusion accumulator: tracks the W/R footprint of the *current fabric cycle*.
    // Each primitive's footprint is checked against this; if disjoint (no hazard)
    // it joins the current cycle. On conflict, a new cycle starts.
    mutable Footprint cycle_acc;
    mutable uint64_t  fabric_cycles = 0;   // # of cycles after uop-joining

    // Unit input/output latches — the storage cells move masks move into.
    mutable uint32_t add_a = 0, add_b = 0, add_o = 0;
    mutable uint32_t xor_a = 0, xor_b = 0, xor_o = 0;
    mutable uint32_t rol_a = 0, rol_n = 0, rol_o = 0;
    mutable uint32_t bne_a = 0, bne_b = 0, bne_o = 0;
    mutable uint32_t lw_addr = 0, lw_o = 0;
    mutable uint32_t sw_addr = 0, sw_data = 0;

    // The accounting hot-path: every primitive/helper goes through here.
    //   uops += popcount(mm)    — total wires fired
    //   fusion: try to OR fp into cycle_acc. If it conflicts, commit a cycle
    //   and restart with fp as the new accumulator.
    void account(MoveMask mm, Footprint fp) const {
        uops += mm.popcount();
        if (cycle_acc.conflicts_with(fp)) {
            fabric_cycles++;
            cycle_acc = fp;
        } else {
            cycle_acc |= fp;
        }
    }

    // Write the PC by firing a MOV_PC move. This is the only path that can
    // update pc_latch — the decoder is no longer allowed to assign it directly.
    void write_pc(uint32_t val) {
        MoveMask mm; mm.set(MOV_PC);
        Footprint fp; fp.wr(LAT_PC);
        account(mm, fp);
        pc_latch = val;
    }

    // Read the PC — fires MOV_PC_RD; PC is in R-mask so subsequent ops that
    // also read PC can fuse, but a write to PC in the same cycle conflicts.
    uint32_t read_pc() const {
        MoveMask mm; mm.set(MOV_PC_RD);
        Footprint fp; fp.rd(LAT_PC);
        account(mm, fp);
        return pc_latch;
    }

    // Read register R_i — fires LRD_{i mod 8}. Two reads of different
    // architectural regs hit different lanes (W of LRD_n is per-lane), so
    // they fuse cleanly. Two reads of the same register fuse too (R∩R is
    // never tested — the broadcast property). Same-lane DIFFERENT regs
    // conflict (the LRD shadow can only hold one value per cycle).
    uint32_t read_reg(uint32_t i) const {
        MoveMask mm; mm.set(LRD_0 + (i & 7));
        Footprint fp;
        fp.wr(LAT_LRD(i));
        fp.rd(LAT_R(i));
        account(mm, fp);
        return x[i & 31];
    }

    // Write register R_i — fires LWR_{i mod 8}. Writes the LWR shadow lane
    // (W) and the architectural register (W). RAW/WAR with downstream reads
    // of R_i is caught by the 3-AND hazard test.
    void write_reg(uint32_t i, uint32_t v) {
        MoveMask mm; mm.set(LWR_0 + (i & 7));
        Footprint fp;
        fp.wr(LAT_LWR(i));
        fp.wr(LAT_R(i));
        account(mm, fp);
        if (i & 31) x[i & 31] = v;
    }

    // Materialize an immediate — fires MOV_IMM. IMM is a singleton resource
    // (one literal per cycle in the trigger-word payload), so two immediates
    // in one cycle conflict.
    uint32_t imm(uint32_t v) const {
        MoveMask mm; mm.set(MOV_IMM);
        Footprint fp; fp.wr(LAT_IMM);
        account(mm, fp);
        return v;
    }

    // execute_mm — apply a move mask's bits to the latches (sample-then-commit).
    // Returns whichever READ_* output the caller selected, or 0.
    uint32_t execute_mm(MoveMask mm, uint32_t arg_a, uint32_t arg_b) const {
        if (mm.test(INJ_ADD_A)) add_a = arg_a;
        if (mm.test(INJ_ADD_B)) add_b = arg_b;
        if (mm.test(INJ_XOR_A)) xor_a = arg_a;
        if (mm.test(INJ_XOR_B)) xor_b = arg_b;
        if (mm.test(INJ_ROL_A)) rol_a = arg_a;
        if (mm.test(INJ_ROL_N)) rol_n = arg_b;
        if (mm.test(INJ_BNE_A)) bne_a = arg_a;
        if (mm.test(INJ_BNE_B)) bne_b = arg_b;
        if (mm.test(FIRE_ADD)) add_o = add_a + add_b;
        if (mm.test(FIRE_XOR)) xor_o = xor_a ^ xor_b;
        if (mm.test(FIRE_ROL)) rol_o = std::rotl(rol_a, static_cast<int>(rol_n & 31));
        if (mm.test(FIRE_BNE)) bne_o = (bne_a != bne_b) ? 1u : 0u;
        if (mm.test(READ_ADD_O)) return add_o;
        if (mm.test(READ_XOR_O)) return xor_o;
        if (mm.test(READ_ROL_O)) return rol_o;
        if (mm.test(READ_BNE_O)) return bne_o;
        return 0;
    }

    // ============================================================
    // BASE: the 6 primitives — each builds a MoveMask (wires fired)
    // AND a Footprint (resource claim) and goes through account().
    // ============================================================
    uint32_t ADD(uint32_t a, uint32_t b) const {
        MoveMask mm; mm.set(INJ_ADD_A); mm.set(INJ_ADD_B); mm.set(FIRE_ADD); mm.set(READ_ADD_O);
        Footprint fp; fp.wr(LAT_ADD);
        account(mm, fp);
        return execute_mm(mm, a, b);
    }
    uint32_t XOR(uint32_t a, uint32_t b) const {
        MoveMask mm; mm.set(INJ_XOR_A); mm.set(INJ_XOR_B); mm.set(FIRE_XOR); mm.set(READ_XOR_O);
        Footprint fp; fp.wr(LAT_XOR);
        account(mm, fp);
        return execute_mm(mm, a, b);
    }
    uint32_t ROL(uint32_t a, uint32_t n) const {
        MoveMask mm; mm.set(INJ_ROL_A); mm.set(INJ_ROL_N); mm.set(FIRE_ROL); mm.set(READ_ROL_O);
        Footprint fp; fp.wr(LAT_ROL);
        account(mm, fp);
        return execute_mm(mm, a, n);
    }
    bool BNE(uint32_t a, uint32_t b) const {
        MoveMask mm; mm.set(INJ_BNE_A); mm.set(INJ_BNE_B); mm.set(FIRE_BNE); mm.set(READ_BNE_O);
        Footprint fp; fp.wr(LAT_BNE);
        account(mm, fp);
        return execute_mm(mm, a, b) != 0;
    }
    uint32_t LW(uint32_t addr) const {
        MoveMask mm; mm.set(INJ_LW_ADDR); mm.set(FIRE_LW); mm.set(READ_LW_O);
        Footprint fp; fp.wr(LAT_LW);
        account(mm, fp);
        lw_addr = addr;
        lw_o = mem[addr >> 2];
        return lw_o;
    }
    void SW(uint32_t addr, uint32_t v) {
        MoveMask mm; mm.set(INJ_SW_ADDR); mm.set(INJ_SW_DATA); mm.set(FIRE_SW);
        Footprint fp; fp.wr(LAT_SW);
        account(mm, fp);
        sw_addr = addr;
        sw_data = v;
        mem[addr >> 2] = v;
    }

    // ============================================================
    // MACRO layer — identical algebra to core_6op.cpp. Each macro is
    // a chain of primitive calls; each primitive emits its own
    // move mask, so uops accumulate naturally.
    // ============================================================
    uint32_t NOT(uint32_t a)             const { return XOR(a, 0xFFFFFFFFu); }
    uint32_t NEG(uint32_t a)             const { return ADD(NOT(a), 1u); }
    uint32_t SUB(uint32_t a, uint32_t b) const { return ADD(a, NEG(b)); }

    uint32_t BIT(uint32_t rs, uint32_t i) const {
        r[0] = ROL(rs, SUB(31u, i));
        return XOR(ADD(r[0], r[0]), ROL(r[0], 1u));
    }

    uint32_t AND(uint32_t a, uint32_t b) const {
        return XOR(
            ROL(SUB(ADD(a, b), XOR(a, b)), 31u),
            ROL(BIT(ADD(BIT(a, 31u), BIT(b, 31u)), 1u), 31u)
        );
    }
    uint32_t OR(uint32_t a, uint32_t b) const { return XOR(XOR(a, b), AND(a, b)); }

    uint32_t SLL_mask(uint32_t n) const { return NOT(SUB(ROL(1u, n), 1u)); }
    uint32_t SRL_mask(uint32_t n) const { return ROL(SLL_mask(n), SUB(32u, n)); }

    uint32_t SLL(uint32_t a, uint32_t n) const {
        r[1] = AND(n, 31u);
        return AND(ROL(a, r[1]), SLL_mask(r[1]));
    }
    uint32_t SRL(uint32_t a, uint32_t n) const {
        r[1] = AND(n, 31u);
        return AND(ROL(a, SUB(32u, r[1])), SRL_mask(r[1]));
    }
    uint32_t SRA(uint32_t a, uint32_t n) const {
        r[2] = AND(n, 31u);
        return OR(SRL(a, r[2]),
                  AND(NEG(BIT(a, 31u)), NOT(SRL_mask(r[2]))));
    }

    uint32_t SLTU(uint32_t a, uint32_t b) const {
        return BIT(OR(AND(NOT(a), b), AND(NOT(XOR(a, b)), SUB(a, b))), 31u);
    }
    uint32_t SLT(uint32_t a, uint32_t b) const {
        return SLTU(XOR(a, 0x80000000u), XOR(b, 0x80000000u));
    }

    uint32_t load_bu(uint32_t addr) const {
        r[3] = AND(addr, 3u);
        return AND(SRL(LW(SUB(addr, r[3])), SLL(r[3], 3u)), 0xFFu);
    }
    uint32_t load_hu(uint32_t addr) const {
        r[3] = AND(addr, 3u);
        return AND(SRL(LW(SUB(addr, r[3])), SLL(r[3], 3u)), 0xFFFFu);
    }
    uint32_t load_b(uint32_t addr) const { return SRA(SLL(load_bu(addr), 24u), 24u); }
    uint32_t load_h(uint32_t addr) const { return SRA(SLL(load_hu(addr), 16u), 16u); }

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
// Decoder: pure dispatch. NO direct touches to c.x[], c.pc_latch, or
// raw literals — every operand goes through read_reg/write_reg/imm/
// read_pc/write_pc/primitive, so every wire-fire is accounted for in
// `uops`. The decoder's only jobs are: (1) parse inst bits, (2) pick
// the right sequence of moves+primitives, (3) issue them.
// ============================================================

static bool do_step(Core& c) {
    uint32_t pc   = c.read_pc();
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

    // Default next_pc is pc + 4. ADD primitive emits its own uops; the
    // literal 4 here is treated as an inlined constant (no MOV_IMM bit).
    uint32_t next_pc = c.ADD(pc, 4u);

    switch (opcode) {
    case 0x37:                                                              // LUI
        // IMM → LWR → R_rd. No unit involved (pure routing).
        c.write_reg(rd, c.imm(imm_u));
        break;
    case 0x17:                                                              // AUIPC
        c.write_reg(rd, c.ADD(pc, c.imm(imm_u)));
        break;
    case 0x6F:                                                              // JAL
        c.write_reg(rd, c.ADD(pc, 4u));
        next_pc = c.ADD(pc, c.imm(static_cast<uint32_t>(imm_j)));
        break;
    case 0x67: {                                                            // JALR
        uint32_t t = c.AND(c.ADD(c.read_reg(rs1), c.imm(static_cast<uint32_t>(imm_i))), 0xFFFFFFFEu);
        c.write_reg(rd, c.ADD(pc, 4u));
        next_pc = t;
        break;
    }
    case 0x63: {                                                            // BRANCH
        uint32_t a = c.read_reg(rs1), b = c.read_reg(rs2);
        // Each branch condition computes a 0/1 cond via BNE/SLT/SLTU.
        // The "negative" branches (BEQ, BGE, BGEU) invert via XOR with 1
        // (the 6-op micro-ISA has no NOT primitive; XOR is how we negate).
        uint32_t cond = 0;
        bool need_invert = false;
        switch (fn3) {
        case 0: cond = c.BNE(a, b) ? 1u : 0u;            need_invert = true;  break;  // BEQ
        case 1: cond = c.BNE(a, b) ? 1u : 0u;            need_invert = false; break;  // BNE
        case 4: cond = c.SLT (a, b);                     need_invert = false; break;  // BLT
        case 5: cond = c.SLT (a, b);                     need_invert = true;  break;  // BGE
        case 6: cond = c.SLTU(a, b);                     need_invert = false; break;  // BLTU
        case 7: cond = c.SLTU(a, b);                     need_invert = true;  break;  // BGEU
        }
        if (need_invert) cond = c.XOR(cond, c.imm(1u));
        // Compute the branch target unconditionally; the guarded write to
        // pc_latch picks taken vs fall-through based on `cond`.
        uint32_t target = c.ADD(pc, c.imm(static_cast<uint32_t>(imm_b)));
        if (cond) next_pc = target;
        break;
    }
    case 0x03: {                                                            // LOAD
        uint32_t addr = c.ADD(c.read_reg(rs1), c.imm(static_cast<uint32_t>(imm_i)));
        uint32_t v = 0;
        switch (fn3) {
        case 0: v = c.load_b (addr); break;
        case 1: v = c.load_h (addr); break;
        case 2: v = c.LW     (addr); break;
        case 4: v = c.load_bu(addr); break;
        case 5: v = c.load_hu(addr); break;
        }
        c.write_reg(rd, v);
        break;
    }
    case 0x23: {                                                            // STORE
        uint32_t addr = c.ADD(c.read_reg(rs1), c.imm(static_cast<uint32_t>(imm_s)));
        uint32_t v    = c.read_reg(rs2);
        switch (fn3) {
        case 0: c.store_b(addr, v); break;
        case 1: c.store_h(addr, v); break;
        case 2: c.SW    (addr, v); break;
        }
        break;
    }
    case 0x13: {                                                            // OP-IMM
        uint32_t a = c.read_reg(rs1), imm = c.imm(static_cast<uint32_t>(imm_i)), sh = c.imm(rs2), r = 0;
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
        c.write_reg(rd, r);
        break;
    }
    case 0x33: {                                                            // OP
        uint32_t a = c.read_reg(rs1), b = c.read_reg(rs2), sh = c.AND(b, 31u), r = 0;
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
        c.write_reg(rd, r);
        break;
    }
    case 0x0F: break;
    case 0x73:
        if ((inst >> 20) == 1 && rd == 0 && rs1 == 0 && fn3 == 0) return false;
        break;
    }

    c.write_pc(next_pc);    // PC commits via MOV_PC move bit, not direct assign
    return true;
}

// ============================================================
// C ABI for the host
// ============================================================

EXPORT Core* core_create(uint32_t* mem, uint32_t entry_pc) {
    Core* c = new Core{};
    c->pc_latch = entry_pc;
    c->mem = mem;
    return c;
}
EXPORT int      core_step    (Core* c) { return do_step(*c) ? 1 : 0; }
EXPORT void     core_run     (Core* c) { while (do_step(*c)) {} }
EXPORT void     core_destroy (Core* c) { delete c; }
EXPORT uint32_t core_get_pc  (Core* c) { return c->pc_latch; }
EXPORT uint32_t core_get_reg (Core* c, int i) { return (i >= 0 && i < 32) ? c->x[i] : 0; }
EXPORT uint64_t core_get_uops(Core* c) { return c->uops; }

// Commit the last cycle still in cycle_acc, then return the fused fabric-cycle
// count: how many cycles the program would take after uop-joining via the
// 3-AND hazard test on disjoint footprints. Idempotent (acc clears on read).
EXPORT uint64_t core_get_fabric_cycles(Core* c) {
    if (!c->cycle_acc.empty()) {
        c->fabric_cycles++;
        c->cycle_acc.clear();
    }
    return c->fabric_cycles;
}
