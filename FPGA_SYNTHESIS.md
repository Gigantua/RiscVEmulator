# FPGA_SYNTHESIS.md — what in `rv32i_core.cpp` is a CPU, and what is host scaffolding

> Companion to `PARAVIRTUAL.md` (the privilege-removal feasibility study).
> Where that document is about *shrinking the guest's dependence on privilege*,
> this one is about *which lines of the native core would actually become RTL*
> on an FPGA — and which are emulator-only and evaporate at synthesis.

## Goal

The long-term target is a real FPGA RISC-V core: **CPU + memory + memory-mapped
peripherals, nothing else**. That model is not exotic — it *is* the standard
SoC. The core only ever issues loads/stores to addresses; an interconnect
decodes them; RAM sits behind some addresses and peripheral blocks behind
others; a few interrupt wires run from the peripherals back into the core. The
core never knows whether an address is RAM or a UART.

So having peripherals does not compromise core purity. The only thing that
would is a "device" that needs a back-channel into CPU registers — and the
current core has none: every load/store is a plain bus access.

## Status

`rv32i_core.cpp` is ~355 lines and splits cleanly into three blocks (see the
file header): a base RV32I CPU, a trap unit, and the host ABI.

**Zicsr has been removed.** No CSR instruction (`opcode 0x73, funct3 != 0`)
decodes — every one traps as illegal, alongside `MRET`/`SRET`/`WFI`. There are
**no architectural CSRs in the core at all.** The trap unit keeps exactly two
registers of state — current privilege (`TrapUnit.priv`) and the host
interrupt-pin latch (`TrapUnit.pending`). Everything a CSR used to hold —
interrupt-enable, per-source mask, handler vector, the saved trap context —
lives in the **trap-frame page** at guest-physical `0x0F000000`, which is
plain RAM. In RTL that page is just a small block of memory at a decoded
address; it needs no CPU back-channel.

**The CLINT timer lives in the C# `ClintDevice`.** `mtime`/`mtimecmp` and the
wall-clock math are not in the core; the core's only timer coupling is the
**MTIP interrupt-input pin** (`rv32i_set_mtip`), mirroring `rv32i_set_meip`.
`check_interrupts()` does no timer compute.

## The three buckets

Every region of `Native/rv32i_core.cpp` falls into one of:

- **① Core** — real CPU logic that maps directly to RTL.
- **② Core interface, needs rework** — genuine CPU function, but the emulator
  shortcut won't synthesize; it becomes a port or a real block.
- **③ Scaffolding** — host/emulator-only; deleted or `#ifdef`'d out of any
  synthesis build.

## Region map

Anchored to function/region names (line numbers drift). Snapshot taken against
the ~355-line revision.

| Region | Bucket | RTL fate |
|---|---|---|
| Header comment | — | Already describes the SoC model (PART 1/2/3) |
| `memset` shim | ③ | CRT replacement for `-nodefaultlib`; RTL has reset logic |
| `CPU_State`: `regs[32]`, `pc` | ① | Register file, PC reg |
| `CPU_State.mem` pointer | ② | Becomes the **bus-master port** |
| `CPU_State.halted` | ① minor | A run/halt control FF |
| `CpuException` / `EXC_*` enum | ① | The decoder→trap-unit handoff — a couple of status wires |
| `mem_read` / `mem_write` | ② | Concept is core (LSU); `*(mem+addr)` → real bus transactions |
| `j_imm`/`b_imm`/`i_imm`/`s_imm` | ① | Pure combinational bit-slicing — synthesizes to wires |
| `cpu_step` (fetch/decode/execute/writeback) | ① | **This is the CPU.** Synthesizes directly |
| `TrapUnit` (`priv`, `pending`) | ① | Two FFs: privilege bit + the IRQ-pin latch |
| `PRIV_*`/`STATUS_*`/`CAUSE_*`/`PIN_*`/`TRAP_PAGE` consts | ① | Architectural constants + the trap-page address map |
| `PV_RESUME_GATEWAY` const | ② | A magic-PC trap-return; real RTL uses `MRET` (see end-state) |
| `do_trap` | ① | Trap-entry FSM — spills the register file to the trap-frame page |
| `trap_return` | ② | Trap-return FSM; reached via the gateway, not an opcode |
| `trap_system` | ① | ECALL/EBREAK → trap; every other SYSTEM encoding → illegal |
| `check_interrupts` | ① | Pure interrupt arbitration. MTIP/MEIP arrive on pins |
| `do_step` (interrupt sample · gateway check · `cpu_step` dispatch) | ① / ② | The step loop is ① ; the `pc == PV_RESUME_GATEWAY` check is ② |
| `rv32i_step_n`, `rv32i_init`, `rv32i_destroy` | ③ | Host batching/construction → reset logic + free-running clock |
| `rv32i_get_pc`, `rv32i_is_halted`, `rv32i_set_reg`, `rv32i_set_halted` | ③ | Debug/introspection ABI → optional JTAG debug module |
| `rv32i_set_meip`, `rv32i_set_mtip` | ② **correct model** | These *are* pins — `meip_i`/`mtip_i`, driven by the PLIC/CLINT. Keep this pattern |
| `DllMain` | ③ | Windows DLL glue |

**Headline:** the immediate decoders + `cpu_step` + the trap unit
(`do_trap`/`trap_return`/`trap_system`/`check_interrupts`) + the `do_step`
loop are the synthesizable CPU. The scaffolding left is small: the `memset`
shim, the host batching/construction ABI, the debug readouts, and `DllMain`.
There is no telemetry and no paravirt MMIO device to strip — earlier revisions
had both; they are gone.

## Target architecture

```
              ┌──────────────────────────────────────────┐
              │            rv32i CPU core                │
              │  regs[32] · pc · priv FF · IRQ-pin latch  │
              │  decode (imm) · execute · trap FSM        │
              │                                           │
   clk,rst ──▶│                                           │
              │  bus master:  addr/wdata/rdata/wstrb/req  │──┐
  mtip_i  ───▶│  irq pins                                 │  │
  meip_i  ───▶│                                           │  │
  msip_i  ───▶│                                           │  │
              └──────────────────────────────────────────┘  │
                                                             ▼
              ┌──────────────────────────────────────────────────┐
              │   Interconnect — pure address decode              │
              └──┬─────────┬──────────┬──────────┬──────────┬─────┘
                 ▼         ▼          ▼          ▼          ▼
              ┌─────┐  ┌───────┐  ┌──────┐  ┌──────┐  ┌──────────┐
              │ RAM │  │ CLINT │  │ PLIC │  │ UART │  │ FB / etc │
              │BRAM/│  │mtime  │  │      │  │      │  │          │
              │DRAM │  │mtimcmp│  │      │  │      │  │          │
              └─────┘  └───┬───┘  └──┬───┘  └──────┘  └──────────┘
                           │         │
                  mtip_i,msip_i    meip_i   (real wires back to the core)
```

Every block under the interconnect is a normal bus slave — including the
trap-frame page, which is just a 4 KB RAM block at `0x0F000000`. The CLINT
owns `mtime`/`mtimecmp` and **drives the `mtip` wire** — the
`mtime >= mtimecmp` comparator, where it belongs. The PLIC drives `meip`. From
the core's view it is purely "CPU + memory + a few interrupt pins".

## The core's port list

The whole synthesizable contract:

```
clk, rst_n
bus:  o_addr[31:0]  o_wdata[31:0]  o_wstrb[3:0]  o_req  o_we   i_rdata[31:0]  i_ready
irq:  i_mtip  i_meip  i_msip
dbg:  (optional) i_halt_req  o_pc[31:0]
```

Nothing else. `rv32i_set_meip`/`rv32i_set_mtip` are already exactly this shape
— the host driving a pin. That is the model; `i_msip` should join it.

## Must be reworked, not just relabeled

Three items are genuine CPU logic but will not synthesize as written:

1. **Memory access** — `*(volatile T*)(mem+addr)` → a load/store unit that
   issues bus cycles and stalls on `i_ready`. Multi-cycle, not one dereference.
2. **No misalignment handling** — RTL cannot do a free unaligned 32-bit access;
   needs cause-0/4/6 traps or a multi-cycle path.
3. **Reserved sub-decodes fall through** — the top-level opcode switch raises
   illegal-instruction on unknown opcodes (`default` case), but inside the
   LOAD and STORE decodes the reserved `funct3` values have no `default` and
   silently retire as a NOP. RTL must raise illegal-instruction there too.

## The end-state

If `PARAVIRTUAL.md` reaches its goal — no privilege, no traps — the
synthesizable core collapses to essentially the immediate decoders plus
`cpu_step`: decode + execute + a bus master. That is the smallest possible
RV32I core and the easiest thing on this list to put on an FPGA.

The trap unit is the *optional* part. It is already CSR-free — its state is
the `0x0F000000` RAM page plus two FFs — so it synthesizes as a small FSM, not
a CSR file. The one non-architectural piece is the `PV_RESUME_GATEWAY`
magic-PC trap-return: real RTL would use `MRET` (or, if the trap unit is
dropped entirely, have no traps at all).

## Rule of thumb while the core is stripped down

- Moving device logic into a **real peripheral** (the CLINT timer, the PLIC)
  is *toward* the FPGA goal — `mtime` does not belong in the CPU.
- Trap state that lives in a **plain RAM page** (the `0x0F000000` trap-frame
  page) is FPGA-friendly: in RTL it is just memory behind a decoded address,
  with no back-channel into CPU registers. Keep it that way — never turn it
  into a guarded MMIO device that aliases internal CPU state.
