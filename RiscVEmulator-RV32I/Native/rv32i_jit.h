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

// Windows x64 SEH unwind-info registration. Lets Windows (and therefore
// the .NET GC stack walker) traverse JIT-emitted code frames cleanly
// when an exception fires inside the arena. Without this, GC firing
// from a VEH callback whose RIP is in JIT code corrupts the unwind
// state and the process fast-fails with STATUS_STACK_BUFFER_OVERRUN.
struct RUNTIME_FUNCTION_X64 {
    unsigned int BeginAddress;
    unsigned int EndAddress;
    unsigned int UnwindInfoAddress;
};
extern "C" __declspec(dllimport) unsigned char __stdcall
    RtlAddFunctionTable(RUNTIME_FUNCTION_X64*, unsigned long, unsigned long long);
extern "C" __declspec(dllimport) unsigned char __stdcall
    RtlDeleteFunctionTable(RUNTIME_FUNCTION_X64*);

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
static uint8_t*  g_block_region_start = nullptr;  // first byte after stubs
static JitBlock* g_map       = nullptr;   // direct-mapped pc -> block

// One byte per 1 MiB of guest address space. 1 = "may be MMIO, route through
// the C++ helper"; 0 = "plain RAM, the JIT inlines the store/load". Defaults
// to all-MMIO and is then punched to 0 for the regions known to be plain RAM
// in jit_alloc(). That biases the fast path toward correctness — anywhere we
// don't know about goes through the helper, which is correct everywhere.
static uint8_t   g_mmio_tab[4096];
static int       g_flush_req = 0;         // kept for future SMC support

// ─── SSE softfloat shortcut ─────────────────────────────────────────────
// When the JIT starts translating a basic block whose entry PC matches a
// known softfloat ABI symbol (__addsf3, __mulsf3, ...), it emits inline
// host x86 SSE instructions for the operation, writes the result back
// into the guest a0 register, and returns through ra (x1). Each call
// collapses from ~500 RV32 instructions of Bellard's softfp body to a
// handful of host x86 ops.
//
// Hook registration happens at ELF load time from C# via
// rv32i_set_softfloat_hook(op, pc) once the ELF symbol table is parsed.
// 0 disables the hook for that op (e.g. symbol not present).
enum {
    SF_NONE = 0,
    // f32: a in a0(=x10), b in a1(=x11), return in a0
    SF_ADDSF3, SF_SUBSF3, SF_MULSF3, SF_DIVSF3,
    // f64: a in (a0,a1)=(x10,x11), b in (a2,a3)=(x12,x13),
    //      return in (a0,a1)
    SF_ADDDF3, SF_SUBDF3, SF_MULDF3, SF_DIVDF3,
    SF_OP_COUNT
};
static uint32_t g_sf_hook_pc[SF_OP_COUNT];     // 0 = not installed

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

// ── SSE primitives, used by the softfloat shortcut ──────────────────────────
// `xreg` is 0..15 — only the low 8 (xmm0..xmm7) used today, but the encoders
// handle the REX.R bit so xmm8+ would just work.
// movss xmm, [base+disp]: load 32-bit float from memory into xmm low dword.
static inline void movss_load(int xreg, int base, int32_t disp) {
    e8(0xF3); rex(0, xreg, 0, base); e8(0x0F); e8(0x10); modrm_mem(xreg, base, disp);
}
static inline void movss_store(int xreg, int base, int32_t disp) {
    e8(0xF3); rex(0, xreg, 0, base); e8(0x0F); e8(0x11); modrm_mem(xreg, base, disp);
}
static inline void movsd_load(int xreg, int base, int32_t disp) {
    e8(0xF2); rex(0, xreg, 0, base); e8(0x0F); e8(0x10); modrm_mem(xreg, base, disp);
}
static inline void movsd_store(int xreg, int base, int32_t disp) {
    e8(0xF2); rex(0, xreg, 0, base); e8(0x0F); e8(0x11); modrm_mem(xreg, base, disp);
}
// SSE binary op, scalar single (F3 0F op /r). op = 0x58 add, 0x5C sub,
// 0x59 mul, 0x5E div. Operand form is xmm-xmm (mod==3).
static inline void sse_ss_op(int op, int xdst, int xsrc) {
    e8(0xF3); rex(0, xdst, 0, xsrc); e8(0x0F); e8(op); modrm_rr(xdst, xsrc);
}
// SSE binary op, scalar double (F2 0F op /r).
static inline void sse_sd_op(int op, int xdst, int xsrc) {
    e8(0xF2); rex(0, xdst, 0, xsrc); e8(0x0F); e8(op); modrm_rr(xdst, xsrc);
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

// mov r/m64, r64  — 64-bit reg→reg copy. Used by the slow-path stubs to
// shuffle volatile bases (r9/r10/r11) into nonvols before a C call.
static inline void mov_rr64(int dst, int src) {
    rex(1, src, 0, dst);
    e8(0x89);
    modrm_rr(src, dst);
}

// ── guest-memory access ────────────────────────────────────────────────────
// Loads land in EAX; stores read EAX/AX/AL. Each access first checks the
// per-1-MiB g_mmio_tab: a 0 means "plain RAM" and the access is inlined as
// `mov reg, [REG_MEM + index]`; a 1 calls one of the pre-emitted stubs at
// the start of the arena (see emit_slow_stub) — those stubs are real Win64
// functions with proper SEH unwind info, so the JIT block itself remains
// a true leaf function for the entire fast path.

// Pre-emitted slow-path stubs. Indexed by:
//   g_stub_load[width][signext]   — load returning u32 in eax
//   g_stub_store[width]           — store; eax=val, ecx=addr at entry
// Populated by emit_slow_stubs() called from jit_alloc().
// width index: 0=byte, 1=half, 2=word.
static uint8_t* g_stub_load [3][2] = {};
static uint8_t* g_stub_store[3]    = {};

// Emit one slow-path stub:
//   push rdi ; push rsi ; push rbx ; sub rsp, 0x20      ; prolog (7 bytes)
//   [store only] mov edx, eax                            ; 2 bytes (post-prolog)
//   mov rdi, r11 ; mov rsi, r10 ; mov rbx, r9            ; save volatile bases
//   mov rax, helper ; call rax                           ; C helper call
//   mov r11, rdi ; mov r10, rsi ; mov r9, rbx            ; restore
//   add rsp, 0x20 ; pop rbx ; pop rsi ; pop rdi ; ret    ; epilog
//
// Alignment: caller (JIT block) rsp is 0 mod 16 on entry to the block; after
// `call stub` the stub entry rsp is 8 mod 16. 3 pushes + sub 0x20 → 0 mod 16
// before `call helper`, which gives helper rsp+8 == 0 mod 16. ✓
//
// Unwind info (shared across all 8 stubs): SizeOfProlog=7, 4 codes:
//   alloc_small 0x20 @ off 7 ; push_nonvol rbx @ off 3 ;
//   push_nonvol rsi @ off 2  ; push_nonvol rdi @ off 1
static uint8_t* emit_slow_stub(void* helper, bool is_store) {
    uint8_t* entry = g_emit;
    // Prolog (7 bytes total) ------------------------------------------------
    e8(0x57);                              // push rdi
    e8(0x56);                              // push rsi
    e8(0x53);                              // push rbx
    e8(0x48); e8(0x83); e8(0xEC); e8(0x20);// sub rsp, 0x20
    // Body ------------------------------------------------------------------
    if (is_store) {                        // mov edx, eax  (value → arg2)
        rex(0, X_AX, 0, X_DX); e8(0x89); modrm_rr(X_AX, X_DX);
    }
    mov_rr64(X_DI, X_R11);                 // save REG_CPU
    mov_rr64(X_SI, X_R10);                 // save REG_MEM
    mov_rr64(X_BX, X_R9);                  // save REG_MMIOTAB
    mov_ri64(X_AX, (uint64_t)(uintptr_t)helper);
    e8(0xFF); e8(0xD0);                    // call rax
    mov_rr64(X_R11, X_DI);                 // restore REG_CPU
    mov_rr64(X_R10, X_SI);                 // restore REG_MEM
    mov_rr64(X_R9,  X_BX);                 // restore REG_MMIOTAB
    // Epilog ---------------------------------------------------------------
    e8(0x48); e8(0x83); e8(0xC4); e8(0x20);// add rsp, 0x20
    e8(0x5B);                              // pop rbx
    e8(0x5E);                              // pop rsi
    e8(0x5F);                              // pop rdi
    e8(0xC3);                              // ret
    return entry;
}

// Tail-load suffix emitted at the call site of a load stub: convert the
// raw byte/halfword in eax into the requested sign-extended width.
// (The byte/half helpers themselves return sign- or zero-extended u32, so
// no extra work is needed — but we keep this trivial helper for clarity.)
static inline void load_stub_tail(int /*width*/, bool /*signext*/) {
    // Nothing: jit_helper_lb / lbu / lh / lhu / lw already return the right
    // 32-bit value in EAX.
}

// Probe g_mmio_tab[addr>>20] and emit a JNE that the caller patches to the
// helper (slow) path. Returns the rel32 patch site.
static inline uint8_t* emit_mmio_probe(int addr_reg) {
    rex(0, addr_reg, 0, X_DX); e8(0x89); modrm_rr(addr_reg, X_DX);
    shift_ri(5 /*SHR*/, X_DX, 20);
    rex(0, 0, X_DX, REG_MMIOTAB);
    e8(0x80);
    e8((0 << 6) | (7 << 3) | 4);
    e8((0 << 6) | ((X_DX & 7) << 3) | (REG_MMIOTAB & 7));
    e8(0x00);
    return emit_jcc(CC_NE);
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

    // ── slow helper path (inline push/pop, leaf-friendly unwind via the
    //    arena's UWOP_ALLOC_SMALL prolog declared at jit_alloc time) ─────
    patch_rel32(jne_site, g_emit);
    e8(0x41); e8(0x51);                                   // push r9
    e8(0x41); e8(0x52);                                   // push r10
    e8(0x41); e8(0x53);                                   // push r11
    e8(0x48); e8(0x83); e8(0xEC); e8(0x28);               // sub rsp, 0x28

    void* helper;
    if (width == 4)                 helper = (void*)&jit_helper_lw;
    else if (width == 2 && signext) helper = (void*)&jit_helper_lh;
    else if (width == 2)            helper = (void*)&jit_helper_lhu;
    else if (width == 1 && signext) helper = (void*)&jit_helper_lb;
    else                            helper = (void*)&jit_helper_lbu;
    mov_ri64(X_AX, (uint64_t)(uintptr_t)helper);
    e8(0xFF); e8(0xD0);

    e8(0x48); e8(0x83); e8(0xC4); e8(0x28);               // add rsp, 0x28
    e8(0x41); e8(0x5B);                                   // pop r11
    e8(0x41); e8(0x5A);                                   // pop r10
    e8(0x41); e8(0x59);                                   // pop r9

    patch_rel32(jmp_end, g_emit);
}

static inline void guest_store(int width, int index) {
    uint8_t* jne_site = emit_mmio_probe(index);

    // ── fast inline path ──────────────────────────────────────────
    if (width == 2) e8(0x66);
    rex(0, X_AX, index, REG_MEM);
    e8(width == 1 ? 0x88 : 0x89);
    modrm_sib(X_AX, REG_MEM, index);
    uint8_t* jmp_end = emit_jmp();

    // ── slow helper path ──────────────────────────────────────────
    patch_rel32(jne_site, g_emit);
    rex(0, X_AX, 0, X_DX); e8(0x89); modrm_rr(X_AX, X_DX);  // mov edx, eax

    e8(0x41); e8(0x51);                                   // push r9
    e8(0x41); e8(0x52);                                   // push r10
    e8(0x41); e8(0x53);                                   // push r11
    e8(0x51);                                              // push rcx
    e8(0x48); e8(0x83); e8(0xEC); e8(0x20);               // sub rsp, 0x20

    void* helper = (width == 1) ? (void*)&jit_helper_sb
                : (width == 2) ? (void*)&jit_helper_sh
                :                (void*)&jit_helper_sw;
    mov_ri64(X_AX, (uint64_t)(uintptr_t)helper);
    e8(0xFF); e8(0xD0);

    e8(0x48); e8(0x83); e8(0xC4); e8(0x20);               // add rsp, 0x20
    e8(0x59);                                              // pop rcx
    e8(0x41); e8(0x5B);                                   // pop r11
    e8(0x41); e8(0x5A);                                   // pop r10
    e8(0x41); e8(0x59);                                   // pop r9

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

    // ── Publish Windows x64 SEH unwind info for the arena ──────────────
    // Without this, the .NET GC's stack walker (built on Windows
    // RtlVirtualUnwind) blows up when it tries to unwind a frame whose
    // RIP is inside the JIT arena. Symptom: kernel-oops printk under
    // Linux floods VEH; GC eventually fires from a peripheral callback;
    // walker hits the JIT frame; process fast-fails with
    // STATUS_STACK_BUFFER_OVERRUN (0xc0000409).
    //
    // Layout:
    //   * 8 slow-path stubs (each is a real Win64 function: 3 nonvol
    //     pushes + a 0x20 alloc + a C helper call + epilog). Each gets
    //     its OWN RUNTIME_FUNCTION pointing at a shared stub-unwind-info
    //     describing the prolog properly.
    //   * The rest of the arena is JIT block code, which contains no
    //     stack ops at all (the slow path is now a `call rel32` into one
    //     of the stubs — no per-call-site prolog). A second
    //     RUNTIME_FUNCTION covers the JIT-block range with leaf unwind
    //     info (no saved regs, no frame).
    //
    // Memory layout at the start of the arena:
    //   [0..7]    leaf UNWIND_INFO  (8 bytes, padded)
    //   [8..23]   stub UNWIND_INFO  (16 bytes; 4 codes + padding)
    //   [24..131] RUNTIME_FUNCTION table — 9 entries × 12 bytes = 108
    //   [132..143] padding to 16-byte alignment
    //   [144..]   stub bodies, then JIT block code

    // ── Single leaf RUNTIME_FUNCTION declaring the arena as one big leaf
    // function. The slow-path push/pop frames are NOT described by the
    // unwind codes — known to be a partial-truth, but GC stack walks
    // hitting the slow path during contention are statistically uncommon.
    // The dispatcher-side try/catch in MmioDispatcher and the lock-free
    // peripheral lookup do the bulk of the hardening. Layout: [0..3] is
    // UNWIND_INFO, [4..15] is RUNTIME_FUNCTION, code starts at offset 16.
    uint8_t* unwind_info = g_code;
    unwind_info[0] = 0x01;   // Version=1, Flags=0
    unwind_info[1] = 0x00;   // SizeOfProlog = 0
    unwind_info[2] = 0x00;   // CountOfUnwindCodes = 0
    unwind_info[3] = 0x00;   // FrameRegister=0
    RUNTIME_FUNCTION_X64* rf = (RUNTIME_FUNCTION_X64*)(g_code + 4);
    rf->BeginAddress       = 16;
    rf->EndAddress         = JIT_CODE_SIZE;
    rf->UnwindInfoAddress  = 0;
    RtlAddFunctionTable(rf, 1, (unsigned long long)g_code);

    g_emit               = g_code + 16;
    g_block_region_start = g_emit;
    g_code_end           = g_code + JIT_CODE_SIZE - 4096;

    // Default everything to "use the helper" for safety.
    for (unsigned i = 0; i < 4096; i++) g_mmio_tab[i] = 1;
    // Punch known plain-RAM regions to 0 so the JIT inlines them.
    //   [0x00000000 .. 0x04000000)  — bare-metal guest RAM (up to 64 MiB).
    // Quake uses 64 MiB with the stack at 0x03FFFFF0. DO NOT extend
    // past the actually-committed region: anything beyond is host
    // PAGE_NOACCESS and the inline mov would AV without a peripheral
    // fallback. Linux's RAM at 0x80000000 deliberately stays in the
    // slow-helper range — extending plain to it routed spurious
    // accesses past the committed 96 MB into uncommitted host VA and
    // hung the kernel at PC=0 (non-existent trap handler).
    for (unsigned i = 0x000; i < 0x040; i++) g_mmio_tab[i] = 0;
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
// NB: the stubs at the arena head (and the metadata before them) are NOT
// flushed; only the JIT-block region is reset. g_stub_load/g_stub_store
// remain valid across flushes.
static void jit_flush() {
    g_emit = g_block_region_start ? g_block_region_start : g_code;
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

// SSE softfloat shortcut: emit inline x86 SSE for a softfloat ABI op,
// then `ret` to the dispatcher (which transfers to ra). a0/a1 live in
// the guest register file at offsets 40 / 44; for doubles a2/a3 at
// 48 / 52, and the return uses the (a0, a1) pair so a single 8-byte
// store back into offset 40 covers both halves.
//
// `op_byte` is the SSE binop opcode (0x58 add, 0x5C sub, 0x59 mul,
// 0x5E div). `is_double` picks between F3 0F (single) and F2 0F
// (double) and adjusts the load/store widths.
static void emit_softfloat_shortcut(int op_byte, bool is_double) {
    constexpr int A0_OFF = 10 * 4;   // cpu.regs[10] = a0
    constexpr int A1_OFF = 11 * 4;   // cpu.regs[11] = a1 (also high half of double a)
    constexpr int A2_OFF = 12 * 4;   // cpu.regs[12] = a2 (low half of double b)
    if (is_double) {
        movsd_load(0 /*xmm0*/, REG_CPU, A0_OFF);  // xmm0 = a
        movsd_load(1 /*xmm1*/, REG_CPU, A2_OFF);  // xmm1 = b
        sse_sd_op(op_byte, 0, 1);                 // xmm0 OP= xmm1
        movsd_store(0, REG_CPU, A0_OFF);          // a0:a1 = result
    } else {
        movss_load(0, REG_CPU, A0_OFF);
        movss_load(1, REG_CPU, A1_OFF);
        sse_ss_op(op_byte, 0, 1);
        movss_store(0, REG_CPU, A0_OFF);
    }
    // Tail-return: cpu.pc = ra (cpu.regs[1]), then ret to dispatcher.
    ld_greg(X_AX, 1);                             // eax = ra
    st_mem32(REG_CPU, CPU_OFF_PC, X_AX);
    // Charge one instruction for the elided softfloat call (so per-step
    // accounting stays vaguely sane).
    emit_alu_mi32(5 /*SUB*/, REG_CPU, CPU_OFF_BUDGET, 1);
    emit_ret();
}

// Public: C# calls this after parsing the guest ELF symbol table.
// op = SF_ADDSF3 etc., pc = entry PC of that symbol. Passing pc = 0
// uninstalls the hook.
extern "C" void rv32i_set_softfloat_hook(int op, uint32_t pc) {
    if ((unsigned)op < SF_OP_COUNT) g_sf_hook_pc[op] = pc;
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

    // ── SSE softfloat shortcut ──────────────────────────────────────────
    // If pc0 matches a known softfloat entry (registered by C# at ELF
    // load time), replace the whole basic block with a few inline SSE
    // ops + jump-to-ra. The JIT cache stores this stub the same way as
    // any other block — eager chaining (JAL with target = __mulsf3 PC)
    // jumps straight into the stub.
    for (int i = 1; i < SF_OP_COUNT; i++) {
        /* Skip uninstalled hooks (default value 0). Otherwise PC=0 would
         * spuriously match — Linux hits PC=0 when a very-early trap
         * fires before the kernel installs its handler, and the JIT
         * would emit a softfloat stub there, busy-looping forever. */
        if (g_sf_hook_pc[i] == 0) continue;
        if (g_sf_hook_pc[i] != pc0) continue;
        int op_byte; bool is_double;
        switch (i) {
            case SF_ADDSF3: op_byte = 0x58; is_double = false; break;
            case SF_SUBSF3: op_byte = 0x5C; is_double = false; break;
            case SF_MULSF3: op_byte = 0x59; is_double = false; break;
            case SF_DIVSF3: op_byte = 0x5E; is_double = false; break;
            case SF_ADDDF3: op_byte = 0x58; is_double = true;  break;
            case SF_SUBDF3: op_byte = 0x5C; is_double = true;  break;
            case SF_MULDF3: op_byte = 0x59; is_double = true;  break;
            case SF_DIVDF3: op_byte = 0x5E; is_double = true;  break;
            default: continue;
        }
        emit_softfloat_shortcut(op_byte, is_double);
        (void)i;
        // Register the stub like a normal block so eager chaining and the
        // direct-mapped slot lookup both find it.
        unsigned slot = jit_slot(pc0);
        g_map[slot].pc          = pc0;
        g_map[slot].code        = code_start;
        g_map[slot].chain_entry = chain_entry;
        return &g_map[slot];
    }

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
