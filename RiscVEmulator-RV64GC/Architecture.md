# RV32I Base Integer Instruction Set Reference

> **Architecture notes:**
> - RV32I has **no flags or condition codes** — zero, carry, overflow, and sign are never set implicitly.
> - All state lives in 32 general-purpose registers `x0–x31` and the `PC`.
> - `x0` is hardwired to 0; writes to it are silently discarded.
> - Every instruction implicitly advances the PC (to `PC+4` or a branch/jump target).

---

## Instruction Formats

RV32I uses six base encoding formats. All instructions are 32 bits wide.

```
R-Type:  [ funct7  | rs2 | rs1 | funct3 | rd  | opcode ]
          31     25  24 20 19 15 14    12 11  7 6      0

I-Type:  [ imm[11:0]       | rs1 | funct3 | rd  | opcode ]
          31             20 19 15 14    12 11  7 6      0

S-Type:  [ imm[11:5] | rs2 | rs1 | funct3 | imm[4:0] | opcode ]
          31       25 24 20 19 15 14    12 11        7 6      0

B-Type:  [ imm[12|10:5] | rs2 | rs1 | funct3 | imm[4:1|11] | opcode ]
          31          25 24 20 19 15 14    12 11           7 6      0

U-Type:  [ imm[31:12]                    | rd  | opcode ]
          31                           12 11  7 6      0

J-Type:  [ imm[20|10:1|11|19:12]         | rd  | opcode ]
          31                           12 11  7 6      0
```

---

## R-Type Instructions (Register-Register)

**Format:** `[ funct7 | rs2 | rs1 | funct3 | rd | opcode ]`

| Bits    | 31–25    | 24–20 | 19–15 | 14–12  | 11–7 | 6–0     |
|---------|----------|-------|-------|--------|------|---------|
| Field   | funct7   | rs2   | rs1   | funct3 | rd   | opcode  |
| Width   | 7 bits   | 5 bits| 5 bits| 3 bits | 5 bits| 7 bits |

All R-type instructions share `opcode = 0110011`.

| Instruction | funct7    | funct3 | Reads        | Writes | Flags |
|-------------|-----------|--------|--------------|--------|-------|
| `ADD`       | `0000000` | `000`  | `rs1`, `rs2` | `rd`   | —     |
| `SUB`       | `0100000` | `000`  | `rs1`, `rs2` | `rd`   | —     |
| `SLL`       | `0000000` | `001`  | `rs1`, `rs2[4:0]` | `rd` | —  |
| `SLT`       | `0000000` | `010`  | `rs1`, `rs2` | `rd` (0 or 1) | — |
| `SLTU`      | `0000000` | `011`  | `rs1`, `rs2` | `rd` (0 or 1) | — |
| `XOR`       | `0000000` | `100`  | `rs1`, `rs2` | `rd`   | —     |
| `SRL`       | `0000000` | `101`  | `rs1`, `rs2[4:0]` | `rd` | — |
| `SRA`       | `0100000` | `101`  | `rs1`, `rs2[4:0]` | `rd` | — |
| `OR`        | `0000000` | `110`  | `rs1`, `rs2` | `rd`   | —     |
| `AND`       | `0000000` | `111`  | `rs1`, `rs2` | `rd`   | —     |

### Binary layout example — `ADD x1, x2, x3`

```
 31      25 24   20 19   15 14  12 11    7 6      0
┌─────────┬───────┬───────┬──────┬───────┬────────┐
│ 0000000 │ 00011 │ 00010 │ 000  │ 00001 │0110011 │
│ funct7  │  rs2  │  rs1  │funct3│  rd   │ opcode │
└─────────┴───────┴───────┴──────┴───────┴────────┘
```

---

## I-Type Instructions (Immediate Arithmetic)

**Format:** `[ imm[11:0] | rs1 | funct3 | rd | opcode ]`

All arithmetic I-type instructions share `opcode = 0010011`.

| Instruction         | imm[11:0]           | funct3 | Reads               | Writes        | Flags |
|---------------------|---------------------|--------|---------------------|---------------|-------|
| `ADDI rd, rs1, imm` | `imm[11:0]`         | `000`  | `rs1`, `imm`        | `rd`          | —     |
| `SLTI rd, rs1, imm` | `imm[11:0]`         | `010`  | `rs1`, `imm`        | `rd` (0 or 1) | —     |
| `SLTIU rd, rs1, imm`| `imm[11:0]`         | `011`  | `rs1`, `imm`        | `rd` (0 or 1) | —     |
| `XORI rd, rs1, imm` | `imm[11:0]`         | `100`  | `rs1`, `imm`        | `rd`          | —     |
| `ORI rd, rs1, imm`  | `imm[11:0]`         | `110`  | `rs1`, `imm`        | `rd`          | —     |
| `ANDI rd, rs1, imm` | `imm[11:0]`         | `111`  | `rs1`, `imm`        | `rd`          | —     |
| `SLLI rd, rs1, shamt`| `0000000\|shamt[4:0]`| `001` | `rs1`, `imm[4:0]`  | `rd`          | —     |
| `SRLI rd, rs1, shamt`| `0000000\|shamt[4:0]`| `101` | `rs1`, `imm[4:0]`  | `rd`          | —     |
| `SRAI rd, rs1, shamt`| `0100000\|shamt[4:0]`| `101` | `rs1`, `imm[4:0]`  | `rd`          | —     |

> For `SLLI`, `SRLI`, `SRAI`: bits `[11:5]` encode the shift qualifier (`0000000` or `0100000`); bits `[4:0]` encode the shift amount (`shamt`).

### Binary layout example — `ADDI x1, x2, 5`

```
 31          20 19   15 14  12 11    7 6      0
┌──────────────┬───────┬──────┬───────┬────────┐
│ 000000000101 │ 00010 │ 000  │ 00001 │0010011 │
│   imm[11:0]  │  rs1  │funct3│  rd   │ opcode │
└──────────────┴───────┴──────┴───────┴────────┘
```

---

## Load Instructions (I-Type)

`opcode = 0000011`

| Instruction          | funct3 | Reads                      | Writes | Flags |
|----------------------|--------|----------------------------|--------|-------|
| `LB rd, imm(rs1)`    | `000`  | `rs1`, `imm`, `MEM[rs1+imm]` | `rd` (sign-ext byte)   | — |
| `LH rd, imm(rs1)`    | `001`  | `rs1`, `imm`, `MEM[rs1+imm]` | `rd` (sign-ext half)   | — |
| `LW rd, imm(rs1)`    | `010`  | `rs1`, `imm`, `MEM[rs1+imm]` | `rd` (word)            | — |
| `LBU rd, imm(rs1)`   | `100`  | `rs1`, `imm`, `MEM[rs1+imm]` | `rd` (zero-ext byte)   | — |
| `LHU rd, imm(rs1)`   | `101`  | `rs1`, `imm`, `MEM[rs1+imm]` | `rd` (zero-ext half)   | — |

### Binary layout example — `LW x1, 8(x2)`

```
 31          20 19   15 14  12 11    7 6      0
┌──────────────┬───────┬──────┬───────┬────────┐
│ 000000001000 │ 00010 │ 010  │ 00001 │0000011 │
│   imm[11:0]  │  rs1  │funct3│  rd   │ opcode │
└──────────────┴───────┴──────┴───────┴────────┘
```

---

## S-Type Instructions (Store)

**Format:** `[ imm[11:5] | rs2 | rs1 | funct3 | imm[4:0] | opcode ]`

`opcode = 0100011`. The 12-bit immediate is split: upper 7 bits in `[31:25]`, lower 5 bits in `[11:7]`.

| Instruction          | funct3 | Reads                   | Writes        | Flags |
|----------------------|--------|-------------------------|---------------|-------|
| `SB rs2, imm(rs1)`   | `000`  | `rs1`, `rs2`, `imm`     | `MEM[rs1+imm]` (byte)  | — |
| `SH rs2, imm(rs1)`   | `001`  | `rs1`, `rs2`, `imm`     | `MEM[rs1+imm]` (half)  | — |
| `SW rs2, imm(rs1)`   | `010`  | `rs1`, `rs2`, `imm`     | `MEM[rs1+imm]` (word)  | — |

### Binary layout example — `SW x3, 8(x2)`

```
 31      25 24   20 19   15 14  12 11    7 6      0
┌─────────┬───────┬───────┬──────┬───────┬────────┐
│ 0000000 │ 00011 │ 00010 │ 010  │ 01000 │0100011 │
│imm[11:5]│  rs2  │  rs1  │funct3│imm[4:0]│opcode │
└─────────┴───────┴───────┴──────┴───────┴────────┘
  imm = 0000000_01000 = 8
```

---

## B-Type Instructions (Branch)

**Format:** `[ imm[12|10:5] | rs2 | rs1 | funct3 | imm[4:1|11] | opcode ]`

`opcode = 1100011`. The 13-bit signed immediate (always even, bit 0 implicit) is scrambled across the word:
- `inst[31]` → `imm[12]`
- `inst[30:25]` → `imm[10:5]`
- `inst[11:8]` → `imm[4:1]`
- `inst[7]` → `imm[11]`

Branch target = `PC + imm`. PC is always written (either to target or `PC+4`).

| Instruction              | funct3 | Reads                       | Writes | Flags |
|--------------------------|--------|-----------------------------|--------|-------|
| `BEQ rs1, rs2, imm`      | `000`  | `rs1`, `rs2`, `imm`, `PC`   | `PC`   | —     |
| `BNE rs1, rs2, imm`      | `001`  | `rs1`, `rs2`, `imm`, `PC`   | `PC`   | —     |
| `BLT rs1, rs2, imm`      | `100`  | `rs1`, `rs2`, `imm`, `PC`   | `PC`   | —     |
| `BGE rs1, rs2, imm`      | `101`  | `rs1`, `rs2`, `imm`, `PC`   | `PC`   | —     |
| `BLTU rs1, rs2, imm`     | `110`  | `rs1`, `rs2`, `imm`, `PC`   | `PC`   | —     |
| `BGEU rs1, rs2, imm`     | `111`  | `rs1`, `rs2`, `imm`, `PC`   | `PC`   | —     |

### Binary layout example — `BEQ x1, x2, +8`

```
 31  30    25 24   20 19   15 14  12 11  8  7  6      0
┌──┬────────┬───────┬───────┬──────┬─────┬──┬────────┐
│0 │000000  │ 00010 │ 00001 │ 000  │0100 │0 │1100011 │
│↑ │imm10:5 │  rs2  │  rs1  │funct3│imm4:1│↑│opcode  │
│12│        │       │       │      │      │11│        │
└──┴────────┴───────┴───────┴──────┴─────┴──┴────────┘
  imm = 0_0_000000_0100_0 = +8
```

---

## U-Type Instructions (Upper Immediate)

**Format:** `[ imm[31:12] | rd | opcode ]`

The 20-bit immediate is placed in the upper 20 bits of `rd`; lower 12 bits are zeroed.

| Instruction     | opcode    | Reads        | Writes | Flags |
|-----------------|-----------|--------------|--------|-------|
| `LUI rd, imm`   | `0110111` | `imm`        | `rd`   | —     |
| `AUIPC rd, imm` | `0010111` | `PC`, `imm`  | `rd`   | —     |

### Binary layout example — `LUI x1, 0x12345`

```
 31                  12 11    7 6      0
┌──────────────────────┬───────┬────────┐
│    00010010001101000101 │ 00001 │0110111 │
│       imm[31:12]     │  rd   │ opcode │
└──────────────────────┴───────┴────────┘
```

---

## J-Type Instructions (Jump)

**Format:** `[ imm[20|10:1|11|19:12] | rd | opcode ]`

`opcode = 1101111` (JAL only; JALR is I-type). The 21-bit signed immediate (bit 0 implicit) is scrambled:
- `inst[31]` → `imm[20]`
- `inst[30:21]` → `imm[10:1]`
- `inst[20]` → `imm[11]`
- `inst[19:12]` → `imm[19:12]`

| Instruction       | opcode    | Reads             | Writes               | Flags |
|-------------------|-----------|-------------------|----------------------|-------|
| `JAL rd, imm`     | `1101111` | `PC`, `imm`       | `rd` (= PC+4), `PC`  | —     |
| `JALR rd, rs1, imm`| `1100111`| `rs1`, `imm`, `PC`| `rd` (= PC+4), `PC`  | —     |

> `JALR` is **I-type**, not J-type. Its target is `(rs1 + imm) & ~1` (bit 0 forced to 0).

### Binary layout example — `JAL x1, +16`

```
 31 30      21 20 19    12 11    7 6      0
┌──┬──────────┬──┬─────────┬───────┬────────┐
│0 │0000001000│0 │00000000 │ 00001 │1101111 │
│↑ │ imm[10:1]│↑ │imm[19:12]│  rd  │ opcode │
│20│          │11│          │      │        │
└──┴──────────┴──┴─────────┴───────┴────────┘
  imm = 0_00000000_0_0000001000_0 = +16
```

### Binary layout example — `JALR x1, x2, 4` (I-type)

```
 31          20 19   15 14  12 11    7 6      0
┌──────────────┬───────┬──────┬───────┬────────┐
│ 000000000100 │ 00010 │ 000  │ 00001 │1100111 │
│   imm[11:0]  │  rs1  │funct3│  rd   │ opcode │
└──────────────┴───────┴──────┴───────┴────────┘
```

---

## System Instructions

| Instruction | opcode    | funct12       | Reads                    | Writes                          | Flags                           |
|-------------|-----------|---------------|--------------------------|---------------------------------|---------------------------------|
| `ECALL`     | `1110011` | `000000000000`| `PC`, ABI regs `a0–a7`   | `PC`, ABI regs `a0–a1`          | May change privilege mode       |
| `EBREAK`    | `1110011` | `000000000001`| `PC`                     | `PC`                            | Traps to debugger               |
| `FENCE`     | `0001111` | n/a           | —                        | — (orders MEM/IO visibility)    | —                               |

`ECALL` and `EBREAK` are I-type with `rs1 = 0`, `funct3 = 000`, `rd = 0`; only `imm[11:0]` differs.

### Binary layout — `ECALL`

```
 31          20 19   15 14  12 11    7 6      0
┌──────────────┬───────┬──────┬───────┬────────┐
│ 000000000000 │ 00000 │ 000  │ 00000 │1110011 │
│   funct12    │  rs1  │funct3│  rd   │ opcode │
└──────────────┴───────┴──────┴───────┴────────┘
```

### Binary layout — `EBREAK`

```
 31          20 19   15 14  12 11    7 6      0
┌──────────────┬───────┬──────┬───────┬────────┐
│ 000000000001 │ 00000 │ 000  │ 00000 │1110011 │
│   funct12    │  rs1  │funct3│  rd   │ opcode │
└──────────────┴───────┴──────┴───────┴────────┘
```

### Binary layout — `FENCE` (ordering: predecessor/successor encoded in imm)

```
 31   28 27  24 23  20 19   15 14  12 11    7 6      0
┌──────┬──────┬──────┬───────┬──────┬───────┬────────┐
│ fm   │ pred │ succ │ 00000 │ 000  │ 00000 │0001111 │
│4 bits│4 bits│4 bits│  rs1  │funct3│  rd   │ opcode │
└──────┴──────┴──────┴───────┴──────┴───────┴────────┘
  pred/succ bits: I=bit3, O=bit2, R=bit1, W=bit0
```

---

## Opcode Summary Table

| Opcode    | Format | Instruction(s)            |
|-----------|--------|---------------------------|
| `0110011` | R      | ADD, SUB, SLL, SLT, SLTU, XOR, SRL, SRA, OR, AND |
| `0010011` | I      | ADDI, SLTI, SLTIU, XORI, ORI, ANDI, SLLI, SRLI, SRAI |
| `0000011` | I      | LB, LH, LW, LBU, LHU     |
| `0100011` | S      | SB, SH, SW                |
| `1100011` | B      | BEQ, BNE, BLT, BGE, BLTU, BGEU |
| `0110111` | U      | LUI                        |
| `0010111` | U      | AUIPC                      |
| `1101111` | J      | JAL                        |
| `1100111` | I      | JALR                       |
| `1110011` | I      | ECALL, EBREAK              |
| `0001111` | I      | FENCE                      |

---

## Key Takeaways

- **No flags ever** — RV32I has no zero, carry, overflow, or sign flags. Comparisons write results directly into a register.
- **PC is always mutated** — every instruction advances PC (to `PC+4` or a branch/jump target).
- **x0 is a sink** — if `rd = x0`, the write is discarded silently.
- **Only stores and system calls write memory** — all other instructions are register-only (plus PC).
- **Immediates are always sign-extended** — except for `SLTIU` which sign-extends then compares as unsigned, and shift amounts which use only the low 5 bits.
- **B and J immediate bits are scrambled** — hardware places sign bit at `inst[31]` in all formats to simplify sign-extension logic.