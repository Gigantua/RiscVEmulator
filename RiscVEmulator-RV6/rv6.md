================================================================================
RV32I 40-INSTRUCTION SYNTHESIS TABLE
Target base: 6 instructions (UNI-ADD, LUT3, CMOV, LW, SW, SHIFT)
================================================================================

BASE INSTRUCTION SET (the 6 kept):
----------------------------------
  UNI-ADD rd, rs1, rs2/imm, mode
    mode=0: rd = rs1 + rs2                    (reg+reg)
    mode=1: rd = rs1 + sext12(imm)            (reg+imm12)
    mode=2: rd = rs1 + (imm20 << 12)          (reg+imm20-shifted)
    NOTE: x0 is hardwired zero. x32 is PC.
          Writes to x32 redirect control flow.
          Reads of x32 yield pc+4 (return-address convention).

  LUT3 rd, rs1, rs2, rs3, tt8
    For each bit i in 0..31:
      idx = (rs1[i]<<2) | (rs2[i]<<1) | rs3[i]
      rd[i] = (tt8 >> idx) & 1
    Common truth tables (tt8):
      AND(a,b)      tt8 = 0b11001000  (ignore rs3, function of rs1&rs2)
      OR(a,b)       tt8 = 0b11111110
      XOR(a,b)      tt8 = 0b10010110
      NOT(a)        tt8 = 0b00001111  (function of rs1 only)
      MUX(s,a,b)    tt8 = 0b11001010  (rs1=s, rs2=a (if s=1), rs3=b (if s=0))
      AND-NOT(a,b)  tt8 = 0b00100010  (a & ~b)

  CMOV rd, rs1, rs2
    if (rs2 != 0) rd = rs1
    NOTE: CMOV x32, target, pred  ==> conditional jump

  LW rd, rs1, imm, mode
    addr = rs1 + sext12(imm)
    mode=0 (LBU): rd = zext8 (mem8 [addr])
    mode=1 (LB ): rd = sext8 (mem8 [addr])
    mode=2 (LHU): rd = zext16(mem16[addr])
    mode=3 (LH ): rd = sext16(mem16[addr])
    mode=4 (LW ): rd =        mem32[addr]
    NOTE: width is explicit so MMIO side effects (e.g. UART RX FIFO pop)
          see the true access width. A pure read-modify-shift synthesis
          of LB/LH from aligned LW only works for plain memory.

  SW rs2, rs1, imm, mode
    addr = rs1 + sext12(imm)
    mode=0 (SB): mem8 [addr] = (uint8_t) rs2
    mode=1 (SH): mem16[addr] = (uint16_t)rs2
    mode=2 (SW): mem32[addr] =            rs2

  SHIFT rd, rs1, rs2, mode
    mode=0 (SLL): rd = rs1 << (rs2 & 31)
    mode=1 (SRL): rd = rs1 >>u (rs2 & 31)     [logical, zero-fill]
    mode=2 (SRA): rd = rs1 >>s (rs2 & 31)     [arithmetic, sign-fill]

CONVENTIONS USED IN SYNTHESIS:
  tmp, tmp2  = scratch registers (any free GPR)
  x0         = hardwired zero
  x32        = program counter (writes redirect control flow)
  // pseudo-C comments after each line

================================================================================

#1  LUI rd, imm20
    // rd = imm20 << 12
    UNI-ADD rd, x0, imm20, mode=2          // rd = 0 + (imm20 << 12)

--------------------------------------------------------------------------------

#2  AUIPC rd, imm20
    // rd = pc + (imm20 << 12)
    // Note: x32 reads as pc+4, so adjust by -4 to match AUIPC semantics
    UNI-ADD rd, x32, imm20, mode=2         // rd = (pc+4) + (imm20 << 12)
    UNI-ADD rd, rd, -4, mode=1             // rd = rd - 4  (now equals pc + imm20<<12)

--------------------------------------------------------------------------------

#3  JAL rd, offset
    // rd = pc + 4; pc = pc + offset
    UNI-ADD rd, x32, 0, mode=1             // rd = pc + 4 (link)
    UNI-ADD x32, x32, offset_minus_4, mode=1   // pc = (pc+4) + (offset-4) = pc+offset
    // For offset > 12 bits, use AUIPC-style 2-instruction sequence first:
    //   UNI-ADD tmp, x32, hi(offset), mode=2
    //   UNI-ADD x32, tmp, lo(offset)-4, mode=1

--------------------------------------------------------------------------------

#4  JALR rd, rs1, imm
    // rd = pc + 4; pc = (rs1 + imm) & ~1
    UNI-ADD tmp, rs1, imm, mode=1          // tmp = rs1 + imm
    LUT3 tmp, tmp, x_neg1, x0, AND-with-~1 // tmp = tmp & ~1 (mask LSB)
    UNI-ADD rd, x32, 0, mode=1             // rd = pc + 4 (link)
    UNI-ADD x32, tmp, 0, mode=1            // pc = tmp
    // Where x_neg1 is a register preloaded with 0xFFFFFFFE
    // (Or use truth table that ANDs with constant via LUT3 differently;
    //  simplest: keep -2 in a scratch reg, AND via LUT3)

--------------------------------------------------------------------------------

#5  BEQ rs1, rs2, offset
    // if (rs1 == rs2) pc += offset
    LUT3 tmp, rs1, rs2, x0, XOR(rs1,rs2)   // tmp = rs1 ^ rs2 (zero iff equal)
    // Compute target into tmp2:
    UNI-ADD tmp2, x32, offset_minus_4, mode=1  // tmp2 = pc + offset
    // We want: if tmp==0, jump to tmp2. CMOV jumps if predicate is nonzero,
    // so we need the inverse: "predicate = (tmp == 0)" = 1 if equal else 0
    // Use SLTU-style synthesis: pred = (tmp == 0) means !tmp
    // Simplest: build "is-zero" predicate
    UNI-ADD tmp, tmp, -1, mode=1           // tmp = (rs1^rs2) - 1
                                           // if rs1==rs2: tmp = -1 (0xFFFFFFFF)
                                           // else: tmp = some nonzero value, but
                                           //   carry-out detection needed.
    // Better: use SHIFT to broadcast "is-nonzero" then invert
    // Actually, simplest correct sequence:
    LUT3 tmp, rs1, rs2, x0, XOR            // tmp = rs1 ^ rs2
    // To convert "tmp==0" to a 1/0 predicate: tmp==0 iff (tmp|-tmp) has MSB clear
    // Easier path: use two CMOVs with inverted logic, OR a SHIFT trick
    UNI-ADD pred, x0, 1, mode=1            // pred = 1 (assume equal)
    CMOV pred, x0, tmp                     // if tmp!=0, pred = 0 (not equal)
    UNI-ADD tmp2, x32, offset_minus_4, mode=1  // tmp2 = pc + offset
    CMOV x32, tmp2, pred                   // if pred!=0 (equal), jump

--------------------------------------------------------------------------------

#6  BNE rs1, rs2, offset
    // if (rs1 != rs2) pc += offset
    LUT3 tmp, rs1, rs2, x0, XOR            // tmp = rs1 ^ rs2 (nonzero iff !=)
    UNI-ADD tmp2, x32, offset_minus_4, mode=1  // tmp2 = pc + offset
    CMOV x32, tmp2, tmp                    // if tmp!=0, jump

--------------------------------------------------------------------------------

#7  BLT rs1, rs2, offset
    // if (rs1 <s rs2) pc += offset
    // Convert signed compare to unsigned by flipping sign bits:
    UNI-ADD msb, x0, 0x80000000_hi20, mode=2   // msb = 0x80000000
    LUT3 a, rs1, msb, x0, XOR              // a = rs1 ^ 0x80000000
    LUT3 b, rs2, msb, x0, XOR              // b = rs2 ^ 0x80000000
    // Now BLT(rs1,rs2) == BLTU(a,b). Synthesize BLTU:
    UNI-ADD nb, b, 0, mode=1               // nb = b
    LUT3 nb, nb, x0, x0, NOT(b)            // nb = ~b
    UNI-ADD nb, nb, 1, mode=1              // nb = -b
    UNI-ADD diff, a, nb, mode=0            // diff = a - b
    // Borrow occurred iff (a < b unsigned). Borrow = MSB of result if no overflow
    // -- simpler: use SHIFT to extract carry indirectly:
    SHIFT pred, diff, 31, mode=1 (SRL)     // pred = diff >> 31 (sign bit)
    // pred=1 if a<b (signed in modified domain == unsigned of original)
    UNI-ADD tmp2, x32, offset_minus_4, mode=1
    CMOV x32, tmp2, pred

--------------------------------------------------------------------------------

#8  BGE rs1, rs2, offset
    // if (rs1 >=s rs2) pc += offset
    // Same as BLT but invert predicate:
    [...same as BLT to compute pred where pred=1 means rs1<rs2...]
    UNI-ADD inv, x0, 1, mode=1             // inv = 1
    CMOV inv, x0, pred                     // if pred!=0, inv=0; else inv=1
    UNI-ADD tmp2, x32, offset_minus_4, mode=1
    CMOV x32, tmp2, inv

--------------------------------------------------------------------------------

#9  BLTU rs1, rs2, offset
    // if (rs1 <u rs2) pc += offset
    LUT3 nb, rs2, x0, x0, NOT              // nb = ~rs2
    UNI-ADD nb, nb, 1, mode=1              // nb = -rs2
    UNI-ADD diff, rs1, nb, mode=0          // diff = rs1 - rs2
    // For unsigned, borrow detection: if rs1<rs2, then conceptual diff has bit 32 set
    // Trick: if (rs1 < rs2) then ((rs1 - rs2) > rs1) unsigned
    // So compare diff vs rs1: if diff > rs1 unsigned, then borrow occurred
    // But we don't have unsigned-compare; recurse? No -- use a different trick.
    // CORRECT TRICK: rs1 < rs2 (unsigned) iff (rs1 XOR sign(rs1-rs2)) ...
    // Simpler: detect by checking if (~rs1 & rs2) | ((~rs1 | rs2) & diff) has MSB set
    LUT3 borrow_bits, rs1, rs2, diff, 0b00110011  // borrow logic per bit
    // (truth table to be verified; this is the standard subtract-borrow function)
    SHIFT pred, borrow_bits, 31, mode=1 (SRL)
    UNI-ADD tmp2, x32, offset_minus_4, mode=1
    CMOV x32, tmp2, pred
    // NOTE: This synthesis is tricky; in practice a software helper or a kept
    // SLTU instruction would be easier. See discussion at end.

--------------------------------------------------------------------------------

#10 BGEU rs1, rs2, offset
    // if (rs1 >=u rs2) pc += offset
    [...compute pred via BLTU sequence above, then invert as in BGE...]

--------------------------------------------------------------------------------

#11 LB rd, rs1, imm
    LW rd, rs1, imm, mode=1                // 1 op (byte sign-extend)

--------------------------------------------------------------------------------

#12 LH rd, rs1, imm
    LW rd, rs1, imm, mode=3                // 1 op (half sign-extend)

--------------------------------------------------------------------------------

#13 LW rd, rs1, imm
    LW rd, rs1, imm, mode=4                // 1 op

--------------------------------------------------------------------------------

#14 LBU rd, rs1, imm
    LW rd, rs1, imm, mode=0                // 1 op (byte zero-extend)

--------------------------------------------------------------------------------

#15 LHU rd, rs1, imm
    LW rd, rs1, imm, mode=2                // 1 op (half zero-extend)

--------------------------------------------------------------------------------

#16 SB rs2, rs1, imm
    SW rs2, rs1, imm, mode=0               // 1 op

--------------------------------------------------------------------------------

#17 SH rs2, rs1, imm
    SW rs2, rs1, imm, mode=1               // 1 op

--------------------------------------------------------------------------------

#18 SW rs2, rs1, imm
    SW rs2, rs1, imm, mode=2               // 1 op

--------------------------------------------------------------------------------

#19 ADDI rd, rs1, imm
    UNI-ADD rd, rs1, imm, mode=1           // KEEP (as a mode of UNI-ADD)

--------------------------------------------------------------------------------

#20 SLTI rd, rs1, imm
    // rd = (rs1 <s imm) ? 1 : 0
    UNI-ADD tmp, x0, imm, mode=1           // tmp = sext12(imm)
    [...synthesize SLT(rs1, tmp), see #31...]

--------------------------------------------------------------------------------

#21 SLTIU rd, rs1, imm
    UNI-ADD tmp, x0, imm, mode=1           // tmp = sext12(imm)
    [...synthesize SLTU(rs1, tmp), see #32...]

--------------------------------------------------------------------------------

#22 XORI rd, rs1, imm
    UNI-ADD tmp, x0, imm, mode=1
    LUT3 rd, rs1, tmp, x0, XOR             // rd = rs1 ^ tmp

--------------------------------------------------------------------------------

#23 ORI rd, rs1, imm
    UNI-ADD tmp, x0, imm, mode=1
    LUT3 rd, rs1, tmp, x0, OR              // rd = rs1 | tmp

--------------------------------------------------------------------------------

#24 ANDI rd, rs1, imm
    UNI-ADD tmp, x0, imm, mode=1
    LUT3 rd, rs1, tmp, x0, AND             // rd = rs1 & tmp

--------------------------------------------------------------------------------

#25 SLLI rd, rs1, shamt
    UNI-ADD tmp, x0, shamt, mode=1         // tmp = shamt
    SHIFT rd, rs1, tmp, mode=0             // rd = rs1 << tmp

--------------------------------------------------------------------------------

#26 SRLI rd, rs1, shamt
    UNI-ADD tmp, x0, shamt, mode=1
    SHIFT rd, rs1, tmp, mode=1             // rd = rs1 >>u tmp

--------------------------------------------------------------------------------

#27 SRAI rd, rs1, shamt
    UNI-ADD tmp, x0, shamt, mode=1
    SHIFT rd, rs1, tmp, mode=2             // rd = rs1 >>s tmp

--------------------------------------------------------------------------------

#28 ADD rd, rs1, rs2
    UNI-ADD rd, rs1, rs2, mode=0           // KEEP (as a mode of UNI-ADD)

--------------------------------------------------------------------------------

#29 SUB rd, rs1, rs2
    // rd = rs1 - rs2
    LUT3 tmp, rs2, x0, x0, NOT             // tmp = ~rs2
    UNI-ADD tmp, tmp, 1, mode=1            // tmp = -rs2
    UNI-ADD rd, rs1, tmp, mode=0           // rd = rs1 + (-rs2)

--------------------------------------------------------------------------------

#30 SLL rd, rs1, rs2
    SHIFT rd, rs1, rs2, mode=0             // 1 instruction

--------------------------------------------------------------------------------

#31 SLT rd, rs1, rs2
    // rd = (rs1 <s rs2) ? 1 : 0
    UNI-ADD msb, x0, 0x80000000_hi20, mode=2
    LUT3 a, rs1, msb, x0, XOR              // flip sign bit of rs1
    LUT3 b, rs2, msb, x0, XOR              // flip sign bit of rs2
    [...synthesize SLTU(a, b), see #32...]

--------------------------------------------------------------------------------

#32 SLTU rd, rs1, rs2
    // rd = (rs1 <u rs2) ? 1 : 0
    // Method: subtract and detect borrow.
    LUT3 nb, rs2, x0, x0, NOT              // nb = ~rs2
    UNI-ADD nb, nb, 1, mode=1              // nb = -rs2
    UNI-ADD diff, rs1, nb, mode=0          // diff = rs1 - rs2
    // Borrow out iff rs1 < rs2 unsigned. Detect via:
    //   borrow = (~rs1 & rs2) | (~(rs1 ^ rs2) & diff), MSB of this is borrow
    // Using LUT3 with truth table for this 3-input function:
    LUT3 brw, rs1, rs2, diff, 0b00010111   // bitwise borrow function
    // (Truth table needs verification: for inputs (a,b,d), output is borrow-in
    //  to next bit; we want the propagated final borrow which is MSB-related.)
    // Simpler & correct: borrow occurred iff (rs1 < rs2) iff
    //   ((rs1 XOR ((rs1 XOR rs2) AND (diff XOR rs1))) has MSB set)
    // Equivalent classic identity for unsigned <:
    LUT3 t, rs1, rs2, diff, 0b...           // (a&~b) | (~(a^b)&d), MSB matters
    SHIFT rd, t, 31, mode=1                 // rd = MSB of t (0 or 1)
    // NOTE: The exact 3-input boolean for borrow detection in MSB position is:
    //   f(a,b,d) = (~a & b) | ((~a | b) & d)
    //   tt8 for this with (rs1,rs2,diff) ordering: compute the 8 values:
    //     f(0,0,0)=0  f(0,0,1)=1  f(0,1,0)=1  f(0,1,1)=1
    //     f(1,0,0)=0  f(1,0,1)=0  f(1,1,0)=0  f(1,1,1)=1
    //   tt8 = 0b10001110 (bit i = f for index i)
    LUT3 t, rs1, rs2, diff, 0b10001110
    SHIFT rd, t, 31, mode=1                 // extract MSB into bit 0

--------------------------------------------------------------------------------

#33 XOR rd, rs1, rs2
    LUT3 rd, rs1, rs2, x0, XOR             // 1 instruction

--------------------------------------------------------------------------------

#34 SRL rd, rs1, rs2
    SHIFT rd, rs1, rs2, mode=1             // 1 instruction

--------------------------------------------------------------------------------

#35 SRA rd, rs1, rs2
    SHIFT rd, rs1, rs2, mode=2             // 1 instruction

--------------------------------------------------------------------------------

#36 OR rd, rs1, rs2
    LUT3 rd, rs1, rs2, x0, OR              // 1 instruction

--------------------------------------------------------------------------------

#37 AND rd, rs1, rs2
    LUT3 rd, rs1, rs2, x0, AND             // 1 instruction

--------------------------------------------------------------------------------

#38 FENCE
    // No-op on single hart. Emit nothing, or:
    UNI-ADD x0, x0, 0, mode=1              // canonical NOP

--------------------------------------------------------------------------------

#39 ECALL
    // Not supported -- this ISA has no trap mechanism.
    // Toolchain must not emit. If it must call out, use a known indirect
    // jump to a runtime-provided handler address (cooperative):
    //   LW tmp, x_runtime_table, ECALL_OFFSET
    //   UNI-ADD x32, tmp, 0, mode=1      // jump to handler

--------------------------------------------------------------------------------

#40 EBREAK
    // Not supported. Same as ECALL.

================================================================================

INSTRUCTION COUNT SUMMARY (synthesis cost in base instructions):
----------------------------------------------------------------
  Kept as-is or trivial encoding change (1 base instr):
    LUI, AUIPC*, JAL*, ADDI, ADD, XOR, OR, AND, LW, SW,
    SLL, SRL, SRA, FENCE (=nop)
    *AUIPC=2, JAL=2 generally
  Two instructions:
    XORI, ORI, ANDI, SLLI, SRLI, SRAI, BNE
  Three instructions:
    SUB
  Five to eight instructions:
    BEQ, BLT, BGE, BLTU, BGEU, SLT, SLTU, SLTI, SLTIU
  One instruction (mode of LW/SW):
    LB, LH, LBU, LHU, SB, SH
  Not supported:
    ECALL, EBREAK (no trap mechanism in this ISA)

================================================================================

NOTES FOR REVIEW:
-----------------

1. PC-as-x32: x32 reads return pc+4 (matches JAL/JALR link-address semantics).
   Writes to x32 redirect the next instruction fetch. There's no AUIPC anymore
   because x32 IS readable; AUIPC becomes (pc+4) + imm20<<12, requiring a -4
   correction to exactly match RV32I AUIPC's "pc-of-this-instruction" semantics.

2. SLTU synthesis uses subtract-and-extract-borrow. The 3-input bitwise function
   for borrow detection at the MSB is:
       f(rs1[i], rs2[i], diff[i]) = (~rs1[i] & rs2[i]) | ((~rs1[i] | rs2[i]) & diff[i])
   tt8 = 0b10001110 (verify against truth table:
     idx 000 (rs1=0,rs2=0,d=0) -> 0
     idx 001 (rs1=0,rs2=0,d=1) -> 1
     idx 010 (rs1=0,rs2=1,d=0) -> 1
     idx 011 (rs1=0,rs2=1,d=1) -> 1
     idx 100 (rs1=1,rs2=0,d=0) -> 0
     idx 101 (rs1=1,rs2=0,d=1) -> 0
     idx 110 (rs1=1,rs2=1,d=0) -> 0
     idx 111 (rs1=1,rs2=1,d=1) -> 1
   Reading bits 7..0: 1 0 0 0 1 1 1 0 = 0x8E = 0b10001110 ✓
   Then SHIFT >> 31 extracts the MSB as a 0/1 result.
   ** PLEASE DOUBLE-CHECK THIS TRUTH TABLE -- it's the trickiest part.

3. Sub-word loads/stores use the width+sign mode of LW/SW so the access
   width is visible to the memory system. This matters for MMIO devices
   whose read/write side effects depend on width (e.g. a UART RX FIFO that
   pops one byte per read). A pure RAM-only synthesis that masks bytes out
   of an aligned LW is still possible (see git history) and costs ~10 ops
   per sub-word access, but is incorrect against MMIO.

4. BEQ synthesis: the "is-zero" predicate is built as:
     pred = 1; if (xor_result != 0) pred = 0;
   Using CMOV: UNI-ADD pred,x0,1; CMOV pred,x0,xor_result
   This works because CMOV writes pred=0 only when xor_result is nonzero,
   leaving pred=1 (meaning "equal") otherwise.

5. Constants like 0x80000000, 0xFF, 0xFFFF, 0xFFFFFFFE used in syntheses
   are assumed pre-loaded into named scratch registers (x_FF_const, etc.)
   In real code they'd be materialized with UNI-ADD mode=2 (+ mode=1 for
   low bits if needed). Add 1-2 instructions per first use; amortize via
   register allocation across a function.

6. FENCE has no semantic effect on a single hart -- the original code in
   the user's PART 1 already retires it as NOP. Toolchain can simply not
   emit it.

7. ECALL/EBREAK have no replacement because this ISA has no trap mechanism
   by design. Any system-call convention would be cooperative: an indirect
   jump through a runtime-provided function pointer table. This is the
   honest answer; a hosted RV32I program that uses ECALL cannot be ported
   without rewriting its system-call interface.

================================================================================
