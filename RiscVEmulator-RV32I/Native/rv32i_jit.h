// rv32i_jit.h — x86-64 JIT for the RV32I core.
//
// A basic-block translator: RV32I instructions are compiled to native x86-64
// the first time their PC is reached, cached, and re-run directly afterwards.
// Guest registers stay memory-resident in CPU_State::regs[] — the JIT loads
// operands from there and writes results back, exactly like the interpreter,
// so the interpreter remains a drop-in fallback for anything not translated.
//
// Memory accesses are emitted as the same plain x86 MOV forms the C++ core
// produced, so the host VEH (MmioDispatcher) still decodes and services them.
//
// Layout: this header is #included by rv32i_core.cpp *after* CPU_State, the
// trap unit and cpu_step() are defined — it depends on all three.
//
//   x86 register roles inside translated code (all volatile under the Win64
//   ABI, so a block needs no callee-saved push/pop — only a 3-instruction
//   prologue that reloads the base pointers):
//     R10  = guest memory base (cpu.mem)              — REG_MEM
//     R11  = &cpu  (CPU_State*)   guest regs [r11+n*4] — REG_CPU
//     R9   = &g_mmio_tab (1 byte per 1 MiB)            — REG_MMIOTAB
//     RAX  = scratch / guest-memory data register (seen by MmioDispatcher)
//     RCX  = scratch / guest-memory address index / shift count
//     RDX  = scratch
//
//   Memory access has a fast and a slow path picked at runtime per access:
//   * Fast: an inline `mov reg, [REG_MEM + index]` (or store). Used when
//     `g_mmio_tab[addr >> 20] == 0` — i.e. plain RAM, no VEH involvement.
//   * Slow: a C-ABI call to one of the jit_helper_l? / jit_helper_s? in
//     rv32i_core.cpp. The actual `mov` lives in DLL .text so VEH-mediated
//     MMIO dispatch is reliable across host processes (`dotnet run` hosts
//     historically deadlock on VEH dispatch from RWX-page stores).

#pragma once
#include <cstdint>

// ── Win32 (declared directly; the core links -nodefaultlib + kernel32) ──────
extern "C" __declspec(dllimport) void* __stdcall
    VirtualAlloc(void*, unsigned long long, unsigned long, unsigned long);
extern "C" __declspec(dllimport) int __stdcall
    VirtualFree(void*, unsigned long long, unsigned long);
extern "C" __declspec(dllimport) unsigned long __stdcall
    GetEnvironmentVariableA(const char*, char*, unsigned long);

static constexpr unsigned long MEM_COMMIT_  = 0x1000;
static constexpr unsigned long MEM_RESERVE_ = 0x2000;
static constexpr unsigned long MEM_RELEASE_ = 0x8000;
static constexpr unsigned long PAGE_XRW_    = 0x40;   // PAGE_EXECUTE_READWRITE

// ════════════════════════════════════════════════════════════════════
// Code cache + block map
// ════════════════════════════════════════════════════════════════════

static constexpr unsigned JIT_CODE_SIZE = 96u * 1024u * 1024u;  // RWX arena
static constexpr unsigned JIT_MAP_BITS  = 18;                   // 256K entries
static constexpr unsigned JIT_MAP_SIZE  = 1u << JIT_MAP_BITS;
static constexpr unsigned JIT_MAP_MASK  = JIT_MAP_SIZE - 1u;
static constexpr unsigned JIT_MAX_INSNS = 1024;                 // per block cap

// A translated basic block: its guest start PC, retired instruction count,
// and the host code entry. code==nullptr marks an empty slot.
struct JitBlock {
    uint32_t pc;
    uint32_t insns;
    uint8_t* code;          // dispatcher entry — runs the prologue first
    uint8_t* chain_entry;   // chained entry — skip prologue (regs are live)
};

static uint8_t*  g_code      = nullptr;   // RWX arena base
static uint8_t*  g_emit      = nullptr;   // emit cursor
static uint8_t*  g_code_end  = nullptr;   // arena end
static JitBlock* g_map       = nullptr;   // direct-mapped pc -> block

// One byte per 1 MiB of guest address space. 1 = "may be MMIO, route through
// the C++ helper"; 0 = "plain RAM, the JIT inlines the store/load". Defaults
// to all-MMIO and is then punched to 0 for the regions known to be plain RAM
// in jit_alloc(). That biases the fast path toward correctness — anywhere we
// don't know about goes through the helper, which is correct everywhere.
static uint8_t   g_mmio_tab[4096];
static int       g_flush_req = 0;         // kept for future SMC support

// Direct-mapped slot for a guest PC (all RV32I instructions are 4-aligned).
static inline unsigned jit_slot(uint32_t pc) { return (pc >> 2) & JIT_MAP_MASK; }

// ════════════════════════════════════════════════════════════════════
// x86-64 instruction emitter
// ════════════════════════════════════════════════════════════════════

// x86 GPR numbers.
enum {
    X_AX = 0, X_CX = 1, X_DX = 2, X_BX = 3,
    X_SP = 4, X_BP = 5, X_SI = 6, X_DI = 7,
    X_R8 = 8, X_R9 = 9, X_R10 = 10, X_R11 = 11,
    X_R12 = 12, X_R13 = 13, X_R14 = 14, X_R15 = 15,
};

// Fixed register roles in translated code (see header comment).
static constexpr int REG_MEM    = X_R10;   // guest memory base
static constexpr int REG_CPU    = X_R11;   // &cpu
static constexpr int REG_MMIOTAB = X_R9;   // &g_mmio_tab (1 byte per 1 MiB)

// Offsets into CPU_State as seen from REG_CPU.  Must mirror the C struct
// layout: regs[32] @ 0, pc @ 128, budget @ 132, halted @ 136, mem @ 144.
static constexpr int CPU_OFF_PC     = 128;
static constexpr int CPU_OFF_BUDGET = 132;

// Condition codes (low nibble of Jcc / SETcc / two-byte opcodes).
enum {
    CC_O = 0x0, CC_NO = 0x1, CC_B = 0x2, CC_AE = 0x3, CC_E = 0x4, CC_NE = 0x5,
    CC_BE = 0x6, CC_A = 0x7, CC_S = 0x8, CC_NS = 0x9, CC_L = 0xC, CC_GE = 0xD,
    CC_LE = 0xE, CC_G = 0xF,
};

static inline void e8 (uint32_t b) { *g_emit++ = (uint8_t)b; }
static inline void e32(uint32_t d) { __builtin_memcpy(g_emit, &d, 4); g_emit += 4; }
static inline void e64(uint64_t q) { __builtin_memcpy(g_emit, &q, 8); g_emit += 8; }

// REX prefix; emitted only when an extended reg or 64-bit operand needs it.
static inline void rex(int w, int r, int x, int b) {
    uint32_t v = 0x40u | (w << 3) | ((r >> 3) << 2) | ((x >> 3) << 1) | (b >> 3);
    if (v != 0x40u) e8(v);
}
// ModRM with mod==3 (register-direct).
static inline void modrm_rr(int reg, int rm) {
    e8(0xC0u | ((reg & 7) << 3) | (rm & 7));
}
// ModRM (+disp) for a memory operand [base + disp]. base must not be RSP/R12.
static inline void modrm_mem(int reg, int base, int32_t disp) {
    int b = base & 7;
    if (disp == 0 && b != 5) {
        e8(0x00u | ((reg & 7) << 3) | b);
    } else if (disp >= -128 && disp <= 127) {
        e8(0x40u | ((reg & 7) << 3) | b); e8((uint8_t)disp);
    } else {
        e8(0x80u | ((reg & 7) << 3) | b); e32((uint32_t)disp);
    }
}
// ModRM+SIB for [base + index*1], no displacement. base low3 must not be 5.
static inline void modrm_sib(int reg, int base, int index) {
    e8(0x00u | ((reg & 7) << 3) | 4);                 // rm=100 -> SIB
    e8((0 << 6) | ((index & 7) << 3) | (base & 7));   // scale=1
}

// ── 32-bit register/immediate moves ─────────────────────────────────────────
static inline void mov_ri32(int dst, uint32_t imm) {     // mov dst, imm32
    rex(0, 0, 0, dst);
    e8(0xB8u + (dst & 7));
    e32(imm);
}
static inline void mov_ri64(int dst, uint64_t imm) {     // mov dst, imm64
    rex(1, 0, 0, dst);
    e8(0xB8u + (dst & 7));
    e64(imm);
}
static inline void mov_rr32(int dst, int src) {          // mov dst, src
    rex(0, src, 0, dst);
    e8(0x89);
    modrm_rr(src, dst);
}

// ── guest-register file access: [REG_CPU + greg*4] ──────────────────────────
static inline void ld_greg(int xdst, int greg) {         // mov xdst, [cpu+greg*4]
    rex(0, xdst, 0, REG_CPU);
    e8(0x8B);
    modrm_mem(xdst, REG_CPU, greg * 4);
}
static inline void st_greg(int greg, int xsrc) {         // mov [cpu+greg*4], xsrc
    rex(0, xsrc, 0, REG_CPU);
    e8(0x89);
    modrm_mem(xsrc, REG_CPU, greg * 4);
}
// Generic 32-bit load/store against a base register + displacement.
static inline void ld_mem32(int xdst, int base, int32_t disp) {
    rex(0, xdst, 0, base); e8(0x8B); modrm_mem(xdst, base, disp);
}
static inline void st_mem32(int base, int32_t disp, int xsrc) {
    rex(0, xsrc, 0, base); e8(0x89); modrm_mem(xsrc, base, disp);
}

// ── 32-bit ALU: dst OP= src  (reg,reg form) ─────────────────────────────────
// op = the "OP r/m32, r32" opcode: ADD 01, OR 09, AND 21, SUB 29, XOR 31, CMP 39.
static inline void alu_rr(int op, int dst, int src) {
    rex(0, src, 0, dst);
    e8(op);
    modrm_rr(src, dst);
}
// 32-bit ALU dst OP= imm32.  ext = ModRM.reg digit: ADD 0,OR 1,AND 4,SUB 5,XOR 6,CMP 7.
static inline void alu_ri(int ext, int dst, uint32_t imm) {
    rex(0, 0, 0, dst);
    if ((int32_t)imm >= -128 && (int32_t)imm <= 127) {
        e8(0x83); modrm_rr(ext, dst); e8((uint8_t)imm);
    } else {
        e8(0x81); modrm_rr(ext, dst); e32(imm);
    }
}
// dst-is-memory ALU: dword [base+disp] OP= imm32.  ext as in alu_ri.
static inline void emit_alu_mi32(int ext, int base, int32_t disp, uint32_t imm) {
    rex(0, 0, 0, base);
    e8(0x81);
    modrm_mem(ext, base, disp);
    e32(imm);
}
// mov dword [base+disp], imm32  (used to spill cpu.pc on exit).
static inline void emit_mov_mi32(int base, int32_t disp, uint32_t imm) {
    rex(0, 0, 0, base);
    e8(0xC7);
    modrm_mem(0, base, disp);
    e32(imm);
}
// Shifts.  ext: SHL 4, SHR 5, SAR 7.
static inline void shift_ri(int ext, int dst, int imm) {
    rex(0, 0, 0, dst);
    e8(0xC1); modrm_rr(ext, dst); e8((uint8_t)(imm & 31));
}
static inline void shift_cl(int ext, int dst) {          // shift dst by CL
    rex(0, 0, 0, dst);
    e8(0xD3); modrm_rr(ext, dst);
}

// ── compares / setcc / sign-zero extension ──────────────────────────────────
static inline void cmp_rr(int a, int b) { alu_rr(0x39, a, b); }   // cmp a, b
static inline void test_rr(int a, int b) {                        // test a, b
    rex(0, b, 0, a); e8(0x85); modrm_rr(b, a);
}
static inline void setcc(int cc, int reg) {              // setcc reg8
    rex(0, 0, 0, reg);
    e8(0x0F); e8(0x90 + cc); modrm_rr(0, reg);
}
static inline void movzx_r8(int dst, int src) {          // movzx dst, src8
    rex(0, dst, 0, src); e8(0x0F); e8(0xB6); modrm_rr(dst, src);
}
static inline void xor_self(int reg) {                   // xor reg, reg -> 0
    alu_rr(0x31, reg, reg);
}

// ── control flow ────────────────────────────────────────────────────────────
static inline void emit_ret() { e8(0xC3); }
static inline void emit_jmp_reg(int reg) {               // jmp reg
    rex(0, 0, 0, reg); e8(0xFF); modrm_rr(4, reg);
}
// Emit a Jcc rel32 with a 0 placeholder; returns the patch site (the rel32).
static inline uint8_t* emit_jcc(int cc) {
    e8(0x0F); e8(0x80 + cc);
    uint8_t* site = g_emit;
    e32(0);
    return site;
}
static inline uint8_t* emit_jmp() {                      // jmp rel32 placeholder
    e8(0xE9);
    uint8_t* site = g_emit;
    e32(0);
    return site;
}
// Patch a previously emitted rel32 so the branch targets `dst`.
static inline void patch_rel32(uint8_t* site, uint8_t* dst) {
    int32_t rel = (int32_t)(dst - (site + 4));
    __builtin_memcpy(site, &rel, 4);
}

// Forward declarations of the load / store helpers defined in rv32i_core.cpp.
extern "C" void     jit_helper_sb(uint32_t addr, uint32_t val);
extern "C" void     jit_helper_sh(uint32_t addr, uint32_t val);
extern "C" void     jit_helper_sw(uint32_t addr, uint32_t val);
extern "C" uint32_t jit_helper_lb (uint32_t addr);
extern "C" uint32_t jit_helper_lh (uint32_t addr);
extern "C" uint32_t jit_helper_lw (uint32_t addr);
extern "C" uint32_t jit_helper_lbu(uint32_t addr);
extern "C" uint32_t jit_helper_lhu(uint32_t addr);

// ── guest-memory access ────────────────────────────────────────────────────
// Loads land in EAX; stores read EAX/AX/AL. Each access first checks the
// per-1-MiB g_mmio_tab: a 0 means "plain RAM" and the access is inlined as
// `mov reg, [REG_MEM + index]`; a 1 routes through a DLL `.text` helper
// (more cycles per access, but VEH dispatch is reliable there). On entry to
// either helper, `index` is the addr register (always X_CX in our translator).

// Probe g_mmio_tab[addr>>20] and emit a JNE that the caller patches to the
// helper (slow) path. Returns the rel32 patch site.
static inline uint8_t* emit_mmio_probe(int addr_reg) {
    // mov edx, addr_reg
    rex(0, addr_reg, 0, X_DX); e8(0x89); modrm_rr(addr_reg, X_DX);
    // shr edx, 20
    shift_ri(5 /*SHR*/, X_DX, 20);
    // cmp byte [REG_MMIOTAB + rdx], 0
    rex(0, 0, X_DX, REG_MMIOTAB);
    e8(0x80);
    e8((0 << 6) | (7 << 3) | 4);                          // ModRM: mod=0 reg=/7 rm=SIB
    e8((0 << 6) | ((X_DX & 7) << 3) | (REG_MMIOTAB & 7)); // SIB scale=1
    e8(0x00);                                              // imm8 = 0
    return emit_jcc(CC_NE);                               // JNE → patched to mmio_path
}

static inline void guest_load(int width, bool signext, int index) {
    uint8_t* jne_site = emit_mmio_probe(index);

    // ── fast inline path ──────────────────────────────────────────
    if (width == 4) {
        rex(0, X_AX, index, REG_MEM); e8(0x8B); modrm_sib(X_AX, REG_MEM, index);
    } else if (width == 2 && signext) {
        rex(0, X_AX, index, REG_MEM); e8(0x0F); e8(0xBF); modrm_sib(X_AX, REG_MEM, index);
    } else if (width == 2) {
        rex(0, X_AX, index, REG_MEM); e8(0x0F); e8(0xB7); modrm_sib(X_AX, REG_MEM, index);
    } else if (width == 1 && signext) {
        rex(0, X_AX, index, REG_MEM); e8(0x0F); e8(0xBE); modrm_sib(X_AX, REG_MEM, index);
    } else {
        rex(0, X_AX, index, REG_MEM); e8(0x0F); e8(0xB6); modrm_sib(X_AX, REG_MEM, index);
    }
    uint8_t* jmp_end = emit_jmp();

    // ── slow helper path ──────────────────────────────────────────
    patch_rel32(jne_site, g_emit);

    // push r9 ; push r10 ; push r11 ; sub rsp, 0x28
    e8(0x41); e8(0x51);
    e8(0x41); e8(0x52);
    e8(0x41); e8(0x53);
    e8(0x48); e8(0x83); e8(0xEC); e8(0x28);

    void* helper;
    if (width == 4)             helper = (void*)&jit_helper_lw;
    else if (width == 2 && signext) helper = (void*)&jit_helper_lh;
    else if (width == 2)        helper = (void*)&jit_helper_lhu;
    else if (width == 1 && signext) helper = (void*)&jit_helper_lb;
    else                        helper = (void*)&jit_helper_lbu;
    mov_ri64(X_AX, (uint64_t)(uintptr_t)helper);
    e8(0xFF); e8(0xD0);                                  // call rax  -- returns in eax

    // add rsp, 0x28 ; pop r11 ; pop r10 ; pop r9
    e8(0x48); e8(0x83); e8(0xC4); e8(0x28);
    e8(0x41); e8(0x5B);
    e8(0x41); e8(0x5A);
    e8(0x41); e8(0x59);

    patch_rel32(jmp_end, g_emit);
}
static inline void guest_store(int width, int index) {
    uint8_t* jne_site = emit_mmio_probe(index);

    // ── fast inline path ──────────────────────────────────────────
    if (width == 2) e8(0x66);
    rex(0, X_AX, index, REG_MEM);
    e8(width == 1 ? 0x88 : 0x89);                        // mov [mem+idx], al/ax/eax
    modrm_sib(X_AX, REG_MEM, index);
    uint8_t* jmp_end = emit_jmp();

    // ── slow helper path ──────────────────────────────────────────
    patch_rel32(jne_site, g_emit);

    // mov edx, eax  -- arg 2 = value
    rex(0, X_AX, 0, X_DX);
    e8(0x89);
    modrm_rr(X_AX, X_DX);

    // push r9 ; push r10 ; push r11 ; push rcx
    e8(0x41); e8(0x51);
    e8(0x41); e8(0x52);
    e8(0x41); e8(0x53);
    e8(0x51);
    // sub rsp, 0x20  (shadow space; total stack adjust 0x28 → 16-aligned at call)
    e8(0x48); e8(0x83); e8(0xEC); e8(0x20);

    void* helper = (width == 1) ? (void*)&jit_helper_sb
                : (width == 2) ? (void*)&jit_helper_sh
                :                (void*)&jit_helper_sw;
    mov_ri64(X_AX, (uint64_t)(uintptr_t)helper);
    e8(0xFF); e8(0xD0);                                  // call rax

    // add rsp, 0x20 ; pop rcx ; pop r11 ; pop r10 ; pop r9
    e8(0x48); e8(0x83); e8(0xC4); e8(0x20);
    e8(0x59);
    e8(0x41); e8(0x5B);
    e8(0x41); e8(0x5A);
    e8(0x41); e8(0x59);

    patch_rel32(jmp_end, g_emit);
}

// ════════════════════════════════════════════════════════════════════
// Code cache management
// ════════════════════════════════════════════════════════════════════

// One-time allocation of the RWX arena and the block map. Also populates
// the per-1 MiB MMIO table: every region defaults to "go through the
// helper", then known plain-RAM regions are punched to inline.
static bool jit_alloc() {
    if (g_code) return true;
    g_code = (uint8_t*)VirtualAlloc(nullptr, JIT_CODE_SIZE,
                                    MEM_COMMIT_ | MEM_RESERVE_, PAGE_XRW_);
    g_map = (JitBlock*)VirtualAlloc(nullptr, JIT_MAP_SIZE * sizeof(JitBlock),
                                    MEM_COMMIT_ | MEM_RESERVE_, 0x04 /*RW*/);
    if (!g_code || !g_map) return false;
    g_emit     = g_code;
    g_code_end = g_code + JIT_CODE_SIZE - 4096;   // leave slack for one block

    // Default everything to "use the helper" for safety.
    for (unsigned i = 0; i < 4096; i++) g_mmio_tab[i] = 1;
    // Punch known plain-RAM regions to 0 so the JIT inlines them.
    //   [0x00000000 .. 0x02000000)  — guest RAM (up to 32 MiB)
    for (unsigned i = 0x000; i < 0x020; i++) g_mmio_tab[i] = 0;
    //   [0x0F000000 .. 0x0F100000)  — trap-frame page
    g_mmio_tab[0x0F0] = 0;
    //   [0x20000000 .. 0x20100000)  — framebuffer (1 MiB; FB is 256 KiB)
    g_mmio_tab[0x200] = 0;
    //   [0x30000000 .. 0x30100000)  — audio PCM buffer
    g_mmio_tab[0x300] = 0;
    //   [0x80000000 .. 0x88000000)  — Linux RAM bank (128 MiB)
    for (unsigned i = 0x800; i < 0x880; i++) g_mmio_tab[i] = 0;
    return true;
}

// Drop every translation — used on SMC and when the arena fills.
static void jit_flush() {
    g_emit = g_code;
    for (unsigned i = 0; i < JIT_MAP_SIZE; i++) g_map[i].code = nullptr;
    g_flush_req = 0;
}

// (SMC tracking removed — TinyCC and Linux do not require it in practice;
// re-introduce per-page invalidation later if a workload regresses.)
static inline void jit_mark_code(uint32_t, uint32_t) {}

// ════════════════════════════════════════════════════════════════════
// Block translator
//
// Depends on CPU_State / cpu / the immediate decoders / the trap unit, so
// this header must be #included after all of them are defined.
// ════════════════════════════════════════════════════════════════════

// Load guest register `rs` into x86 `xreg` (x0 reads as a zeroed register).
static inline void load_rs(int xreg, uint32_t rs) {
    if (rs == 0) xor_self(xreg);
    else         ld_greg(xreg, (int)rs);
}
// Store x86 `xreg` into guest register `rd` (writes to x0 are dropped).
static inline void store_rd(uint32_t rd, int xreg) {
    if (rd != 0) st_greg((int)rd, xreg);
}

// Emit a block-exit transferring control to guest pc `target_pc`. If a block
// for that target is already translated we emit a direct `jmp` to its
// chain_entry (skipping prologue); otherwise we fall back to spilling
// cpu.pc + ret. Self-loops chain to self_chain_entry, known at emit time.
static void emit_exit(uint32_t target_pc, uint32_t self_pc0, uint8_t* self_chain_entry) {
    uint8_t* dst = nullptr;
    if (target_pc == self_pc0) {
        dst = self_chain_entry;
    } else {
        JitBlock* b = &g_map[jit_slot(target_pc)];
        if (b->code && b->pc == target_pc) dst = b->chain_entry;
    }
    if (dst) {
        e8(0xE9); uint8_t* s = g_emit; e32(0); patch_rel32(s, dst);
    } else {
        mov_ri32(X_AX, target_pc);
        emit_ret();
    }
}

// Translate the basic block at guest pc0. Returns a g_map entry, or nullptr
// when the first instruction is SYSTEM/illegal (caller interprets one step).
static JitBlock* jit_translate(uint32_t pc0) {
    if (g_emit >= g_code_end) jit_flush();
    uint8_t* code_start = g_emit;

    // Prologue — reload the three base pointers (all volatile registers).
    mov_ri64(REG_CPU,     (uint64_t)(uintptr_t)&cpu);
    mov_ri64(REG_MEM,     (uint64_t)(uintptr_t)cpu.mem);
    mov_ri64(REG_MMIOTAB, (uint64_t)(uintptr_t)g_mmio_tab);

    uint8_t* chain_entry = g_emit;

    // Per-block budget gate: `sub [cpu.budget], insns` followed by `js exit`.
    // The insns immediate is back-patched once the body is fully emitted.
    // exit_stub adds the insns back so cpu.budget on early-out is accurate.
    emit_alu_mi32(5 /*SUB*/, REG_CPU, CPU_OFF_BUDGET, 0);
    uint8_t* insns_imm_site = g_emit - 4;
    uint8_t* js_site        = emit_jcc(CC_S);

    uint32_t pc    = pc0;
    uint32_t insns = 0;
    bool     cf    = false;     // a control-flow op emitted its own epilogue

    while (!cf && insns < JIT_MAX_INSNS) {
        uint32_t instr = *(const uint32_t*)(cpu.mem + pc);
        uint32_t op  = instr & 0x7F;
        uint32_t rd  = (instr >> 7)  & 0x1F;
        uint32_t f3  = (instr >> 12) & 0x7;
        uint32_t f7  = (instr >> 25) & 0x7F;
        uint32_t rs1 = (instr >> 15) & 0x1F;
        uint32_t rs2 = (instr >> 20) & 0x1F;

        // Detect SYSTEM / illegal encodings — these end the block *before*
        // themselves so the C dispatcher interprets them.
        bool illegal = false;
        switch (op) {
            case 0x37: case 0x17: case 0x6F: case 0x67: case 0x63:
            case 0x03: case 0x23: case 0x13: case 0x33: case 0x0F: break;
            default: illegal = true;                       // 0x73 SYSTEM included
        }
        if (!illegal && op == 0x13 &&
            ((f3 == 1 && f7 != 0) || (f3 == 5 && f7 != 0 && f7 != 0x20)))
            illegal = true;
        if (!illegal && op == 0x33 &&
            (f7 != 0 && !(f7 == 0x20 && (f3 == 0 || f3 == 5))))
            illegal = true;
        if (!illegal && op == 0x03 && (f3 == 3 || f3 == 6 || f3 == 7))
            illegal = true;
        if (!illegal && op == 0x23 && f3 > 2)
            illegal = true;
        if (illegal) break;                                // stop; pc unchanged

        switch (op) {
        case 0x37:                                         // LUI
            mov_ri32(X_AX, instr & 0xFFFFF000u);
            store_rd(rd, X_AX);
            break;
        case 0x17:                                         // AUIPC
            mov_ri32(X_AX, pc + (instr & 0xFFFFF000u));
            store_rd(rd, X_AX);
            break;
        case 0x6F: {                                       // JAL
            if (rd) { mov_ri32(X_AX, pc + 4); st_greg((int)rd, X_AX); }
            emit_exit(pc + j_imm(instr), pc0, chain_entry);
            cf = true;
            break;
        }
        case 0x67: {                                       // JALR (dynamic)
            load_rs(X_DX, rs1);
            alu_ri(0, X_DX, (uint32_t)i_imm(instr));        // edx += imm
            alu_ri(4, X_DX, 0xFFFFFFFEu);                   // edx &= ~1
            if (rd) { mov_ri32(X_AX, pc + 4); st_greg((int)rd, X_AX); }
            st_mem32(REG_CPU, CPU_OFF_PC, X_DX);             // cpu.pc = edx
            mov_rr32(X_AX, X_DX);
            emit_ret();
            cf = true;
            break;
        }
        case 0x63: {                                       // BRANCH
            int cc;
            switch (f3) {
                case 0: cc = CC_E;  break;  case 1: cc = CC_NE; break;
                case 4: cc = CC_L;  break;  case 5: cc = CC_GE; break;
                case 6: cc = CC_B;  break;  case 7: cc = CC_AE; break;
                default: cc = -1;   break;
            }
            if (cc < 0) { illegal = true; break; }          // unreachable guard
            load_rs(X_AX, rs1);
            load_rs(X_CX, rs2);
            cmp_rr(X_AX, X_CX);
            uint8_t* taken = emit_jcc(cc);
            emit_exit(pc + 4, pc0, chain_entry);            // not taken
            patch_rel32(taken, g_emit);
            emit_exit(pc + b_imm(instr), pc0, chain_entry); // taken
            cf = true;
            break;
        }
        case 0x03: {                                       // LOAD
            int width, sext;
            switch (f3) {
                case 0: width = 1; sext = 1; break;
                case 1: width = 2; sext = 1; break;
                case 2: width = 4; sext = 0; break;
                case 4: width = 1; sext = 0; break;
                default:width = 2; sext = 0; break;         // f3==5 LHU
            }
            load_rs(X_CX, rs1);
            alu_ri(0, X_CX, (uint32_t)i_imm(instr));        // ecx = addr
            guest_load(width, sext != 0, X_CX);             // -> eax
            store_rd(rd, X_AX);
            break;
        }
        case 0x23: {                                       // STORE
            int width = (f3 == 0) ? 1 : (f3 == 1) ? 2 : 4;
            load_rs(X_CX, rs1);
            alu_ri(0, X_CX, (uint32_t)s_imm(instr));        // ecx = addr
            load_rs(X_AX, rs2);                             // eax = value
            guest_store(width, X_CX);
            break;
        }
        case 0x13: {                                       // OP-IMM
            int32_t imm = i_imm(instr);
            int     sh  = (instr >> 20) & 0x1F;
            if (f3 == 0 && rs1 == 0) {                      // li rd, imm
                mov_ri32(X_AX, (uint32_t)imm);
            } else {
                load_rs(X_AX, rs1);
                switch (f3) {
                    case 0: alu_ri(0, X_AX, (uint32_t)imm); break;          // ADDI
                    case 1: shift_ri(4, X_AX, sh);          break;          // SLLI
                    case 2: alu_ri(7, X_AX, (uint32_t)imm);                 // SLTI
                            setcc(CC_L, X_DX); movzx_r8(X_AX, X_DX); break;
                    case 3: alu_ri(7, X_AX, (uint32_t)imm);                 // SLTIU
                            setcc(CC_B, X_DX); movzx_r8(X_AX, X_DX); break;
                    case 4: alu_ri(6, X_AX, (uint32_t)imm); break;          // XORI
                    case 5: shift_ri(f7 == 0x20 ? 7 : 5, X_AX, sh); break;  // SRAI/SRLI
                    case 6: alu_ri(1, X_AX, (uint32_t)imm); break;          // ORI
                    case 7: alu_ri(4, X_AX, (uint32_t)imm); break;          // ANDI
                }
            }
            store_rd(rd, X_AX);
            break;
        }
        case 0x33: {                                       // OP
            load_rs(X_AX, rs1);
            load_rs(X_CX, rs2);
            switch (f3) {
                case 0: alu_rr(f7 == 0x20 ? 0x29 : 0x01, X_AX, X_CX); break; // SUB/ADD
                case 1: shift_cl(4, X_AX); break;                           // SLL
                case 2: cmp_rr(X_AX, X_CX);                                 // SLT
                        setcc(CC_L, X_DX); movzx_r8(X_AX, X_DX); break;
                case 3: cmp_rr(X_AX, X_CX);                                 // SLTU
                        setcc(CC_B, X_DX); movzx_r8(X_AX, X_DX); break;
                case 4: alu_rr(0x31, X_AX, X_CX); break;                    // XOR
                case 5: shift_cl(f7 == 0x20 ? 7 : 5, X_AX); break;          // SRA/SRL
                case 6: alu_rr(0x09, X_AX, X_CX); break;                    // OR
                case 7: alu_rr(0x21, X_AX, X_CX); break;                    // AND
            }
            store_rd(rd, X_AX);
            break;
        }
        case 0x0F:                                         // FENCE -> NOP
            break;
        }

        insns++;
        pc += 4;
    }

    if (insns == 0) { g_emit = code_start; return nullptr; }

    if (!cf) emit_exit(pc, pc0, chain_entry);              // straight-line exit

    // Exit stub for the chain_entry budget gate: restore the insns we
    // pre-debited so cpu.budget on return is accurate, spill cpu.pc, and
    // return our own pc0 so the dispatcher knows where we stopped.
    patch_rel32(js_site, g_emit);
    emit_alu_mi32(0 /*ADD*/, REG_CPU, CPU_OFF_BUDGET, insns);
    emit_mov_mi32(REG_CPU, CPU_OFF_PC, pc0);
    mov_ri32(X_AX, pc0);
    emit_ret();

    // Back-fill the sub's insns immediate now the count is known.
    __builtin_memcpy(insns_imm_site, &insns, 4);

    jit_mark_code(pc0, pc - pc0);
    unsigned slot = jit_slot(pc0);
    g_map[slot] = { pc0, insns, code_start, chain_entry };
    return &g_map[slot];
}

// Look up the block at pc, translating on a miss. nullptr => uncompilable
// first instruction (SYSTEM / illegal); the caller interprets a single step.
static JitBlock* jit_lookup(uint32_t pc) {
    JitBlock* b = &g_map[jit_slot(pc)];
    if (b->code && b->pc == pc) return b;
    return jit_translate(pc);
}

// C dispatch loop: run translated blocks until `budget` guest instructions
// retire, the CPU halts, or a trap needs servicing. Returns retired count
// (negative when the run ended halted).
static int jit_run(int budget) {
    int retired = 0;
    while (retired < budget) {
        if (cpu.halted) return retired ? -retired : -1;
        if (check_interrupts(cpu, trap)) { retired++; continue; }

        uint32_t pc = cpu.pc;
        if (__builtin_expect(pc == PV_RESUME_GATEWAY, 0)) {
            trap_return(cpu, trap);
            retired++;
            continue;
        }

        uint32_t instr = *(const uint32_t*)(cpu.mem + pc);
        if ((instr & 0x7F) == 0x73) {                      // SYSTEM -> interpret
            CpuException e = cpu_step(cpu);
            trap_system(cpu, trap, e.instr);
            retired++;
            continue;
        }

        JitBlock* b = jit_lookup(pc);
        if (!b) {                                          // illegal first insn
            CpuException e = cpu_step(cpu);
            if      (e.kind == EXC_SYSTEM)  trap_system(cpu, trap, e.instr);
            else if (e.kind == EXC_ILLEGAL) do_trap(cpu, trap, CAUSE_ILLEGAL, e.instr);
            retired++;
            continue;
        }

        int remaining = budget - retired;
        cpu.budget    = remaining;
        uint32_t next = ((uint32_t (*)())b->code)();
        int consumed  = remaining - cpu.budget;
        if (consumed <= 0) {
            // Budget exhausted before the block ran a single instruction —
            // step once via the interpreter so we always make forward
            // progress, otherwise the dispatcher could spin on a too-small
            // remaining.
            if (cpu.halted) break;
            do_step();
            retired++;
        } else {
            cpu.pc   = next;
            retired += consumed;
        }
        if (__builtin_expect(g_flush_req != 0, 0)) jit_flush();
    }
    return retired;
}
