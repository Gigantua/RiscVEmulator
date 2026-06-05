using System.Text;

namespace RiscVEmulator.Core.Cuda
{
    /// <summary>
    /// Ahead-of-time RV32IMA → CUDA-C translator (the GPU JIT).
    ///
    /// The interpreter costs ~110 GPU instructions per guest instruction
    /// (fetch + decode + dynamic dispatch + predicated ALU mux + shared-memory
    /// register file), opcode-independent, capping a single GPU thread at
    /// ~2.7 MIPS. Translating the guest's known code to native CUDA C collapses
    /// that to a handful, because:
    ///   • guest registers become local <c>uint32_t R[32]</c> accessed with
    ///     COMPILE-TIME-CONSTANT indices, so ptxas keeps them in real registers;
    ///   • there is no fetch and no decode — each guest instruction is a C
    ///     statement;
    ///   • straight-line code falls through and branches jump directly to the
    ///     target's label (<b>block chaining</b>) — no per-instruction dispatch.
    ///
    /// Hang safety (critical on a display GPU): every control-flow cycle passes
    /// through a budget decrement. A backward branch/JAL (target ≤ pc) and every
    /// JALR emit <c>if(--bl&lt;=0||h.halted){...;goto out;}</c>, so the kernel is
    /// always bounded regardless of guest behaviour.
    ///
    /// Loads/stores use an inline RAM fast path (<c>jit_l*</c>/<c>jit_s*</c>):
    /// a single bounds compare hits RAM directly, otherwise falls back to
    /// <c>mem_read</c>/<c>mem_write</c> (MMIO, exit device, framebuffer, …).
    /// Opcodes the translator doesn't handle set <c>pc</c> and
    /// <c>goto interp</c> (interpreter fallback), so correctness never depends on
    /// full coverage.
    /// </summary>
    public static class RvJit
    {
        /// <summary>
        /// Translate the read-only code span [lo, hi) into a CUDA-C body: a
        /// <c>switch(pc)</c> that jumps to a per-instruction label, followed by
        /// the chained, labelled instructions. Assumes locals <c>R[0..31]</c>,
        /// <c>pc</c>, <c>bl</c> (budget left), and <c>h</c>/<c>m</c>
        /// (<c>Hart&amp;</c>/<c>CoreMem&amp;</c>) plus the <c>jit_*</c> helpers,
        /// and the labels <c>dispatch</c>, <c>out</c>, <c>interp</c>.
        ///
        /// <paramref name="entry"/> (the guest entry PC) is added to the dispatch
        /// set so the kernel's initial <c>pc=entry; goto dispatch</c> always chains
        /// into native code instead of diverting to the interpreter. It is optional
        /// for source-compat with callers that pass only <c>(code, lo, hi)</c>; when
        /// 0 or out of range the basic-block-leader analysis still recovers most
        /// entries, and any miss is merely a (correct) interpreter fallback.
        /// </summary>
        public static string TranslateBody(byte[] code, uint lo, uint hi, uint entry = 0)
        {
            // Pre-pass: collect the set of addresses that can be entered through
            // the runtime `switch(pc)` dispatch — i.e. addresses where control may
            // arrive with arbitrary register state. Keeping the switch to just
            // these (instead of every word in the span) is the single biggest win:
            // each switch `case` is a merge point that forces `R[]`, `pc`, `bl`
            // etc. live, which defeats ptxas scalar-replacement of the register
            // file. Straight-line / statically-chained code never re-enters via the
            // switch, so it does NOT need a case — only a label (emitted for all).
            //
            // Dispatch reachability sources (the "basic-block leaders"):
            //   • lo, the first instruction of the span;
            //   • every in-range static BRANCH / JAL target (some are re-dispatched
            //     when out of range, but in-range ones chain directly — listing
            //     them keeps a JALR that happens to land on one chainable);
            //   • every return address (next-PC after a JAL/JALR that links rd!=0),
            //     since that is exactly where a `ret` (JALR) lands;
            //   • every instruction whose predecessor does NOT fall through
            //     (predecessor is an unconditional JAL or a JALR) — this is a
            //     function/region entry such as `_start`, and is exactly where the
            //     kernel's initial `pc=entry; goto dispatch` lands, so it MUST be a
            //     case or the whole program would divert to the interpreter.
            // Any dynamic JALR target NOT in this set falls through `default:` to
            // the interpreter, which is the bit-exact oracle and already had its
            // budget decremented — correct and bounded, just not chained.
            var targets = new System.Collections.Generic.HashSet<uint>();
            targets.Add(lo);
            if (entry >= lo && entry < hi) targets.Add(entry);   // guaranteed real entry leader
            for (uint a = lo; a + 4 <= hi; a += 4)
            {
                uint instr = LE32(code, (int)(a - lo));
                uint op = instr & 0x7F;
                int rd = (int)((instr >> 7) & 0x1F);
                uint next = a + 4;
                if (op == 0x63) // BRANCH → in-range taken target
                {
                    uint t = a + BImm(instr);
                    if (t >= lo && t < hi) targets.Add(t);
                }
                else if (op == 0x6F) // JAL → target + (link) return addr + fall-through leader
                {
                    uint t = a + JImm(instr);
                    if (t >= lo && t < hi) targets.Add(t);
                    if (rd != 0 && next < hi) targets.Add(next);   // return address
                    if (next < hi) targets.Add(next);              // leader after non-fall-through
                }
                else if (op == 0x67) // JALR → dynamic; link/return addr + fall-through leader
                {
                    if (rd != 0 && next < hi) targets.Add(next);   // return address
                    if (next < hi) targets.Add(next);              // leader after non-fall-through
                }
            }

            var sb = new StringBuilder();
            sb.Append("    goto dispatch;\n");
            sb.Append("  dispatch:\n");
            sb.Append("    switch (pc) {\n");
            for (uint a = lo; a + 4 <= hi; a += 4)
                if (targets.Contains(a))
                    sb.Append($"      case 0x{a:X}u: goto L_{a:X};\n");
            sb.Append("      default: goto interp;\n");
            sb.Append("    }\n");
            for (uint a = lo; a + 4 <= hi; a += 4)
            {
                uint instr = LE32(code, (int)(a - lo));
                sb.Append($"  L_{a:X}: ");
                EmitInstr(sb, a, instr, lo, hi, code);
                sb.Append('\n');
            }
            return sb.ToString();
        }

        // Can executing the cycle body [t, pc] (inclusive) ever set h.halted?
        // h.halted is only raised by the exit-device store (a STORE op) or by the
        // interpreter fallback (any op the JIT routes to `goto interp`, plus JALR
        // which checks h.halted itself). If the back-edge closes a loop whose body
        // contains none of those, h.halted provably cannot change across the cycle,
        // so the back-edge needs only the budget decrement — bit-exact and still
        // bounded (every cycle still decrements bl).
        private static bool CycleCanHalt(byte[] code, uint lo, uint t, uint pc)
        {
            for (uint a = t; a <= pc && a + 4 <= lo + (uint)code.Length; a += 4)
            {
                uint ins = LE32(code, (int)(a - lo));
                uint op = ins & 0x7F;
                if (op == 0x23) return true;          // STORE  → may hit exit device
                if (op == 0x67) return true;          // JALR   → checks/forwards halt
                if (op == 0x73) return true;          // SYSTEM (ECALL/EBREAK) → interp
                if (op == 0x03) continue;             // LOAD never raises halt
                if (op == 0x37 || op == 0x17 || op == 0x6F || op == 0x63 ||
                    op == 0x13 || op == 0x33 || op == 0x0F) continue;  // pure / handled
                return true;                          // anything else → conservative (interp)
            }
            return false;
        }

        private static void EmitInstr(StringBuilder sb, uint pc, uint instr, uint lo, uint hi, byte[] code)
        {
            uint op  = instr & 0x7F;
            int  rd  = (int)((instr >> 7) & 0x1F);
            int  rs1 = (int)((instr >> 15) & 0x1F);
            int  rs2 = (int)((instr >> 20) & 0x1F);
            uint f3  = (instr >> 12) & 0x7;
            uint f7  = (instr >> 25) & 0x7F;
            uint next = pc + 4;

            string D(string e) => rd == 0 ? $"(void)({e});" : $"R[{rd}] = {e};";
            // Source operands: x0 always reads 0 in RV32I, so emit the literal
            // instead of R[0]. Bit-exact, and it drops a false data dependency on
            // the R[0] array slot (helps ptxas keep the file in registers).
            string S1 = rs1 == 0 ? "0u" : $"R[{rs1}]";
            string S2 = rs2 == 0 ? "0u" : $"R[{rs2}]";
            bool InR(uint t) => t >= lo && t < hi;
            // Back-edge budget decrement (hang safety) for an in-range target ≤ pc.
            // Always decrement bl (keeps every cycle bounded). The extra h.halted
            // test can be dropped — turning the back-edge into a single predicted
            // signed compare — only when BOTH:
            //   • the back-edge is CONDITIONAL (a BRANCH, which has a fall-through
            //     escape), so the loop has a natural exit and h.halted is not the
            //     sole way out (a post-halt infinite spin like `j .` is an
            //     UNCONDITIONAL back-edge and must keep the check, or it would burn
            //     the whole budget spinning instead of stopping promptly); and
            //   • the cycle body provably cannot raise h.halted (no store / JALR /
            //     system / interp-routed op between t and pc).
            // Both hold for a pure-ALU counted loop → bit-exact, bounded, cheaper.
            string Chk(uint t, bool conditional)
            {
                if (t > pc) return "";
                bool dropHalt = conditional && !CycleCanHalt(code, lo, t, pc);
                string halt = dropHalt ? "" : "||h.halted";
                return $"if(--bl<=0{halt}){{pc=0x{t:X}u;goto out;}} ";
            }
            // Jump to a static target: chain to its label if in range (with the
            // back-edge guard), else re-dispatch via the switch (→ interp if it
            // isn't translated code). `conditional` = the jump sits under a branch
            // predicate (has a fall-through escape).
            string Jump(uint t, bool conditional) =>
                InR(t) ? $"{Chk(t, conditional)}goto L_{t:X};" : $"pc=0x{t:X}u; goto dispatch;";

            switch (op)
            {
                case 0x37: sb.Append(D($"0x{instr & 0xFFFFF000u:X}u")); return;          // LUI
                case 0x17: sb.Append(D($"0x{pc + (instr & 0xFFFFF000u):X}u")); return;   // AUIPC

                case 0x6F: // JAL
                {
                    uint t = pc + JImm(instr);
                    if (rd != 0) sb.Append($"R[{rd}] = 0x{next:X}u; ");
                    sb.Append(Jump(t, conditional: false));   // unconditional → keep halt check
                    return;
                }
                case 0x67: // JALR (dynamic target → always re-dispatch + budget guard)
                    sb.Append($"{{ uint32_t t = ({S1} + {SImmI(IImm(instr))}) & ~1u; ");
                    if (rd != 0) sb.Append($"R[{rd}] = 0x{next:X}u; ");
                    sb.Append("pc = t; if(--bl<=0||h.halted) goto out; goto dispatch; }");
                    return;

                case 0x63: // BRANCH (taken → jump; not taken → fall through)
                {
                    uint t = pc + BImm(instr);
                    string? c = f3 switch
                    {
                        0 => $"{S1} == {S2}",
                        1 => $"{S1} != {S2}",
                        4 => $"(int32_t){S1} <  (int32_t){S2}",
                        5 => $"(int32_t){S1} >= (int32_t){S2}",
                        6 => $"{S1} <  {S2}",
                        7 => $"{S1} >= {S2}",
                        _ => null
                    };
                    if (c == null) { sb.Append($"pc = 0x{pc:X}u; goto interp;"); return; }
                    sb.Append($"if ({c}) {{ {Jump(t, conditional: true)} }}");   // branch has fall-through escape
                    return;
                }

                case 0x03: // LOAD (inline RAM fast path)
                {
                    string addr = $"(uint32_t)({S1} + {SImmI(IImm(instr))})";
                    string? ld = f3 switch
                    {
                        0 => $"jit_l8s (h, m, {addr})",
                        1 => $"jit_l16s(h, m, {addr})",
                        2 => $"jit_l32 (h, m, {addr})",
                        4 => $"jit_l8u (h, m, {addr})",
                        5 => $"jit_l16u(h, m, {addr})",
                        _ => null
                    };
                    if (ld == null) { sb.Append($"pc = 0x{pc:X}u; goto interp;"); return; }
                    sb.Append(D(ld)); return;
                }
                case 0x23: // STORE (inline RAM fast path)
                {
                    string addr = $"(uint32_t)({S1} + {SImmI(SImm(instr))})";
                    string? st = f3 switch
                    {
                        0 => $"jit_s8 (h, m, {addr}, {S2});",
                        1 => $"jit_s16(h, m, {addr}, {S2});",
                        2 => $"jit_s32(h, m, {addr}, {S2});",
                        _ => null
                    };
                    if (st == null) { sb.Append($"pc = 0x{pc:X}u; goto interp;"); return; }
                    sb.Append(st); return;
                }

                case 0x13: // OP-IMM
                {
                    int imm = IImm(instr);
                    int sh = (int)((instr >> 20) & 0x1F);
                    string? e = f3 switch
                    {
                        // ADDI: unsigned add wraps identically to two's-complement
                        // signed add — drop the int32 round-trip cast.
                        0 => $"{S1} + {SImmI(imm)}",
                        1 => f7 == 0 ? $"{S1} << {sh}" : null,
                        2 => $"((int32_t){S1} < {imm}) ? 1u : 0u",
                        3 => $"({S1} < {SImmU(imm)}) ? 1u : 0u",
                        4 => $"{S1} ^ {SImmU(imm)}",
                        5 => f7 == 0x20 ? $"(uint32_t)((int32_t){S1} >> {sh})"
                                        : (f7 == 0 ? $"{S1} >> {sh}" : null),
                        6 => $"{S1} | {SImmU(imm)}",
                        7 => $"{S1} & {SImmU(imm)}",
                        _ => null
                    };
                    if (e == null) { sb.Append($"pc = 0x{pc:X}u; goto interp;"); return; }
                    sb.Append(D(e)); return;
                }
                case 0x33: // OP (+ M)
                {
                    string? e;
                    if (f7 == 0x01)
                        e = f3 switch
                        {
                            0 => $"(uint32_t)({S1} * {S2})",
                            1 => $"(uint32_t)(((int64_t)(int32_t){S1} * (int64_t)(int32_t){S2}) >> 32)",
                            2 => $"(uint32_t)(((int64_t)(int32_t){S1} * (int64_t)(uint64_t){S2}) >> 32)",
                            3 => $"(uint32_t)(((uint64_t){S1} * (uint64_t){S2}) >> 32)",
                            4 => $"jit_div((int32_t){S1}, (int32_t){S2})",
                            5 => $"jit_divu({S1}, {S2})",
                            6 => $"jit_rem((int32_t){S1}, (int32_t){S2})",
                            7 => $"jit_remu({S1}, {S2})",
                            _ => null
                        };
                    else
                    {
                        bool sub = f7 == 0x20;
                        e = f3 switch
                        {
                            // ADD/SUB: unsigned arithmetic wraps identically to
                            // two's-complement signed — no int32 round-trip.
                            0 => sub ? $"{S1} - {S2}" : $"{S1} + {S2}",
                            1 => $"{S1} << ({S2} & 31)",
                            2 => $"((int32_t){S1} < (int32_t){S2}) ? 1u : 0u",
                            3 => $"({S1} < {S2}) ? 1u : 0u",
                            4 => $"{S1} ^ {S2}",
                            5 => sub ? $"(uint32_t)((int32_t){S1} >> ({S2} & 31))"
                                     : $"{S1} >> ({S2} & 31)",
                            6 => $"{S1} | {S2}",
                            7 => $"{S1} & {S2}",
                            _ => null
                        };
                    }
                    if (e == null) { sb.Append($"pc = 0x{pc:X}u; goto interp;"); return; }
                    sb.Append(D(e)); return;
                }

                case 0x0F: sb.Append("/* FENCE = nop */"); return;                       // FENCE
                default:   sb.Append($"pc = 0x{pc:X}u; goto interp;"); return;            // A/SYSTEM/unknown
            }
        }

        /// <summary>
        /// SLICED translation for LARGE guests (Doom/Linux): emit one small
        /// <c>__device__ __noinline__</c> function per <paramref name="sliceWords"/>-
        /// instruction span plus a <c>jit_dispatch</c> that calls the right slice
        /// by pc. A single 150k-instruction function is impractical for ptxas;
        /// many ~1k-instruction functions compile fine. Guest regs are passed as
        /// <c>uint32_t* R</c> (a per-thread local array) — not in registers across
        /// the call, but fetch/decode/dispatch are still gone, so it's many× the
        /// interpreter. Branches inside a slice chain via <c>goto</c> (hot loops
        /// that fit a slice stay fast); cross-slice/JALR transfers <c>return</c>
        /// the next pc to the kernel dispatch loop. SYSTEM/illegal/A set
        /// <c>h.pc</c> and return the sentinel <c>0xFFFFFFFE</c> (interpreter
        /// fallback). Hang-safe: in-slice back-edges decrement <c>*bl</c> and the
        /// kernel decrements once per dispatch, so every cycle is bounded.
        /// </summary>
        public static string TranslateSliced(byte[] code, uint lo, uint hi, uint sliceWords = 1024)
        {
            var (slices, dispatch, _) = TranslateSlicedParts(code, lo, hi, sliceWords);
            var sb = new StringBuilder();
            foreach (var s in slices) sb.Append(s);
            sb.Append(dispatch);
            return sb.ToString();
        }

        /// <summary>
        /// Slice functions as a LIST (one self-contained <c>__device__</c>
        /// function string each) plus the <c>jit_dispatch</c> function. The list
        /// lets the runtime spread the slices across several <c>.cu</c> files and
        /// compile them as PARALLEL <c>nvcc -dc</c> processes (cicc is single-
        /// threaded, so one giant .cu pins one core; many files use all cores).
        /// Slices have external linkage (no <c>static</c>) so the device linker
        /// resolves the dispatcher's cross-translation-unit calls under -rdc=true.
        /// </summary>
        public static (System.Collections.Generic.List<string> slices, string dispatch, uint nslices)
            TranslateSlicedParts(byte[] code, uint lo, uint hi, uint sliceWords = 1024)
        {
            uint sliceBytes = sliceWords * 4;
            uint nslices = (hi - lo + sliceBytes - 1) / sliceBytes;
            var slices = new System.Collections.Generic.List<string>((int)nslices);
            for (uint s = 0; s < nslices; s++)
            {
                uint sLo = lo + s * sliceBytes;
                uint sHi = Math.Min(sLo + sliceBytes, hi);
                var sb = new StringBuilder();
                sb.Append($"__device__ __noinline__ uint32_t jit_slice_{s}(uint32_t* R, uint32_t pc, Hart& h, CoreMem& m, long long* bl) {{\n");
                sb.Append("  switch (pc) {\n");
                for (uint a = sLo; a + 4 <= sHi; a += 4) sb.Append($"    case 0x{a:X}u: goto L_{a:X};\n");
                sb.Append("    default: return pc;\n  }\n");
                for (uint a = sLo; a + 4 <= sHi; a += 4)
                {
                    uint instr = LE32(code, (int)(a - lo));
                    sb.Append($"  L_{a:X}: ");
                    EmitSliced(sb, a, instr, lo, hi, sLo, sHi);
                    sb.Append('\n');
                }
                sb.Append($"  return 0x{sHi:X}u;\n}}\n");
                slices.Add(sb.ToString());
            }
            var d = new StringBuilder();
            d.Append("__device__ uint32_t jit_dispatch(uint32_t* R, uint32_t pc, Hart& h, CoreMem& m, long long* bl) {\n");
            d.Append($"  uint32_t s = (pc - 0x{lo:X}u) / 0x{sliceBytes:X}u;\n");
            d.Append("  switch (s) {\n");
            for (uint s = 0; s < nslices; s++) d.Append($"    case {s}u: return jit_slice_{s}(R, pc, h, m, bl);\n");
            d.Append("    default: return pc;\n  }\n}\n");
            return (slices, d.ToString(), nslices);
        }

        private static void EmitSliced(StringBuilder sb, uint pc, uint instr, uint lo, uint hi, uint sLo, uint sHi)
        {
            uint op = instr & 0x7F;
            int  rd  = (int)((instr >> 7) & 0x1F);
            int  rs1 = (int)((instr >> 15) & 0x1F);
            int  rs2 = (int)((instr >> 20) & 0x1F);
            uint f3  = (instr >> 12) & 0x7;
            uint f7  = (instr >> 25) & 0x7F;
            uint next = pc + 4;

            string S(int n) => n == 0 ? "0u" : $"R[{n}]";
            string D(string e) => rd == 0 ? $"(void)({e});" : $"R[{rd}] = {e};";
            bool InR(uint t) => t >= lo && t < hi;
            bool InS(uint t) => t >= sLo && t < sHi;
            // Control transfer to a static target. Only chain (goto) to a target
            // that is 4-aligned AND in this slice — those are the only addresses
            // that have an emitted L_ label. Everything else returns the target
            // to the kernel (cross-slice re-dispatch, or an out-of-range /
            // misaligned target that can only come from .rodata decoded as code,
            // which real control flow never actually reaches). In-slice back-edge
            // decrements *bl (hang-safety); forward chains directly.
            bool CanGoto(uint t) => InS(t) && (t & 3u) == 0;
            string Go(uint t) =>
                CanGoto(t) ? (t <= pc ? $"if(--(*bl)>0 && !h.halted) goto L_{t:X}; return 0x{t:X}u;"
                                      : $"goto L_{t:X};")
                           : $"return 0x{t:X}u;";

            switch (op)
            {
                case 0x37: sb.Append(D($"0x{instr & 0xFFFFF000u:X}u")); return;
                case 0x17: sb.Append(D($"0x{pc + (instr & 0xFFFFF000u):X}u")); return;
                case 0x6F:
                {
                    uint t = pc + JImm(instr);
                    if (rd != 0) sb.Append($"R[{rd}] = 0x{next:X}u; ");
                    sb.Append(Go(t));
                    return;
                }
                case 0x67:
                    sb.Append($"{{ uint32_t t = ({S(rs1)} + {SImmI(IImm(instr))}) & ~1u; ");
                    if (rd != 0) sb.Append($"R[{rd}] = 0x{next:X}u; ");
                    sb.Append("return t; }");
                    return;
                case 0x63:
                {
                    uint t = pc + BImm(instr);
                    string? c = f3 switch
                    {
                        0 => $"{S(rs1)} == {S(rs2)}",
                        1 => $"{S(rs1)} != {S(rs2)}",
                        4 => $"(int32_t){S(rs1)} <  (int32_t){S(rs2)}",
                        5 => $"(int32_t){S(rs1)} >= (int32_t){S(rs2)}",
                        6 => $"{S(rs1)} <  {S(rs2)}",
                        7 => $"{S(rs1)} >= {S(rs2)}",
                        _ => null
                    };
                    if (c == null) { sb.Append($"h.pc = 0x{pc:X}u; return 0xFFFFFFFEu;"); return; }
                    sb.Append($"if ({c}) {{ {Go(t)} }}");
                    return;
                }
                case 0x03:
                {
                    string addr = $"(uint32_t)({S(rs1)} + {SImmI(IImm(instr))})";
                    string? ld = f3 switch
                    {
                        0 => $"jit_l8s (h, m, {addr})", 1 => $"jit_l16s(h, m, {addr})",
                        2 => $"jit_l32 (h, m, {addr})", 4 => $"jit_l8u (h, m, {addr})",
                        5 => $"jit_l16u(h, m, {addr})", _ => null
                    };
                    if (ld == null) { sb.Append($"h.pc = 0x{pc:X}u; return 0xFFFFFFFEu;"); return; }
                    sb.Append(D(ld)); return;
                }
                case 0x23:
                {
                    string addr = $"(uint32_t)({S(rs1)} + {SImmI(SImm(instr))})";
                    string? st = f3 switch
                    {
                        0 => $"jit_s8 (h, m, {addr}, {S(rs2)});",
                        1 => $"jit_s16(h, m, {addr}, {S(rs2)});",
                        2 => $"jit_s32(h, m, {addr}, {S(rs2)});",
                        _ => null
                    };
                    if (st == null) { sb.Append($"h.pc = 0x{pc:X}u; return 0xFFFFFFFEu;"); return; }
                    sb.Append(st); return;
                }
                case 0x13:
                {
                    int imm = IImm(instr); int sh = (int)((instr >> 20) & 0x1F);
                    string? e = f3 switch
                    {
                        0 => $"({S(rs1)} + {SImmU(imm)})",
                        1 => f7 == 0 ? $"{S(rs1)} << {sh}" : null,
                        2 => $"((int32_t){S(rs1)} < {imm}) ? 1u : 0u",
                        3 => $"({S(rs1)} < {SImmU(imm)}) ? 1u : 0u",
                        4 => $"{S(rs1)} ^ {SImmU(imm)}",
                        5 => f7 == 0x20 ? $"(uint32_t)((int32_t){S(rs1)} >> {sh})" : (f7 == 0 ? $"{S(rs1)} >> {sh}" : null),
                        6 => $"{S(rs1)} | {SImmU(imm)}",
                        7 => $"{S(rs1)} & {SImmU(imm)}",
                        _ => null
                    };
                    if (e == null) { sb.Append($"h.pc = 0x{pc:X}u; return 0xFFFFFFFEu;"); return; }
                    sb.Append(D(e)); return;
                }
                case 0x33:
                {
                    string? e;
                    if (f7 == 0x01)
                        e = f3 switch
                        {
                            0 => $"({S(rs1)} * {S(rs2)})",
                            1 => $"(uint32_t)(((int64_t)(int32_t){S(rs1)} * (int64_t)(int32_t){S(rs2)}) >> 32)",
                            2 => $"(uint32_t)(((int64_t)(int32_t){S(rs1)} * (int64_t)(uint64_t){S(rs2)}) >> 32)",
                            3 => $"(uint32_t)(((uint64_t){S(rs1)} * (uint64_t){S(rs2)}) >> 32)",
                            4 => $"jit_div((int32_t){S(rs1)}, (int32_t){S(rs2)})",
                            5 => $"jit_divu({S(rs1)}, {S(rs2)})",
                            6 => $"jit_rem((int32_t){S(rs1)}, (int32_t){S(rs2)})",
                            7 => $"jit_remu({S(rs1)}, {S(rs2)})",
                            _ => null
                        };
                    else
                    {
                        bool sub = f7 == 0x20;
                        e = f3 switch
                        {
                            0 => sub ? $"({S(rs1)} - {S(rs2)})" : $"({S(rs1)} + {S(rs2)})",
                            1 => $"{S(rs1)} << ({S(rs2)} & 31)",
                            2 => $"((int32_t){S(rs1)} < (int32_t){S(rs2)}) ? 1u : 0u",
                            3 => $"({S(rs1)} < {S(rs2)}) ? 1u : 0u",
                            4 => $"{S(rs1)} ^ {S(rs2)}",
                            5 => sub ? $"(uint32_t)((int32_t){S(rs1)} >> ({S(rs2)} & 31))" : $"{S(rs1)} >> ({S(rs2)} & 31)",
                            6 => $"{S(rs1)} | {S(rs2)}",
                            7 => $"{S(rs1)} & {S(rs2)}",
                            _ => null
                        };
                    }
                    if (e == null) { sb.Append($"h.pc = 0x{pc:X}u; return 0xFFFFFFFEu;"); return; }
                    sb.Append(D(e)); return;
                }
                case 0x0F: sb.Append("/* FENCE */"); return;
                default:   sb.Append($"h.pc = 0x{pc:X}u; return 0xFFFFFFFEu;"); return;
            }
        }

        // ── RV32I immediate decoders (mirror rv32i_cuda.cu) ──
        private static uint JImm(uint i)
        {
            uint v = ((i>>31)&1u)<<20 | ((i>>12)&0xFFu)<<12 | ((i>>20)&1u)<<11 | ((i>>21)&0x3FFu)<<1;
            return (v & 0x100000u) != 0 ? v | 0xFFE00000u : v;
        }
        private static uint BImm(uint i)
        {
            uint v = ((i>>31)&1u)<<12 | ((i>>7)&1u)<<11 | ((i>>25)&0x3Fu)<<5 | ((i>>8)&0xFu)<<1;
            return (v & 0x1000u) != 0 ? v | 0xFFFFE000u : v;
        }
        private static int IImm(uint i) => (int)i >> 20;
        private static int SImm(uint i) => (int)(i & 0xFE000000) >> 20 | (int)((i >> 7) & 0x1F);

        private static string SImmI(int v) => v < 0 ? $"({v})" : v.ToString();
        private static string SImmU(int v) => $"0x{(uint)v:X}u";

        private static uint LE32(byte[] b, int o) =>
            (uint)(b[o] | (b[o+1] << 8) | (b[o+2] << 16) | (b[o+3] << 24));
    }
}
