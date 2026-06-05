using System.Text;

namespace RiscVEmulator.Core.Cuda
{
    /// <summary>
    /// RV32IM → PTX translator (the "killer" JIT path).
    ///
    /// The C-emitting JIT (<see cref="RvJit"/>) must round-trip through nvcc + cl
    /// + the device linker into a DLL — minutes of compile for a large guest, and
    /// a separate module to load. This emitter generates a complete PTX <c>.entry</c>
    /// kernel for the guest directly, which <c>ptxas</c> assembles to sm_86 SASS in
    /// milliseconds and the CUDA Driver API loads in-process (cuModuleLoadData) —
    /// a true runtime JIT, no nvcc, no DLL.
    ///
    /// Codegen model (matches the validated --jit C harness's RAM-only memory
    /// model so it can be checked against the same guest + interpreter oracle):
    ///   • guest registers x0..x31 are PTX virtual registers %R0..%R31 (x0 pinned
    ///     to 0 and never written), so ptxas keeps them in SASS registers;
    ///   • each guest instruction is straight-line PTX at label <c>L_&lt;pc&gt;</c>;
    ///     static branches/JAL chain directly to the target label (block chaining);
    ///   • a dynamic JALR / out-of-range target jumps to <c>DISP</c>, which maps pc
    ///     to a label via a compare chain over the basic-block-leader set (anything
    ///     unmatched exits cleanly — bit-exact, just not chained);
    ///   • memory is RAM + the exit device at <c>exitBase</c> (store there halts),
    ///     identical to <c>Program.JitTemplate</c>; loads/stores are inline
    ///     bounds-checked ld.global/st.global;
    ///   • DIV/REM are guarded for RISC-V semantics (÷0 ⇒ −1 / dividend; INT_MIN÷−1).
    ///
    /// Hang-safety: every back-edge and every DISP pass decrements a 64-bit budget
    /// and exits at zero, so the kernel is always bounded on a display GPU.
    /// </summary>
    public static class RvPtxJit
    {
        private static string H(uint v) => "0x" + v.ToString("X");

        // PTX param layout the driver-API launcher fills in (all by-value):
        //   p_ram(u64) p_ramsz(u32) p_sp(u32) p_entry(u32)
        //   p_budget(u64) p_exitbase(u32) p_regsout(u64) p_pcout(u64) p_exitout(u64)
        public static string Emit(byte[] code, uint lo, uint hi, uint entry,
                                  uint exitBase = 0x40000000u, string kernelName = "rvjit_run")
        {
            var leaders = new System.Collections.Generic.HashSet<uint> { lo };
            if (entry >= lo && entry < hi) leaders.Add(entry);
            for (uint a = lo; a + 4 <= hi; a += 4)
            {
                uint ins = LE32(code, (int)(a - lo));
                uint op = ins & 0x7F; uint next = a + 4;
                if (op == 0x63) { uint t = a + BImm(ins); if (t >= lo && t < hi) leaders.Add(t); }
                else if (op == 0x6F) { uint t = a + JImm(ins); if (t >= lo && t < hi) leaders.Add(t); if (next < hi) leaders.Add(next); }
                else if (op == 0x67) { if (next < hi) leaders.Add(next); }
            }

            var sb = new StringBuilder();
            sb.Append(".version 7.1\n.target sm_86\n.address_size 64\n\n");
            sb.Append($".visible .entry {kernelName} (\n");
            sb.Append("    .param .u64 p_ram, .param .u32 p_ramsz, .param .u32 p_sp,\n");
            sb.Append("    .param .u32 p_entry, .param .u64 p_budget, .param .u32 p_exitbase,\n");
            sb.Append("    .param .u64 p_regsout, .param .u64 p_pcout, .param .u64 p_exitout\n");
            sb.Append(")\n{\n");
            sb.Append("    .reg .b32 %R<32>;\n");
            sb.Append("    .reg .b32 %t<6>;\n");
            sb.Append("    .reg .b32 %pc;\n    .reg .b32 %rsz;\n    .reg .b32 %eb;\n    .reg .b32 %exitc;\n");
            sb.Append("    .reg .b64 %bl;\n    .reg .b64 %ram;\n    .reg .b64 %ad;\n    .reg .b64 %q;\n");
            sb.Append("    .reg .pred %p;\n    .reg .pred %p2;\n\n");

            // ── prologue ──
            sb.Append("    ld.param.u64 %ram, [p_ram];\n");
            sb.Append("    ld.param.u32 %rsz, [p_ramsz];\n");
            sb.Append("    ld.param.u32 %eb,  [p_exitbase];\n");
            sb.Append("    ld.param.u64 %bl,  [p_budget];\n");
            for (int i = 0; i < 32; i++) sb.Append($"    mov.b32 %R{i}, 0;\n");
            sb.Append("    ld.param.u32 %R2,  [p_sp];\n");
            sb.Append("    ld.param.u32 %pc,  [p_entry];\n");
            sb.Append("    mov.b32 %exitc, 0;\n");
            sb.Append($"    bra L_{entry:X};\n\n");

            // ── dynamic dispatch: pc → label (compare chain over leaders) ──
            sb.Append("DISP:\n");
            foreach (uint t in SortedLeaders(leaders))
            {
                sb.Append($"    setp.eq.u32 %p, %pc, {H(t)};\n");
                sb.Append($"    @%p bra L_{t:X};\n");
            }
            sb.Append("    bra EXIT;\n\n");

            // ── translated body ──
            for (uint a = lo; a + 4 <= hi; a += 4)
            {
                sb.Append($"L_{a:X}:\n");
                EmitInstr(sb, a, LE32(code, (int)(a - lo)), lo, hi);
            }
            sb.Append($"    mov.b32 %pc, {H(hi)}; bra EXIT;\n");

            // ── epilogue: write x0..x31, pc, exitcode back to global ──
            sb.Append("EXIT:\n");
            sb.Append("    ld.param.u64 %q, [p_regsout];\n");
            for (int i = 0; i < 32; i++) sb.Append($"    st.global.u32 [%q+{i * 4}], %R{i};\n");
            sb.Append("    ld.param.u64 %q, [p_pcout];   st.global.u32 [%q], %pc;\n");
            sb.Append("    ld.param.u64 %q, [p_exitout]; st.global.u32 [%q], %exitc;\n");
            sb.Append("    ret;\n}\n");
            return sb.ToString();
        }

        private static System.Collections.Generic.List<uint> SortedLeaders(System.Collections.Generic.HashSet<uint> s)
        {
            var l = new System.Collections.Generic.List<uint>(s); l.Sort(); return l;
        }

        private static string Rn(int n) => $"%R{n}";

        private static void EmitInstr(StringBuilder sb, uint pc, uint instr, uint lo, uint hi)
        {
            uint op = instr & 0x7F;
            int rd = (int)((instr >> 7) & 0x1F);
            int rs1 = (int)((instr >> 15) & 0x1F);
            int rs2 = (int)((instr >> 20) & 0x1F);
            uint f3 = (instr >> 12) & 0x7;
            uint f7 = (instr >> 25) & 0x7F;
            uint next = pc + 4;
            bool InR(uint t) => t >= lo && t < hi;

            void Wr(string ptx) { if (rd != 0) sb.Append("    " + ptx + "\n"); }
            void Budget(string onZeroPc)
            {
                sb.Append("    add.s64 %bl, %bl, -1;\n");
                sb.Append("    setp.le.s64 %p2, %bl, 0;\n");
                sb.Append($"    @%p2 mov.b32 %pc, {onZeroPc};\n");
                sb.Append("    @%p2 bra EXIT;\n");
            }
            void Jump(uint t, bool backedge)
            {
                if (backedge) Budget(H(t));
                if (InR(t)) sb.Append($"    bra L_{t:X};\n");
                else sb.Append($"    mov.b32 %pc, {H(t)}; bra EXIT;\n");
            }

            switch (op)
            {
                case 0x37: Wr($"mov.b32 {Rn(rd)}, {H(instr & 0xFFFFF000u)};"); return;          // LUI
                case 0x17: Wr($"mov.b32 {Rn(rd)}, {H(pc + (instr & 0xFFFFF000u))};"); return;    // AUIPC

                case 0x6F: // JAL
                {
                    uint t = pc + JImm(instr);
                    if (rd != 0) sb.Append($"    mov.b32 {Rn(rd)}, {H(next)};\n");
                    Jump(t, backedge: t <= pc);
                    return;
                }
                case 0x67: // JALR
                {
                    sb.Append($"    add.s32 %t0, {Rn(rs1)}, {IImm(instr)};\n");
                    sb.Append("    and.b32 %t0, %t0, 0xFFFFFFFE;\n");
                    if (rd != 0) sb.Append($"    mov.b32 {Rn(rd)}, {H(next)};\n");
                    sb.Append("    mov.b32 %pc, %t0;\n");
                    Budget("%pc");
                    sb.Append("    bra DISP;\n");
                    return;
                }
                case 0x63: // BRANCH
                {
                    uint t = pc + BImm(instr);
                    string? cc = f3 switch
                    {
                        0 => "eq.u32", 1 => "ne.u32",
                        4 => "lt.s32", 5 => "ge.s32",
                        6 => "lt.u32", 7 => "ge.u32",
                        _ => null
                    };
                    if (cc == null) { sb.Append($"    mov.b32 %pc, {H(pc)}; bra EXIT;\n"); return; }
                    sb.Append($"    setp.{cc} %p, {Rn(rs1)}, {Rn(rs2)};\n");
                    if (t <= pc)
                    {
                        sb.Append($"    @!%p bra $b_{pc:X};\n");
                        Jump(t, backedge: true);
                        sb.Append($"$b_{pc:X}:\n");
                    }
                    else
                    {
                        if (InR(t)) sb.Append($"    @%p bra L_{t:X};\n");
                        else sb.Append($"    @!%p bra $b_{pc:X};\n    mov.b32 %pc, {H(t)}; bra EXIT;\n$b_{pc:X}:\n");
                    }
                    return;
                }

                case 0x03: // LOAD
                {
                    int sz = f3 switch { 0 => 1, 4 => 1, 1 => 2, 5 => 2, 2 => 4, _ => 0 };
                    if (sz == 0) { sb.Append($"    mov.b32 %pc, {H(pc)}; bra EXIT;\n"); return; }
                    sb.Append($"    add.s32 %t0, {Rn(rs1)}, {IImm(instr)};\n");
                    sb.Append($"    sub.u32 %t1, %rsz, {sz};\n");
                    sb.Append("    setp.gt.u32 %p, %t0, %t1;\n");
                    string lab = $"$ld_{pc:X}";
                    if (rd != 0) sb.Append($"    @%p mov.b32 {Rn(rd)}, 0;\n");
                    sb.Append($"    @%p bra {lab};\n");
                    sb.Append("    cvt.u64.u32 %ad, %t0;\n    add.u64 %ad, %ad, %ram;\n");
                    string ld = f3 switch
                    {
                        0 => "ld.global.s8  %t2, [%ad];",
                        4 => "ld.global.u8  %t2, [%ad];",
                        1 => "ld.global.s16 %t2, [%ad];",
                        5 => "ld.global.u16 %t2, [%ad];",
                        2 => "ld.global.u32 %t2, [%ad];",
                        _ => ""
                    };
                    sb.Append("    " + ld + "\n");
                    if (rd != 0) sb.Append($"    mov.b32 {Rn(rd)}, %t2;\n");
                    sb.Append($"{lab}:\n");
                    return;
                }
                case 0x23: // STORE
                {
                    int sz = f3 switch { 0 => 1, 1 => 2, 2 => 4, _ => 0 };
                    if (sz == 0) { sb.Append($"    mov.b32 %pc, {H(pc)}; bra EXIT;\n"); return; }
                    sb.Append($"    add.s32 %t0, {Rn(rs1)}, {SImm(instr)};\n");
                    sb.Append("    setp.eq.u32 %p, %t0, %eb;\n");
                    sb.Append($"    @%p mov.b32 %exitc, {Rn(rs2)};\n");
                    sb.Append($"    @%p mov.b32 %pc, {H(next)};\n");
                    sb.Append("    @%p bra EXIT;\n");
                    sb.Append($"    sub.u32 %t1, %rsz, {sz};\n");
                    sb.Append("    setp.gt.u32 %p, %t0, %t1;\n");
                    string lab = $"$st_{pc:X}";
                    sb.Append($"    @%p bra {lab};\n");
                    sb.Append("    cvt.u64.u32 %ad, %t0;\n    add.u64 %ad, %ad, %ram;\n");
                    string st = f3 switch
                    {
                        0 => $"st.global.u8  [%ad], {Rn(rs2)};",
                        1 => $"st.global.u16 [%ad], {Rn(rs2)};",
                        2 => $"st.global.u32 [%ad], {Rn(rs2)};",
                        _ => ""
                    };
                    sb.Append("    " + st + "\n");
                    sb.Append($"{lab}:\n");
                    return;
                }

                case 0x13: // OP-IMM
                {
                    int imm = IImm(instr); int sh = (int)((instr >> 20) & 0x1F);
                    switch (f3)
                    {
                        case 0: Wr($"add.s32 {Rn(rd)}, {Rn(rs1)}, {imm};"); return;          // ADDI
                        case 1: if (f7 == 0) { Wr($"shl.b32 {Rn(rd)}, {Rn(rs1)}, {sh};"); return; } break; // SLLI
                        case 2: sb.Append($"    setp.lt.s32 %p, {Rn(rs1)}, {imm};\n"); Wr($"selp.b32 {Rn(rd)}, 1, 0, %p;"); return; // SLTI
                        case 3: sb.Append($"    setp.lt.u32 %p, {Rn(rs1)}, {H((uint)imm)};\n"); Wr($"selp.b32 {Rn(rd)}, 1, 0, %p;"); return; // SLTIU
                        case 4: Wr($"xor.b32 {Rn(rd)}, {Rn(rs1)}, {H((uint)imm)};"); return;   // XORI
                        case 5: if (f7 == 0x20) { Wr($"shr.s32 {Rn(rd)}, {Rn(rs1)}, {sh};"); return; } if (f7 == 0) { Wr($"shr.u32 {Rn(rd)}, {Rn(rs1)}, {sh};"); return; } break; // SRAI/SRLI
                        case 6: Wr($"or.b32 {Rn(rd)}, {Rn(rs1)}, {H((uint)imm)};"); return;    // ORI
                        case 7: Wr($"and.b32 {Rn(rd)}, {Rn(rs1)}, {H((uint)imm)};"); return;   // ANDI
                    }
                    sb.Append($"    mov.b32 %pc, {H(pc)}; bra EXIT;\n"); return;
                }
                case 0x33: // OP (+M)
                {
                    if (f7 == 0x01) { EmitMulDiv(sb, rd, rs1, rs2, f3, pc); return; }
                    bool sub = f7 == 0x20;
                    switch (f3)
                    {
                        case 0: Wr($"{(sub ? "sub" : "add")}.s32 {Rn(rd)}, {Rn(rs1)}, {Rn(rs2)};"); return; // ADD/SUB
                        case 1: sb.Append($"    and.b32 %t0, {Rn(rs2)}, 31;\n"); Wr($"shl.b32 {Rn(rd)}, {Rn(rs1)}, %t0;"); return; // SLL
                        case 2: sb.Append($"    setp.lt.s32 %p, {Rn(rs1)}, {Rn(rs2)};\n"); Wr($"selp.b32 {Rn(rd)}, 1, 0, %p;"); return; // SLT
                        case 3: sb.Append($"    setp.lt.u32 %p, {Rn(rs1)}, {Rn(rs2)};\n"); Wr($"selp.b32 {Rn(rd)}, 1, 0, %p;"); return; // SLTU
                        case 4: Wr($"xor.b32 {Rn(rd)}, {Rn(rs1)}, {Rn(rs2)};"); return; // XOR
                        case 5: sb.Append($"    and.b32 %t0, {Rn(rs2)}, 31;\n"); Wr($"{(sub ? "shr.s32" : "shr.u32")} {Rn(rd)}, {Rn(rs1)}, %t0;"); return; // SRA/SRL
                        case 6: Wr($"or.b32 {Rn(rd)}, {Rn(rs1)}, {Rn(rs2)};"); return;  // OR
                        case 7: Wr($"and.b32 {Rn(rd)}, {Rn(rs1)}, {Rn(rs2)};"); return; // AND
                    }
                    return;
                }
                case 0x0F: sb.Append("    // fence = nop\n"); return;                              // FENCE
                default: sb.Append($"    mov.b32 %pc, {H(pc)}; bra EXIT;\n"); return;              // SYSTEM/A/unknown
            }
        }

        private static void EmitMulDiv(StringBuilder sb, int rd, int rs1, int rs2, uint f3, uint pc)
        {
            void Wr(string s) { if (rd != 0) sb.Append("    " + s + "\n"); }
            string R(int n) => $"%R{n}";
            switch (f3)
            {
                case 0: Wr($"mul.lo.s32 {R(rd)}, {R(rs1)}, {R(rs2)};"); return;        // MUL
                case 1: Wr($"mul.hi.s32 {R(rd)}, {R(rs1)}, {R(rs2)};"); return;        // MULH
                case 3: Wr($"mul.hi.u32 {R(rd)}, {R(rs1)}, {R(rs2)};"); return;        // MULHU
                case 2: // MULHSU: (s64)rs1 * (u64)rs2 >> 32
                    sb.Append($"    cvt.s64.s32 %q, {R(rs1)};\n");
                    sb.Append($"    cvt.u64.u32 %ad, {R(rs2)};\n");
                    sb.Append("    mul.lo.s64 %q, %q, %ad;\n");
                    sb.Append("    shr.u64 %q, %q, 32;\n");
                    Wr($"cvt.u32.u64 {R(rd)}, %q;");
                    return;
                case 4: // DIV: /0 → -1; INT_MIN/-1 → INT_MIN
                {
                    string end = $"$dve_{pc:X}";
                    sb.Append($"    setp.eq.s32 %p, {R(rs2)}, 0;\n");
                    if (rd != 0) sb.Append($"    @%p mov.b32 {R(rd)}, 0xFFFFFFFF;\n");
                    sb.Append($"    @%p bra {end};\n");
                    sb.Append($"    setp.eq.s32 %p, {R(rs1)}, 0x80000000;\n");
                    sb.Append($"    setp.eq.s32 %p2, {R(rs2)}, -1;\n    and.pred %p, %p, %p2;\n");
                    if (rd != 0) sb.Append($"    @%p mov.b32 {R(rd)}, 0x80000000;\n");
                    sb.Append($"    @%p bra {end};\n");
                    Wr($"div.s32 {R(rd)}, {R(rs1)}, {R(rs2)};");
                    sb.Append($"{end}:\n");
                    return;
                }
                case 5: // DIVU: /0 → 0xFFFFFFFF
                {
                    string end = $"$dvue_{pc:X}";
                    sb.Append($"    setp.eq.u32 %p, {R(rs2)}, 0;\n");
                    if (rd != 0) sb.Append($"    @%p mov.b32 {R(rd)}, 0xFFFFFFFF;\n");
                    sb.Append($"    @%p bra {end};\n");
                    Wr($"div.u32 {R(rd)}, {R(rs1)}, {R(rs2)};");
                    sb.Append($"{end}:\n");
                    return;
                }
                case 6: // REM: /0 → rs1; INT_MIN/-1 → 0
                {
                    string end = $"$rem_{pc:X}";
                    sb.Append($"    setp.eq.s32 %p, {R(rs2)}, 0;\n");
                    if (rd != 0) sb.Append($"    @%p mov.b32 {R(rd)}, {R(rs1)};\n");
                    sb.Append($"    @%p bra {end};\n");
                    sb.Append($"    setp.eq.s32 %p, {R(rs1)}, 0x80000000;\n");
                    sb.Append($"    setp.eq.s32 %p2, {R(rs2)}, -1;\n    and.pred %p, %p, %p2;\n");
                    if (rd != 0) sb.Append($"    @%p mov.b32 {R(rd)}, 0;\n");
                    sb.Append($"    @%p bra {end};\n");
                    Wr($"rem.s32 {R(rd)}, {R(rs1)}, {R(rs2)};");
                    sb.Append($"{end}:\n");
                    return;
                }
                case 7: // REMU: /0 → rs1
                {
                    string end = $"$remu_{pc:X}";
                    sb.Append($"    setp.eq.u32 %p, {R(rs2)}, 0;\n");
                    if (rd != 0) sb.Append($"    @%p mov.b32 {R(rd)}, {R(rs1)};\n");
                    sb.Append($"    @%p bra {end};\n");
                    Wr($"rem.u32 {R(rd)}, {R(rs1)}, {R(rs2)};");
                    sb.Append($"{end}:\n");
                    return;
                }
            }
        }

        // ── immediates (mirror rv32i_cuda.cu) ──
        private static uint JImm(uint i)
        {
            uint v = ((i >> 31) & 1u) << 20 | ((i >> 12) & 0xFFu) << 12 | ((i >> 20) & 1u) << 11 | ((i >> 21) & 0x3FFu) << 1;
            return (v & 0x100000u) != 0 ? v | 0xFFE00000u : v;
        }
        private static uint BImm(uint i)
        {
            uint v = ((i >> 31) & 1u) << 12 | ((i >> 7) & 1u) << 11 | ((i >> 25) & 0x3Fu) << 5 | ((i >> 8) & 0xFu) << 1;
            return (v & 0x1000u) != 0 ? v | 0xFFFFE000u : v;
        }
        private static int IImm(uint i) => (int)i >> 20;
        private static int SImm(uint i) => (int)(i & 0xFE000000) >> 20 | (int)((i >> 7) & 0x1F);
        private static uint LE32(byte[] b, int o) => (uint)(b[o] | (b[o + 1] << 8) | (b[o + 2] << 16) | (b[o + 3] << 24));
    }
}
