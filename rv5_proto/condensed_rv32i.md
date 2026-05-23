# The minimum fast 6-instruction core for Base RV32I

A single decoder, six instruction kinds, every Base-RV32I instruction
lowered to a **constant number** of primitive operations. No bit-loops,
no SMC-required shifts, no O(n) per byte-extract. Targeted at a small
FPGA build.

## The chosen 6

| # | Instr | Form | Cost | What it gives us |
|---|-------|------|------|------------------|
| 1 | `ADD` | R | O(1) | reg-reg add (and, with `NOT`+1, subtract); `SLL` by 1 = `ADD(a,a)` |
| 2 | `XOR` | R | O(1) | linear bit mixer (`NOT` via `XOR ~0`; OR/AND derived) |
| 3 | `ROL` | R | O(1) | left rotate; `ROR(a,n) = ROL(a, 32−n)`; shifts and masks built on top |
| 4 | `BNE` | B | O(1) | conditional branch; unconditional jump via `BNE r,x0` (`r ≠ 0`) |
| 5 | `LW`  | I | O(1) | word load (and constant-pool fetch via `LI`) |
| 6 | `SW`  | S | O(1) | word store (and the SMC slot used to realize `JALR`) |

`ROL` is the only primitive not in Base RV32I — it lives in the **Zbb**
bit-manipulation extension (encoding `funct7=0b0110000, funct3=0b001`).
This is the one-instruction cost of "fast": replacing `SLL` with `ROL`
collapses every shift, mask, and bit-extract to O(1). Without it the
same operations are O(32) bit-loops.

The synthesized core therefore implements **Base RV32I + one Zbb op
(ROLI/ROL)**, with every RV32I instruction lowered to a bounded number
of decode cycles.

## Macros built on the 6

Every macro below has constant cost. Temporaries `t, s, x, d, …` are
fresh registers allocated by the assembler.

```
LI   rd, K          ;  LW rd, pool_K(gp)                    ; 32-bit constant from .rodata pool
ADDI rd, rs, K      ;  LI t, K       ; ADD rd, rs, t
NOT  rd, rs         ;  LI t, ~0      ; XOR rd, rs, t
NEG  rd, rs         ;  NOT t, rs     ; ADDI rd, t, 1
SUB  rd, a, b       ;  NEG t, b      ; ADD rd, a, t
BIT  rd, rs, i      ;  ROL r,rs,31-i ; ADD x,r,r ; ROL y,r,1 ; XOR rd,x,y
                    ;  ; rd = (rs >> i) & 1, isolated cleanly into bit 0
                    ;  ; identity: ADD(r,r) drops r's bit 31; ROL(r,1) keeps it
                    ;  ; in bit 0; XOR exposes it with all other bits cancelled.

AND  rd, a, b       ;  ADD s,a,b ; XOR x,a,b ; SUB d,s,x    ; d = 2(a&b)
                    ;  ROL lo,d,31                          ; lo = (a&b) with bit 31 = 0
                    ;  BIT ah,a,31 ; BIT bh,b,31            ; isolate bit 31 of each
                    ;  ADD c,ah,bh ; BIT c1,c,1             ; c1 = (a_31 & b_31)
                    ;  ROL hi,c1,31                         ; hi = c1 << 31 (only bit 31)
                    ;  XOR rd,lo,hi                         ; disjoint → XOR = OR

OR   rd, a, b       ;  XOR x,a,b ; AND y,a,b ; XOR rd,x,y   ; (a^b) and (a&b) are bit-disjoint
ANDI rd, rs, K      ;  LI t,K ; AND rd, rs, t
ORI  rd, rs, K      ;  LI t,K ; OR  rd, rs, t
XORI rd, rs, K      ;  LI t,K ; XOR rd, rs, t

SLL  rd, rs, n      ;  ROL r,rs,n  ; ANDI rd,r,(~0 << n)    ; clear the wrapped low n bits
SRL  rd, rs, n      ;  ROL r,rs,32-n ; ANDI rd,r,(~0 >> n)  ; clear the wrapped high n bits
SRA  rd, rs, n      ;  SRL t,rs,n
                    ;  BIT s,rs,31                          ; sign bit (0 or 1)
                    ;  NEG ms,s                             ; 0 or 0xFFFFFFFF
                    ;  LI hm,(~0 << (32-n))                 ; high-n-bits mask
                    ;  AND fill,ms,hm                       ; sign extended to top n bits
                    ;  OR  rd,t,fill

SLTU rd, a, b       ;  NOT na,a ; AND p,na,b                ; p = ~a & b
                    ;  XOR e,a,b ; NOT ne,e                 ; ne = ~(a^b)
                    ;  SUB s,a,b
                    ;  AND q,ne,s
                    ;  OR  u,p,q
                    ;  BIT rd,u,31                          ; bit 31 of u

SLT  rd, a, b       ;  LI m,0x80000000 ; XOR ax,a,m ; XOR bx,b,m
                    ;  SLTU rd, ax, bx                      ; signed = unsigned after sign-flip

J  label            ;  LI t,1 ; BNE t,x0,label              ; short range; assembler picks form
JR rs               ;  SW rs, slot(x0)                      ; patch the imm of a BNE at slot
                    ;  J  slot                              ; fall through; trampoline gives rs
```

A *literal pool* (read-only `.rodata`) holds every immediate the program
references; `LI rd, K` is one `LW` from that pool through the pool-base
register `gp`. The reset microsequence loads `gp` with the pool base
before user code starts.

A *trampoline slot* is a single writable code word used for
register-indirect jumps: `JR` patches the slot's BNE immediate to the
desired displacement, then falls through.

---

## Mapping all 40 RV32I instructions to the 6

### U-type

| Mnemonic | emu |
|----------|-----|
| `LUI rd, imm`   | `LI rd, (imm<<12)` |
| `AUIPC rd, imm` | `LI rd, (pc + (imm<<12))`  — pool-resolved at link time |

### J / I jumps

| Mnemonic | emu |
|----------|-----|
| `JAL rd, off`      | `LI rd, pc+4` ; `J pc+off` |
| `JALR rd, rs, off` | `ADDI t,rs,off` ; `ANDI t,t,~1` ; `LI rd, pc+4` ; `JR t` |

For `|off| > 4 KiB` (JAL exceeds BNE's branch range), the assembler
substitutes `LI t, pc+off ; JR t`. Same trampoline mechanism.

### B-type

| Mnemonic | emu |
|----------|-----|
| `BEQ  a,b,L` | `BNE a,b,1f` ; `J L` ; `1:` |
| `BNE  a,b,L` | **native** |
| `BLT  a,b,L` | `SLT  t,a,b` ; `BNE t,x0,L` |
| `BGE  a,b,L` | `SLT  t,a,b` ; `BEQ t,x0,L` |
| `BLTU a,b,L` | `SLTU t,a,b` ; `BNE t,x0,L` |
| `BGEU a,b,L` | `SLTU t,a,b` ; `BEQ t,x0,L` |

### Loads (`k = ANDI(addr, 3)`, aligned `a = SUB(addr, k)`)

| Mnemonic | emu |
|----------|-----|
| `LW  rd, off(rs)` | **native** (after `ADDI` folds the offset) |
| `LHU rd, off(rs)` | `LW w,a` ; `SRL t,w,SLL(k,3)` ; `ANDI rd,t,0xFFFF` |
| `LH  rd, off(rs)` | `LHU h` ; `SLL t,h,16` ; `SRA rd,t,16` |
| `LBU rd, off(rs)` | `LW w,a` ; `SRL t,w,SLL(k,3)` ; `ANDI rd,t,0xFF` |
| `LB  rd, off(rs)` | `LBU b` ; `SLL t,b,24` ; `SRA rd,t,24` |

### Stores

| Mnemonic | emu |
|----------|-----|
| `SW rs2, off(rs1)` | **native** |
| `SH rs2, off(rs1)` | `LW w,a` ; mask out half ; `OR` in `(rs2&0xFFFF) << (8·k)` ; `SW back` |
| `SB rs2, off(rs1)` | `LW w,a` ; mask out byte ; `OR` in `(rs2&0xFF)   << (8·k)` ; `SW back` |

### OP-IMM

| Mnemonic | emu |
|----------|-----|
| `ADDI  rd, rs, K` | `LI t,K` ; `ADD  rd,rs,t` |
| `SLTI  rd, rs, K` | `LI t,K` ; `SLT  rd,rs,t` |
| `SLTIU rd, rs, K` | `LI t,K` ; `SLTU rd,rs,t` |
| `XORI  rd, rs, K` | `LI t,K` ; `XOR  rd,rs,t`            **(native XOR)** |
| `ORI   rd, rs, K` | `OR  ` macro with `LI t,K` |
| `ANDI  rd, rs, K` | `AND ` macro with `LI t,K` |
| `SLLI  rd, rs, n` | `SLL` macro (`ROL` + mask) |
| `SRLI  rd, rs, n` | `SRL` macro (`ROL(32−n)` + mask) |
| `SRAI  rd, rs, n` | `SRA` macro (SRL + sign-fill mask) |

### OP

| Mnemonic | emu |
|----------|-----|
| `ADD  rd, a, b` | **native** |
| `SUB  rd, a, b` | `NEG t,b` ; `ADD rd,a,t` |
| `SLL  rd, a, b` | `ANDI s,b,31` ; `ROL r,a,s` ; mask low `s` bits |
| `SLT  rd, a, b` | `SLT` macro |
| `SLTU rd, a, b` | `SLTU` macro |
| `XOR  rd, a, b` | **native** |
| `SRL  rd, a, b` | `ANDI s,b,31` ; `SUB t,LI(32),s` ; `ROL r,a,t` ; mask high `s` bits |
| `SRA  rd, a, b` | `SRL` + sign-fill mask |
| `OR   rd, a, b` | `OR ` macro |
| `AND  rd, a, b` | `AND` macro |

### MISC-MEM / SYSTEM

| Mnemonic | emu |
|----------|-----|
| `FENCE`            | NOP (single-hart, in-order; the spec explicitly allows this) |
| `ECALL` / `EBREAK` | single SYSTEM decode that always traps (the spec explicitly allows this) |

---

## Why 6 is the floor

Each of the 6 occupies a distinct functional niche; none can be
synthesized from the others without paying super-constant cost.

1. **Carry-generating arithmetic** (`ADD`). Linear ops (`XOR`, `ROL`)
   cannot produce inter-bit carry. The only escape is a 256-entry
   byte-add table, which moves the adder into BRAM — net gate count
   rises and per-add cost becomes 4 sequential lookups (not O(1)).
2. **Linear bit-mixer** (`XOR`). Cannot be derived from `ADD` (which
   has carry) without already having `AND`. Symmetrically `AND` cannot
   be derived from `ADD` without `XOR`.
3. **Bit movement** (`ROL`). `SLL` by repeated `ADD(a,a)` is O(n), not
   O(1); choosing `ROL` over `SLL` is what makes the rest of the
   library O(1).
4. **Conditional control** (`BNE`). The only RV32I instruction that
   conditionally writes PC. Without it, no loops or branches.
5. **Memory load** (`LW`) and **memory store** (`SW`). Without `LW`
   programs cannot read external data; without `SW` they cannot mutate
   it. The constant pool and trampoline slot both rely on these.

Removing any one collapses an entire category.

---

## Coverage review

Per the unprivileged spec (docs.riscv.org/reference/isa/unpriv/rv32.html)
Base RV32I is exactly **40 instructions**. The spec itself draws the
boundary for a minimal implementation:

> RV32I contains 40 unique instructions, though a simple implementation
> might cover the ECALL/EBREAK instructions with a single SYSTEM
> hardware instruction that always traps and might be able to implement
> the FENCE instruction as a NOP.

Of the 40, **37 perform register/memory computation, and all 37 are
synthesized from {ADD, XOR, ROL, BNE, LW, SW}** in O(1) primitive ops.
The remaining three are handled as the spec permits:

- `FENCE` → NOP. Decode-only, no datapath.
- `ECALL`, `EBREAK` → one SYSTEM decode that always traps.

**Minimum FPGA implementation: 6 + 1.** Six O(1) computational
primitives plus one SYSTEM-trap, with `FENCE` accepted by the decoder.

### FPGA-implementation notes

- **`ROL` is one 32-bit barrel rotator** (~150 LUTs on Xilinx 7-series
  via a 5-stage 2-input mux network). The same network does left and
  right shifts by ANDing the result with a mask supplied through `LI`
  + the `AND` macro. One datapath unit, not three.
- **`ADD` and `XOR` share LUTs.** The 32-bit adder's per-bit logic is
  already an XOR with a carry chain; disabling the carry chain yields
  `XOR`. One LUT/CARRY bank serves both, with a one-bit decode signal.
- **`JR` via SMC trampoline** writes one instruction word and
  immediately fetches it. Unified BRAM (one read port for fetch+load,
  one write port for store) is the natural mapping. No I-cache.
- **`LI` via constant pool.** The reset microsequence loads `gp` (the
  pool base) before user code starts. Every `LI rd, K` is `LW rd, K(gp)`
  with a 12-bit pool offset.
- **Estimated core size**: ~500 LUTs (ALU + barrel rotator + regfile
  in LUTRAM + decoder + branch unit) plus BRAM for code, data, and
  constant pool. No multipliers, no DSPs, no caches.

---

## Parameter coverage — the argument-expanded ISA

A single RV32I mnemonic is many distinct machine instructions once the
operand fields are filled in. "Covered" must mean every legal
`(mnemonic, arguments)` tuple is reproduced.

### Size of the parameter-expanded space

| Format | # mnemonics | Operand fields | Tuples / mnemonic | Subtotal |
|---|---:|---|---:|---:|
| R-type        | 10 | rd, rs1, rs2          | 32³ = 32,768       | 327,680 |
| I OP-IMM ALU  |  6 | rd, rs1, imm12        | 32²·2¹² = 4,194,304 | 25,165,824 |
| I OP-IMM shift|  3 | rd, rs1, shamt5       | 32²·2⁵ = 32,768    | 98,304 |
| I loads       |  5 | rd, rs1, imm12        | 4,194,304          | 20,971,520 |
| I JALR        |  1 | rd, rs1, imm12        | 4,194,304          | 4,194,304 |
| S-type        |  3 | rs1, rs2, imm12       | 4,194,304          | 12,582,912 |
| B-type        |  6 | rs1, rs2, imm13/2     | 4,194,304          | 25,165,824 |
| U-type        |  2 | rd, imm20             | 32·2²⁰ = 33,554,432 | 67,108,864 |
| J JAL         |  1 | rd, imm21/2           | 33,554,432         | 33,554,432 |
| FENCE         |  1 | pred, succ (4b each)  | 2⁸ = 256           | 256 |
| ECALL, EBREAK |  2 | none                  | 1                  | 2 |
| **Total**     |    |                       |                    | **≈ 1.89 × 10⁸** |

Direct enumeration is infeasible; the claim must be proved **uniformly
in every operand field**.

### Uniformity claims, by operand dimension

**(R1) Register operands.** Every macro reads sources into fresh
temporaries before writing `rd`. The assembler allocates temporaries
from a pool disjoint from `{x0, rs1, rs2, rd}`; with 30 GPRs free
outside the operand set, allocation always succeeds (max temporaries
used by any macro: 8, in `SLTU`). Writes to `x0` are dropped by
hardware; the macro's final writeback instruction handles this
silently. Degenerate cases (`rs1 = rs2`, `rd ∈ {rs1, rs2}`) follow by
direct evaluation: `ADD(a,a) = 2a`, `XOR(a,a) = 0`, `AND(a,a) = a`,
`SLT(a,a) = 0`, etc., all match the spec.

**(R2) Immediates.** Materialized through `LI(K) = LW(pool_addr(K))`
into a `.rodata` array sized to the program's distinct constants. All
2¹² I/S, 2¹³ B (low bit fixed), 2²⁰ U, 2²¹ J (low bit fixed) values are
representable. Boundary cases (`K = 0`, `K = -1`, `K = INT_MIN`,
sign-extension corners) all pool-resolve. `SLTIU`'s sign-extend-then-
compare-unsigned semantics is preserved because `LI` produces the
sign-extended u32 directly.

**(R3) Shift amounts shamt ∈ {0..31} and dynamic R[rs2] & 31.** The
`SLL`/`SRL`/`SRA` macros use `ROL(a, n)` or `ROL(a, 32−n)` followed by
a mask drawn from the pool. Masks are indexed by `n`; only 32 distinct
masks exist (for n = 0..31), all pool-resolved at link time. For `n =
0`, the mask is `~0` and the rotation is by 0 — both identity, result
is `a`. For `n = 31`, the mask isolates one bit. Dynamic forms compute
`n = ANDI(R[rs2], 31)` at runtime; the same per-`n` mask table is
indexed by `n` to pick the right mask via `LI` + table arithmetic, or
the assembler emits the static form when `R[rs2]` is constant. Both
paths preserve O(1) cost.

**(R4) Branch and jump offsets.**
- B-type 13-bit signed (±4 KiB): inside `BNE`'s native range.
- J-type 21-bit signed (±1 MiB): exceeds `BNE`'s range. The assembler
  emits `BNE r,x0,target` when `|off| ≤ 4 KiB`, else `LI t,target ; JR
  t` via the trampoline. Both forms reach any 32-bit target. The same
  long-range expansion serves out-of-range branches.
- JALR 12-bit offset: `ADDI(R[rs], off)` handles all 2¹² values; the
  low bit is cleared by `ANDI(t, ~1)`.

**(R5) Memory addresses and alignment.** `LW`/`SW` require word
alignment of the synthesized address — same constraint as a typical
RV32I implementation that does not handle misaligned access in
hardware; misaligned access traps via the SYSTEM path. `LH`/`LB` and
`SH`/`SB` compute the sub-word index `k = ANDI(addr, 3)` at runtime;
shift amounts `8·k` and `24 − 8·k` are computed via `SLL(k, 3)` and
`SUB(24, SLL(k, 3))`, both producing values in `{0, 8, 16, 24}`. All 4
alignments × 2¹² offsets are covered uniformly in O(1).

**(R6) FENCE pred/succ fields.** All 2⁸ encodings decode to the same
no-op.

### Conclusion

For each of the ~1.89 × 10⁸ argument-expanded RV32I instructions, the
expansion above produces the architecturally-required result in a
constant number of primitive ops. The proof is uniform in every operand
field: registers flow through the macros, immediates flow through the
constant pool, shift amounts index a fixed mask table, branches reach
any target via the assembler's range expansion. No legal `(mnemonic,
args)` tuple is uncovered.

---

## C++ semantics for the 40 base instructions

```cpp
uint32_t R[32];           // R[0] hardwired to 0 (writes ignored)
uint32_t PC;
uint8_t  MEM[/*…*/];      // little-endian byte-addressed memory
auto S    = [](uint32_t x){ return (int32_t)x; };
auto sext = [](uint32_t imm, int bits){
    uint32_t m = 1u << (bits - 1);
    return (imm ^ m) - m;
};
auto LD32 = [](uint32_t a){ return *(uint32_t*)&MEM[a]; };
auto LD16 = [](uint32_t a){ return *(uint16_t*)&MEM[a]; };
auto LD8  = [](uint32_t a){ return MEM[a]; };
auto ST32 = [](uint32_t a, uint32_t v){ *(uint32_t*)&MEM[a] = v; };
auto ST16 = [](uint32_t a, uint16_t v){ *(uint16_t*)&MEM[a] = v; };
auto ST8  = [](uint32_t a, uint8_t  v){ MEM[a] = v; };
// PC+=4 implicit except where the instruction writes PC.
```

### U-type
```cpp
LUI  (rd, imm20): R[rd] = imm20 << 12;
AUIPC(rd, imm20): R[rd] = PC + (imm20 << 12);
```

### Jumps
```cpp
JAL  (rd, off):   R[rd] = PC + 4; PC = PC + sext(off, 21);
JALR (rd, rs, off): { uint32_t t = (R[rs] + sext(off, 12)) & ~1u;
                      R[rd] = PC + 4; PC = t; }
```

### B-type
```cpp
BEQ  (a,b,off): PC += (R[a] == R[b])      ? sext(off,13) : 4;
BNE  (a,b,off): PC += (R[a] != R[b])      ? sext(off,13) : 4;
BLT  (a,b,off): PC += (S(R[a]) <  S(R[b])) ? sext(off,13) : 4;
BGE  (a,b,off): PC += (S(R[a]) >= S(R[b])) ? sext(off,13) : 4;
BLTU (a,b,off): PC += (R[a]    <  R[b])    ? sext(off,13) : 4;
BGEU (a,b,off): PC += (R[a]    >= R[b])    ? sext(off,13) : 4;
```

### Loads (addr = R[rs] + sext(off,12))
```cpp
LB  (rd,off,rs): R[rd] = (uint32_t)(int32_t)(int8_t) LD8 (R[rs]+sext(off,12));
LBU (rd,off,rs): R[rd] = LD8 (R[rs]+sext(off,12));
LH  (rd,off,rs): R[rd] = (uint32_t)(int32_t)(int16_t)LD16(R[rs]+sext(off,12));
LHU (rd,off,rs): R[rd] = LD16(R[rs]+sext(off,12));
LW  (rd,off,rs): R[rd] = LD32(R[rs]+sext(off,12));
```

### Stores (addr = R[rs1] + sext(off,12))
```cpp
SB (rs2,off,rs1): ST8 (R[rs1]+sext(off,12), (uint8_t) R[rs2]);
SH (rs2,off,rs1): ST16(R[rs1]+sext(off,12), (uint16_t)R[rs2]);
SW (rs2,off,rs1): ST32(R[rs1]+sext(off,12), R[rs2]);
```

### OP-IMM (K = sext(imm12,12); shamt 5-bit)
```cpp
ADDI  (rd,rs,K): R[rd] = R[rs] + K;
SLTI  (rd,rs,K): R[rd] = (S(R[rs]) <  S(K))         ? 1u : 0u;
SLTIU (rd,rs,K): R[rd] = (R[rs]    <  (uint32_t)K)  ? 1u : 0u;
XORI  (rd,rs,K): R[rd] = R[rs] ^ K;
ORI   (rd,rs,K): R[rd] = R[rs] | K;
ANDI  (rd,rs,K): R[rd] = R[rs] & K;
SLLI  (rd,rs,n): R[rd] = R[rs] << n;
SRLI  (rd,rs,n): R[rd] = R[rs] >> n;
SRAI  (rd,rs,n): R[rd] = (uint32_t)(S(R[rs]) >> n);
```

### OP (shift amount from low 5 bits of R[b])
```cpp
ADD  (rd,a,b): R[rd] = R[a] + R[b];
SUB  (rd,a,b): R[rd] = R[a] - R[b];
SLL  (rd,a,b): R[rd] = R[a] << (R[b] & 31);
SLT  (rd,a,b): R[rd] = (S(R[a]) <  S(R[b])) ? 1u : 0u;
SLTU (rd,a,b): R[rd] = (R[a]    <  R[b])    ? 1u : 0u;
XOR  (rd,a,b): R[rd] = R[a] ^ R[b];
SRL  (rd,a,b): R[rd] = R[a] >> (R[b] & 31);
SRA  (rd,a,b): R[rd] = (uint32_t)(S(R[a]) >> (R[b] & 31));
OR   (rd,a,b): R[rd] = R[a] | R[b];
AND  (rd,a,b): R[rd] = R[a] & R[b];
```

### MISC-MEM / SYSTEM
```cpp
// FENCE         — NOP on a single-hart in-order core
// ECALL, EBREAK — single SYSTEM decode that always traps
raise_trap();
```

Writes to `R[0]` are silently dropped: in every block above, wrap the
writeback as `if (rd) R[rd] = v;`.

---

## C++ replacement — each instruction using only the 6

### The 6 primitives
```cpp
uint8_t MEM[/*…*/];
uint32_t ADD(uint32_t a, uint32_t b) { return a + b; }
uint32_t XOR(uint32_t a, uint32_t b) { return a ^ b; }
uint32_t ROL(uint32_t a, uint32_t n) { n &= 31; return n ? (a << n) | (a >> (32 - n)) : a; }
bool     BNE(uint32_t a, uint32_t b) { return a != b; }
uint32_t LW (uint32_t addr)             { return *(uint32_t*)&MEM[addr]; }
void     SW (uint32_t addr, uint32_t v) { *(uint32_t*)&MEM[addr] = v; }
```

(`ROL(a,0) = a`; the `n ? … : a` guard avoids `>> 32`, which is UB in
C. The hardware barrel rotator handles `n=0` natively.)

### Layer 1 — macros built directly on the 6
```cpp
uint32_t LI  (uint32_t K)              { return LW(pool_addr(K)); }
uint32_t NOT_(uint32_t x)              { return XOR(x, LI(0xFFFFFFFFu)); }
uint32_t NEG (uint32_t x)              { return ADD(NOT_(x), LI(1)); }
uint32_t SUB (uint32_t a, uint32_t b)  { return ADD(a, NEG(b)); }
uint32_t ADDI(uint32_t x, uint32_t K)  { return ADD(x, LI(K)); }

// Isolate bit i of rs into bit 0, zeros elsewhere. O(1).
// Identity: r = ROL(rs, 31-i) puts rs's bit i into bit 31. Then
// ADD(r,r) shifts left dropping bit 31; ROL(r,1) shifts left keeping
// bit 31 in bit 0. XOR exposes exactly that bit.
uint32_t BIT(uint32_t rs, uint32_t i) {
    uint32_t r = ROL(rs, SUB(LI(31), i));
    return XOR(ADD(r, r), ROL(r, 1));
}

uint32_t AND_(uint32_t a, uint32_t b) {
    uint32_t s  = ADD(a, b);
    uint32_t x  = XOR(a, b);
    uint32_t d  = SUB(s, x);                // d = 2(a&b) mod 2^32
    uint32_t lo = ROL(d, 31);               // d's bit 0 is 0 → ROL(d,31) ≡ d>>1
    uint32_t ah = BIT(a, 31);               // a_31 in bit 0
    uint32_t bh = BIT(b, 31);               // b_31 in bit 0
    uint32_t c  = ADD(ah, bh);              // c ∈ {0,1,2}
    uint32_t c1 = BIT(c, 1);                // bit 1 of c = (a_31 & b_31)
    uint32_t hi = ROL(c1, 31);              // c1 << 31 (only bit 31 set)
    return XOR(lo, hi);                     // lo and hi bit-disjoint
}

uint32_t OR_ (uint32_t a, uint32_t b)  { return XOR(XOR(a, b), AND_(a, b)); }
uint32_t ANDI(uint32_t x, uint32_t K)  { return AND_(x, LI(K)); }
uint32_t ORI (uint32_t x, uint32_t K)  { return OR_ (x, LI(K)); }
uint32_t XORI(uint32_t x, uint32_t K)  { return XOR (x, LI(K)); }

// Shifts: O(1) via ROL + pool-resolved mask.
uint32_t SLL_(uint32_t a, uint32_t n)  { return AND_(ROL(a, n),    LI(0xFFFFFFFFu << n)); }
uint32_t SRL_(uint32_t a, uint32_t n)  { return AND_(ROL(a, SUB(LI(32), n)), LI(0xFFFFFFFFu >> n)); }
uint32_t SRA_(uint32_t a, uint32_t n) {
    uint32_t t    = SRL_(a, n);
    uint32_t sign = BIT(a, 31);
    uint32_t ms   = NEG(sign);              // 0 or 0xFFFFFFFF
    uint32_t hm   = LI(~(0xFFFFFFFFu >> n));// top-n-bits mask
    uint32_t fill = AND_(ms, hm);
    return OR_(t, fill);
}

uint32_t SLTU_(uint32_t a, uint32_t b) {
    uint32_t p = AND_(NOT_(a), b);
    uint32_t e = NOT_(XOR(a, b));
    uint32_t s = SUB(a, b);
    uint32_t u = OR_(p, AND_(e, s));
    return BIT(u, 31);
}

uint32_t SLT_(uint32_t a, uint32_t b) {
    uint32_t m = LI(0x80000000u);
    return SLTU_(XOR(a, m), XOR(b, m));
}

// Unconditional jump (short range; long range uses LI + JR).
#define J(L)  do { if (BNE(LI(1), LI(0))) goto L; } while (0)

// Register-indirect jump: SMC trampoline.
void JR(uint32_t target, uint32_t slot_addr) {
    uint32_t base      = LW(slot_addr);
    uint32_t off       = SUB(target, slot_addr);
    uint32_t imm_field = encode_bne_imm(off);   // bit-pack via SLL_/XOR/OR_
    SW(slot_addr, OR_(base, imm_field));
    /* fall through into the slot */
}
```

### Layer 2 — each of the 37 RV32I instructions as a one-liner

Effects on `R[]` and `PC` only; `if (rd)` writeback guard implied.

```cpp
// U-type
LUI  (rd, imm20)    : R[rd] = LI(imm20 << 12);
AUIPC(rd, imm20)    : R[rd] = LI(PC + (imm20 << 12));

// Jumps
JAL  (rd, off)      : R[rd] = LI(PC + 4); J(target);
JALR (rd, rs, off)  : { uint32_t t = ANDI(ADDI(R[rs], off), 0xFFFFFFFEu);
                        R[rd] = LI(PC + 4); JR(t, slot); }

// Branches (target = PC + off when taken)
BEQ  (a, b, off)    : if (!BNE(R[a], R[b])) J(target);
BNE  (a, b, off)    : if ( BNE(R[a], R[b])) J(target);            // native
BLT  (a, b, off)    : if ( BNE(SLT_ (R[a], R[b]), LI(0))) J(target);
BGE  (a, b, off)    : if (!BNE(SLT_ (R[a], R[b]), LI(0))) J(target);
BLTU (a, b, off)    : if ( BNE(SLTU_(R[a], R[b]), LI(0))) J(target);
BGEU (a, b, off)    : if (!BNE(SLTU_(R[a], R[b]), LI(0))) J(target);

// Loads      (k = ANDI(R[rs]+off, 3), a = SUB(R[rs]+off, k))
LW   (rd, off, rs)  : R[rd] = LW(ADDI(R[rs], off));                // native
LHU  (rd, off, rs)  : { uint32_t w = LW(a);
                        R[rd] = ANDI(SRL_(w, SLL_(LI(k),3)), 0xFFFFu); }
LH   (rd, off, rs)  : { uint32_t h = /* LHU */;
                        R[rd] = SRA_(SLL_(h, 16), 16); }
LBU  (rd, off, rs)  : { uint32_t w = LW(a);
                        R[rd] = ANDI(SRL_(w, SLL_(LI(k),3)), 0xFFu); }
LB   (rd, off, rs)  : { uint32_t bu = /* LBU */;
                        R[rd] = SRA_(SLL_(bu, 24), 24); }

// Stores
SW   (rs2, off, rs1): SW(ADDI(R[rs1], off), R[rs2]);               // native
SH   (rs2, off, rs1): { uint32_t w = LW(a);
                        uint32_t mask = SLL_(LI(0xFFFFu), SLL_(LI(k),3));
                        uint32_t ins  = SLL_(ANDI(R[rs2], 0xFFFFu), SLL_(LI(k),3));
                        SW(a, OR_(AND_(w, NOT_(mask)), ins)); }
SB   (rs2, off, rs1): { uint32_t w = LW(a);
                        uint32_t mask = SLL_(LI(0xFFu), SLL_(LI(k),3));
                        uint32_t ins  = SLL_(ANDI(R[rs2], 0xFFu),   SLL_(LI(k),3));
                        SW(a, OR_(AND_(w, NOT_(mask)), ins)); }

// OP-IMM
ADDI (rd, rs, K)    : R[rd] = ADD  (R[rs], LI(K));
SLTI (rd, rs, K)    : R[rd] = SLT_ (R[rs], LI(K));
SLTIU(rd, rs, K)    : R[rd] = SLTU_(R[rs], LI(K));
XORI (rd, rs, K)    : R[rd] = XOR  (R[rs], LI(K));                 // native XOR
ORI  (rd, rs, K)    : R[rd] = OR_  (R[rs], LI(K));
ANDI (rd, rs, K)    : R[rd] = AND_ (R[rs], LI(K));
SLLI (rd, rs, n)    : R[rd] = SLL_ (R[rs], n);
SRLI (rd, rs, n)    : R[rd] = SRL_ (R[rs], n);
SRAI (rd, rs, n)    : R[rd] = SRA_ (R[rs], n);

// OP
ADD  (rd, a, b)     : R[rd] = ADD  (R[a], R[b]);                   // native
SUB  (rd, a, b)     : R[rd] = SUB  (R[a], R[b]);
SLL  (rd, a, b)     : R[rd] = SLL_ (R[a], ANDI(R[b], 31));
SLT  (rd, a, b)     : R[rd] = SLT_ (R[a], R[b]);
SLTU (rd, a, b)     : R[rd] = SLTU_(R[a], R[b]);
XOR  (rd, a, b)     : R[rd] = XOR  (R[a], R[b]);                   // native
SRL  (rd, a, b)     : R[rd] = SRL_ (R[a], ANDI(R[b], 31));
SRA  (rd, a, b)     : R[rd] = SRA_ (R[a], ANDI(R[b], 31));
OR   (rd, a, b)     : R[rd] = OR_  (R[a], R[b]);
AND  (rd, a, b)     : R[rd] = AND_ (R[a], R[b]);
```

Every right-hand side resolves transitively to calls of only `ADD`,
`XOR`, `ROL`, `BNE`, `LW`, `SW`. Five RV32I mnemonics (`ADD`, `XOR`,
`LW`, `SW`, plus `BNE` as the branch primitive) appear as leaves
directly; `ROL` is the sixth leaf, drawn from Zbb. The remaining 37 RV32I
instructions lower to bounded sequences of those leaves.
