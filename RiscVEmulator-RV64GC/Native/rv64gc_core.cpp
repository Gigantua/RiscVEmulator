// rv64gc_core.cpp — Native RISC-V RV64GC hot path. Windows only (ClangCL).
//
// A 64-bit sibling of rv32i_core.cpp. The RV32 core is left untouched and
// still drives every bare-metal example and the Sv32 Linux builds; this
// core exists solely to boot a real rv64gc distribution (Alpine riscv64).
//
// ── ISA implemented ─────────────────────────────────────────────────
//
//   RV64I            Base integer ISA + the *W 32-bit-result variants
//                    (ADDIW/SLLIW/SRLIW/SRAIW, ADDW/SUBW/SLLW/SRLW/SRAW),
//                    LD/SD/LWU, 6-bit shift amounts.
//   M                MUL/MULH[SU|U]/DIV[U]/REM[U] + MULW/DIVW/DIVUW/REMW/REMUW.
//   A                LR/SC + AMO{SWAP,ADD,XOR,AND,OR,MIN[U],MAX[U]} .W and .D.
//   F + D            Single- and double-precision. Single values are
//                    NaN-boxed in the 64-bit f-registers. No exception
//                    flags, rounding mode ignored (RNE).
//   C                Compressed 16-bit encodings — decoded by expanding
//                    each to its 32-bit equivalent (decompress()).
//   Zicsr / Zifencei CSR access; FENCE / FENCE.I are NOPs.
//
// ── Privileged architecture (always on; boots in M-mode) ────────────
//
//   M/S/U modes, trap delegation, MRET/SRET/WFI/ECALL/EBREAK.
//   Sv39 MMU — three-level page-table walk on fetch/load/store/AMO when
//   satp.MODE = 8 and the effective privilege is below M. 256-entry
//   direct-mapped TLB; SFENCE.VMA / satp writes flush it; hardware A/D.
//   SBI — optional (rv64_set_sbi_mode): the emulator acts as the M-mode
//   firmware and services S-mode ECALLs in place. This is how Linux boots.
//
// ── Host integration ────────────────────────────────────────────────
//
// Identical model to the RV32 core: every guest access is one pointer
// dereference into a host-provided base buffer, *(volatile T*)(mem + pa).
// The host commits plain pages for RAM and guarded (PAGE_NOACCESS) pages
// for MMIO; a vectored exception handler turns AVs into peripheral I/O.
// RAM sits at 0x80000000, so the host reservation spans [0, 0x80000000 +
// ramSize). Translated physical addresses index straight into it.

#include <cstdint>
#include <emmintrin.h>          // _mm_sqrt_ss / _mm_sqrt_sd — no CRT sqrt libcall.

extern "C" int _fltused = 0;    // float-ABI marker; required by the linker.

using u8  = uint8_t;   using u16 = uint16_t;
using u32 = uint32_t;  using u64 = uint64_t;
using i8  = int8_t;    using i16 = int16_t;
using i32 = int32_t;   using i64 = int64_t;

static inline float  fsqrtf(float x)  { return _mm_cvtss_f32(_mm_sqrt_ss(_mm_set_ss(x))); }
static inline double fsqrtd(double x) { return _mm_cvtsd_f64(_mm_sqrt_sd(_mm_setzero_pd(), _mm_set_sd(x))); }

// Own mem{set,cpy} so aggregate value-init links without a CRT (-nodefaultlib).
extern "C" void* memset(void* dst, int c, unsigned long long n) {
    auto* d = (unsigned char*)dst;
    for (unsigned long long i = 0; i < n; i++) d[i] = (unsigned char)c;
    return dst;
}
extern "C" void* memcpy(void* dst, const void* src, unsigned long long n) {
    auto* d = (unsigned char*)dst; auto* s = (const unsigned char*)src;
    for (unsigned long long i = 0; i < n; i++) d[i] = s[i];
    return dst;
}

// ── State ────────────────────────────────────────────────────────────

struct CPU_State {
    u64 regs[32];
    u64 fregs[32];
    u64 pc;
    int halted;
    u64 mtime;
    u64 mtimecmp;
    u64 rsv_addr;               // A-ext LR/SC reservation

    u8* mem;                    // host base; guest access hits *(mem + pa)

    u64 priv_mode;
    int wfi_pending;
    u64 csr_mstatus, csr_mtvec, csr_mie, csr_mip;
    u64 csr_mepc, csr_mtval, csr_mcause, csr_mscratch;
    u64 csr_medeleg, csr_mideleg;
    u64 csr_stvec, csr_sscratch, csr_sepc, csr_scause, csr_stval, csr_satp;
};

static CPU_State cpu;

// ── Memory access ───────────────────────────────────────────────────

template<typename T>
static __forceinline T mem_read(CPU_State& cpu, u64 addr) {
    return *(volatile T*)(cpu.mem + addr);
}
template<typename T>
static __forceinline void mem_write(CPU_State& cpu, u64 addr, T val) {
    *(volatile T*)(cpu.mem + addr) = val;
}

// ── Sv39 MMU ─────────────────────────────────────────────────────────
//
// Three-level walk for fetch/load/store/AMO. Active only when satp.MODE
// (bits 63:60) selects Sv39 AND the effective privilege is below Machine.
// A 256-entry direct-mapped TLB caches the leaf PTE and resolved page;
// SFENCE.VMA / satp writes bump g_tlb_gen, invalidating all entries. The
// cache stores only the walk result — permissions and A/D are re-checked
// on every hit, so a stale privilege level can never leak through.

static u64 g_tlb_gen  = 1;              // bumped by SFENCE.VMA / satp write
static int g_sbi_mode = 0;              // 1 → emulator services SBI ecalls

static u32 g_trap_count   = 0;
static u32 g_sbi_count    = 0;
static u32 g_sbi_timer_ct = 0;

struct TLBEntry {
    u64 tag;            // vaddr >> 12
    u64 gen;            // matches g_tlb_gen while valid
    u64 page_base;      // host-physical base of the resolved 4 KiB page
    u64 pte;            // cached leaf PTE (perm + A/D re-check)
    u64 pte_addr;       // physical address of that PTE (A/D write-back)
};
static TLBEntry g_tlb[256];

static constexpr u64 PPN_MASK = (1ULL << 44) - 1;   // PTE / satp PPN field

// Effective privilege for an access. Data accesses honour mstatus.MPRV.
static __forceinline u64 mmu_eff_mode(CPU_State& cpu, int access) {
    if (access != 0 && ((cpu.csr_mstatus >> 17) & 1u))      // MPRV
        return (cpu.csr_mstatus >> 11) & 3u;                // MPP
    return cpu.priv_mode;
}

// Permission check against a leaf PTE. access: 0=fetch 1=load 2=store/AMO.
// PTE bits: V=1 R=2 W=4 X=8 U=16.
static __forceinline int mmu_denied(CPU_State& cpu, u64 pte, int access, u64 mode) {
    u64 sum = (cpu.csr_mstatus >> 18) & 1u;
    u64 mxr = (cpu.csr_mstatus >> 19) & 1u;
    if (mode == 0 && !(pte & 16u)) return 1;
    if (mode == 1 &&  (pte & 16u) && (access == 0 || !sum)) return 1;
    if (access == 0) return (pte & 8u) == 0;
    if (access == 1) return !((pte & 2u) || (mxr && (pte & 8u)));
    return (pte & 4u) == 0;
}

// Translate a guest virtual address. On a fault sets `fault` to the
// page-fault cause (12 fetch / 13 load / 15 store) and returns 0.
static u64 mmu_translate(CPU_State& cpu, u64 vaddr, int access, u64& fault) {
    fault = 0;
    u64 mode = mmu_eff_mode(cpu, access);
    if (mode == 3 || (cpu.csr_satp >> 60) == 0) return vaddr;   // bare — no walk

    static const u64 FAULT[3] = { 12u, 13u, 15u };
    u64 vpn[3] = { (vaddr >> 12) & 0x1FF, (vaddr >> 21) & 0x1FF, (vaddr >> 30) & 0x1FF };
    u64 tag = vaddr >> 12;
    TLBEntry& e = g_tlb[tag & 255u];
    u64 pte, pte_addr, page_base;

    if (e.tag == tag && e.gen == g_tlb_gen) {
        pte = e.pte; pte_addr = e.pte_addr; page_base = e.page_base;
    } else {
        u64 a = (cpu.csr_satp & PPN_MASK) << 12;
        int level = 2;
        for (;; level--) {
            pte_addr = a + vpn[level] * 8u;
            pte      = mem_read<u64>(cpu, pte_addr);
            if (!(pte & 1u) || ((pte & 4u) && !(pte & 2u))) { fault = FAULT[access]; return 0; }
            if (pte & 0xAu) break;                          // R|X set → leaf
            if (level == 0) { fault = FAULT[access]; return 0; }
            a = ((pte >> 10) & PPN_MASK) << 12;
        }
        u64 ppn = (pte >> 10) & PPN_MASK;
        // Misaligned superpage: lower PPN slices must be zero.
        if (level == 1 && (ppn & 0x1FFu))    { fault = FAULT[access]; return 0; }
        if (level == 2 && (ppn & 0x3FFFFu))  { fault = FAULT[access]; return 0; }
        u64 pa;
        if      (level == 0) pa = (ppn << 12) | (vaddr & 0xFFFu);
        else if (level == 1) pa = ((ppn & ~0x1FFull) << 12) | (vaddr & 0x1FFFFFu);
        else                 pa = ((ppn & ~0x3FFFFull) << 12) | (vaddr & 0x3FFFFFFFu);
        page_base = pa & ~0xFFFull;
        e.tag = tag; e.gen = g_tlb_gen;
        e.pte = pte; e.pte_addr = pte_addr; e.page_base = page_base;
    }

    if (mmu_denied(cpu, pte, access, mode)) { fault = FAULT[access]; return 0; }

    u64 need = 0x40u | (access == 2 ? 0x80u : 0u);          // A always, D on store
    if ((pte & need) != need) {
        pte |= need;
        mem_write<u64>(cpu, pte_addr, pte);
        e.pte = pte;
    }
    return page_base | (vaddr & 0xFFFu);
}

// ── Immediate decoders (32-bit instruction form) ────────────────────

static constexpr i32 i_imm(u32 i) { return (i32)i >> 20; }
static constexpr i32 s_imm(u32 i) {
    return ((i32)(i & 0xFE000000) >> 20) | (i32)((i >> 7) & 0x1Fu);
}
static constexpr i32 b_imm(u32 i) {
    u32 v = ((i>>31)&1u)<<12 | ((i>>7)&1u)<<11 | ((i>>25)&0x3Fu)<<5 | ((i>>8)&0xFu)<<1;
    return (i32)((v & 0x1000u) ? v | 0xFFFFE000u : v);
}
static constexpr i32 j_imm(u32 i) {
    u32 v = ((i>>31)&1u)<<20 | ((i>>12)&0xFFu)<<12 | ((i>>20)&1u)<<11 | ((i>>21)&0x3FFu)<<1;
    return (i32)((v & 0x100000u) ? v | 0xFFE00000u : v);
}

// ── C-extension: decompress a 16-bit instruction to its 32-bit form ──
//
// Every compressed encoding has an exact 32-bit equivalent. We expand it
// and feed the result through the normal decoder, so the hot path has a
// single instruction-decode switch. Returns 0 for an illegal encoding.

static constexpr u32 enc_i(i32 imm, u32 rs1, u32 f3, u32 rd, u32 op) {
    return ((u32)imm & 0xFFFu) << 20 | rs1 << 15 | f3 << 12 | rd << 7 | op;
}
static constexpr u32 enc_s(i32 imm, u32 rs2, u32 rs1, u32 f3, u32 op) {
    return ((u32)imm & 0xFE0u) << 20 | rs2 << 20 | rs1 << 15 | f3 << 12
         | ((u32)imm & 0x1Fu) << 7 | op;
}
static constexpr u32 enc_r(u32 f7, u32 rs2, u32 rs1, u32 f3, u32 rd, u32 op) {
    return f7 << 25 | rs2 << 20 | rs1 << 15 | f3 << 12 | rd << 7 | op;
}
static constexpr u32 enc_u(i32 imm, u32 rd, u32 op) {
    return ((u32)imm & 0xFFFFF000u) | rd << 7 | op;
}
static constexpr u32 enc_b(i32 imm, u32 f3, u32 rs1, u32 rs2, u32 op) {
    u32 u = (u32)imm;
    return ((u>>12)&1u)<<31 | ((u>>5)&0x3Fu)<<25 | rs2<<20 | rs1<<15
         | f3<<12 | ((u>>1)&0xFu)<<8 | ((u>>11)&1u)<<7 | op;
}
static constexpr u32 enc_j(i32 imm, u32 rd, u32 op) {
    u32 u = (u32)imm;
    return ((u>>20)&1u)<<31 | ((u>>1)&0x3FFu)<<21 | ((u>>11)&1u)<<20
         | ((u>>12)&0xFFu)<<12 | rd<<7 | op;
}
static constexpr i32 sext(u32 v, int bits) {
    u32 m = 1u << (bits - 1);
    return (i32)((v ^ m) - m);
}

static u32 decompress(u16 c) {
    u32 op  = c & 3u;
    u32 f3  = (c >> 13) & 7u;
    u32 rd  = (c >> 7) & 0x1Fu;          // also rs1 for many forms
    u32 rs2 = (c >> 2) & 0x1Fu;
    u32 rp1 = 8u + ((c >> 7) & 7u);      // popular-register-set rs1'/rd'
    u32 rp2 = 8u + ((c >> 2) & 7u);      // popular-register-set rs2'/rd'

    if (op == 0) {                                          // ── quadrant 0
        switch (f3) {
            case 0: {                                       // C.ADDI4SPN
                u32 imm = ((c>>11)&3u)<<4 | ((c>>7)&0xFu)<<6
                        | ((c>>6)&1u)<<2 | ((c>>5)&1u)<<3;
                if (imm == 0) return 0;
                return enc_i((i32)imm, 2, 0, rp2, 0x13);
            }
            case 1: {                                       // C.FLD
                u32 imm = ((c>>10)&7u)<<3 | ((c>>5)&3u)<<6;
                return enc_i((i32)imm, rp1, 3, rp2, 0x07);
            }
            case 2: {                                       // C.LW
                u32 imm = ((c>>10)&7u)<<3 | ((c>>6)&1u)<<2 | ((c>>5)&1u)<<6;
                return enc_i((i32)imm, rp1, 2, rp2, 0x03);
            }
            case 3: {                                       // C.LD
                u32 imm = ((c>>10)&7u)<<3 | ((c>>5)&3u)<<6;
                return enc_i((i32)imm, rp1, 3, rp2, 0x03);
            }
            case 5: {                                       // C.FSD
                u32 imm = ((c>>10)&7u)<<3 | ((c>>5)&3u)<<6;
                return enc_s((i32)imm, rp2, rp1, 3, 0x27);
            }
            case 6: {                                       // C.SW
                u32 imm = ((c>>10)&7u)<<3 | ((c>>6)&1u)<<2 | ((c>>5)&1u)<<6;
                return enc_s((i32)imm, rp2, rp1, 2, 0x23);
            }
            case 7: {                                       // C.SD
                u32 imm = ((c>>10)&7u)<<3 | ((c>>5)&3u)<<6;
                return enc_s((i32)imm, rp2, rp1, 3, 0x23);
            }
            default: return 0;
        }
    }

    if (op == 1) {                                          // ── quadrant 1
        i32 imm6 = sext(((c>>2)&0x1Fu) | ((c>>12)&1u)<<5, 6);
        switch (f3) {
            case 0:                                         // C.ADDI / C.NOP
                return enc_i(imm6, rd, 0, rd, 0x13);
            case 1:                                         // C.ADDIW
                if (rd == 0) return 0;
                return enc_i(imm6, rd, 0, rd, 0x1B);
            case 2:                                         // C.LI
                return enc_i(imm6, 0, 0, rd, 0x13);
            case 3:
                if (rd == 2) {                              // C.ADDI16SP
                    i32 imm = sext(((c>>12)&1u)<<9 | ((c>>3)&3u)<<7
                                 | ((c>>5)&1u)<<6 | ((c>>6)&1u)<<4
                                 | ((c>>2)&1u)<<5, 10);
                    return enc_i(imm, 2, 0, 2, 0x13);
                } else {                                    // C.LUI
                    i32 imm = sext(((c>>12)&1u)<<17 | ((c>>2)&0x1Fu)<<12, 18);
                    if (imm == 0) return 0;
                    return enc_u(imm, rd, 0x37);
                }
            case 4: {                                       // C.MISC-ALU
                u32 sub  = (c >> 10) & 3u;
                u32 sh   = ((c>>2)&0x1Fu) | ((c>>12)&1u)<<5;
                if (sub == 0) return enc_i((i32)sh, rp1, 5, rp1, 0x13);          // C.SRLI
                if (sub == 1) return enc_i((i32)(0x400u | sh), rp1, 5, rp1, 0x13);// C.SRAI
                if (sub == 2) return enc_i(imm6, rp1, 7, rp1, 0x13);             // C.ANDI
                u32 c12 = (c >> 12) & 1u, o2 = (c >> 5) & 3u;
                if (c12 == 0) {
                    switch (o2) {
                        case 0: return enc_r(0x20, rp2, rp1, 0, rp1, 0x33);      // C.SUB
                        case 1: return enc_r(0x00, rp2, rp1, 4, rp1, 0x33);      // C.XOR
                        case 2: return enc_r(0x00, rp2, rp1, 6, rp1, 0x33);      // C.OR
                        default:return enc_r(0x00, rp2, rp1, 7, rp1, 0x33);      // C.AND
                    }
                } else {
                    if (o2 == 0) return enc_r(0x20, rp2, rp1, 0, rp1, 0x3B);     // C.SUBW
                    if (o2 == 1) return enc_r(0x00, rp2, rp1, 0, rp1, 0x3B);     // C.ADDW
                    return 0;
                }
            }
            case 5: {                                       // C.J
                i32 imm = sext(((c>>12)&1u)<<11 | ((c>>11)&1u)<<4 | ((c>>9)&3u)<<8
                             | ((c>>8)&1u)<<10 | ((c>>7)&1u)<<6 | ((c>>6)&1u)<<7
                             | ((c>>3)&7u)<<1  | ((c>>2)&1u)<<5, 12);
                return enc_j(imm, 0, 0x6F);
            }
            case 6:                                         // C.BEQZ
            case 7: {                                       // C.BNEZ
                i32 imm = sext(((c>>12)&1u)<<8 | ((c>>10)&3u)<<3 | ((c>>5)&3u)<<6
                             | ((c>>3)&3u)<<1 | ((c>>2)&1u)<<5, 9);
                return enc_b(imm, f3 == 6 ? 0 : 1, rp1, 0, 0x63);
            }
            default: return 0;
        }
    }

    if (op == 2) {                                          // ── quadrant 2
        u32 sh = ((c>>2)&0x1Fu) | ((c>>12)&1u)<<5;
        switch (f3) {
            case 0:                                         // C.SLLI
                return enc_i((i32)sh, rd, 1, rd, 0x13);
            case 1: {                                       // C.FLDSP
                u32 imm = ((c>>12)&1u)<<5 | ((c>>5)&3u)<<3 | ((c>>2)&7u)<<6;
                return enc_i((i32)imm, 2, 3, rd, 0x07);
            }
            case 2: {                                       // C.LWSP
                if (rd == 0) return 0;
                u32 imm = ((c>>12)&1u)<<5 | ((c>>4)&7u)<<2 | ((c>>2)&3u)<<6;
                return enc_i((i32)imm, 2, 2, rd, 0x03);
            }
            case 3: {                                       // C.LDSP
                if (rd == 0) return 0;
                u32 imm = ((c>>12)&1u)<<5 | ((c>>5)&3u)<<3 | ((c>>2)&7u)<<6;
                return enc_i((i32)imm, 2, 3, rd, 0x03);
            }
            case 4: {
                u32 c12 = (c >> 12) & 1u;
                if (c12 == 0) {
                    if (rs2 == 0) {                          // C.JR
                        if (rd == 0) return 0;
                        return enc_i(0, rd, 0, 0, 0x67);
                    }
                    return enc_r(0, rs2, 0, 0, rd, 0x33);    // C.MV
                } else {
                    if (rs2 == 0 && rd == 0) return 0x00100073u;  // C.EBREAK
                    if (rs2 == 0) return enc_i(0, rd, 0, 1, 0x67);// C.JALR
                    return enc_r(0, rs2, rd, 0, rd, 0x33);   // C.ADD
                }
            }
            case 5: {                                       // C.FSDSP
                u32 imm = ((c>>10)&7u)<<3 | ((c>>7)&7u)<<6;
                return enc_s((i32)imm, rs2, 2, 3, 0x27);
            }
            case 6: {                                       // C.SWSP
                u32 imm = ((c>>9)&0xFu)<<2 | ((c>>7)&3u)<<6;
                return enc_s((i32)imm, rs2, 2, 2, 0x23);
            }
            case 7: {                                       // C.SDSP
                u32 imm = ((c>>10)&7u)<<3 | ((c>>7)&7u)<<6;
                return enc_s((i32)imm, rs2, 2, 3, 0x23);
            }
            default: return 0;
        }
    }
    return 0;                                               // op==3 unreachable
}

// ── M-extension ─────────────────────────────────────────────────────

static inline u64 exec_m(u32 f3, u64 a, u64 b) {
    i64 sa = (i64)a, sb = (i64)b;
    switch (f3) {
        case 0: return a * b;
        case 1: return (u64)(((__int128)sa * (__int128)sb) >> 64);
        case 2: return (u64)(((__int128)sa * (unsigned __int128)b) >> 64);
        case 3: return (u64)(((unsigned __int128)a * (unsigned __int128)b) >> 64);
        case 4: return sb == 0 ? ~0ull : (sa == INT64_MIN && sb == -1 ? (u64)sa : (u64)(sa / sb));
        case 5: return b  == 0 ? ~0ull : a / b;
        case 6: return sb == 0 ? a : (sa == INT64_MIN && sb == -1 ? 0u : (u64)(sa % sb));
        case 7: return b  == 0 ? a : a % b;
        default: return 0;
    }
}

// 32-bit-result M ops (MULW/DIVW/DIVUW/REMW/REMUW), sign-extended to 64.
static inline u64 exec_mw(u32 f3, u32 a, u32 b) {
    i32 sa = (i32)a, sb = (i32)b;
    i32 r;
    switch (f3) {
        case 0: r = (i32)(a * b);                                          break;
        case 4: r = sb == 0 ? -1 : (sa == INT32_MIN && sb == -1 ? sa : sa / sb); break;
        case 5: r = b  == 0 ? -1 : (i32)(a / b);                           break;
        case 6: r = sb == 0 ? sa : (sa == INT32_MIN && sb == -1 ? 0 : sa % sb);  break;
        case 7: r = b  == 0 ? (i32)a : (i32)(a % b);                       break;
        default: r = 0;                                                    break;
    }
    return (u64)(i64)r;
}

// ── F / D extension ─────────────────────────────────────────────────
//
// Single values live NaN-boxed in the 64-bit f-registers. No exception
// flags; rounding mode is ignored (host RNE). Enough fidelity for a glibc
// rv64gc userland — the same pragmatism the RV32 core takes with F.

static inline float  fs_get(CPU_State& c, int r) { float v;  u32 b=(u32)c.fregs[r]; __builtin_memcpy(&v,&b,4); return v; }
static inline void   fs_set(CPU_State& c, int r, float v)  { u32 b; __builtin_memcpy(&b,&v,4); c.fregs[r]=0xFFFFFFFF00000000ull|b; }
static inline double fd_get(CPU_State& c, int r) { double v; __builtin_memcpy(&v,&c.fregs[r],8); return v; }
static inline void   fd_set(CPU_State& c, int r, double v) { __builtin_memcpy(&c.fregs[r],&v,8); }

static inline int fs_nan(float x)  { return x != x; }
static inline int fd_nan(double x) { return x != x; }

static u32 fclass_s(u32 b) {
    u32 sgn=b>>31, exp=(b>>23)&0xFF, m=b&0x7FFFFF;
    if (exp==0xFF) return m ? ((m&0x400000)?512u:256u) : (sgn?1u:128u);
    if (exp==0)    return m ? (sgn?4u:32u)             : (sgn?8u:16u);
    return sgn?2u:64u;
}
static u32 fclass_d(u64 b) {
    u64 sgn=b>>63, exp=(b>>52)&0x7FF, m=b&0xFFFFFFFFFFFFFull;
    if (exp==0x7FF) return m ? ((m&0x8000000000000ull)?512u:256u) : (sgn?1u:128u);
    if (exp==0)     return m ? (sgn?4u:32u)                       : (sgn?8u:16u);
    return sgn?2u:64u;
}

static void exec_fp(CPU_State& cpu, int rd, int rs1, int rs2, u32 f3, u32 f7) {
    u32 fmt  = f7 & 3u;             // 0 = single, 1 = double
    u32 kind = f7 & 0x7Cu;
    int dbl  = (fmt == 1);

    switch (kind) {
        case 0x00: case 0x04: case 0x08: case 0x0C: {        // FADD/FSUB/FMUL/FDIV
            if (dbl) {
                double a=fd_get(cpu,rs1), b=fd_get(cpu,rs2), r=0;
                r = kind==0x00?a+b : kind==0x04?a-b : kind==0x08?a*b : a/b;
                fd_set(cpu,rd,r);
            } else {
                float a=fs_get(cpu,rs1), b=fs_get(cpu,rs2), r=0;
                r = kind==0x00?a+b : kind==0x04?a-b : kind==0x08?a*b : a/b;
                fs_set(cpu,rd,r);
            }
            return;
        }
        case 0x2C:                                           // FSQRT
            if (dbl) fd_set(cpu,rd,fsqrtd(fd_get(cpu,rs1)));
            else     fs_set(cpu,rd,fsqrtf(fs_get(cpu,rs1)));
            return;
        case 0x10: {                                         // FSGNJ[N|X]
            if (dbl) {
                u64 a=cpu.fregs[rs1], b=cpu.fregs[rs2];
                u64 s = f3==0?b : f3==1?~b : a^b;
                cpu.fregs[rd] = (s & (1ull<<63)) | (a & ~(1ull<<63));
            } else {
                u32 a=(u32)cpu.fregs[rs1], b=(u32)cpu.fregs[rs2];
                u32 s = f3==0?b : f3==1?~b : a^b;
                u32 v = (s & 0x80000000u) | (a & 0x7FFFFFFFu);
                cpu.fregs[rd] = 0xFFFFFFFF00000000ull | v;
            }
            return;
        }
        case 0x14: {                                         // FMIN / FMAX
            if (dbl) {
                double a=fd_get(cpu,rs1), b=fd_get(cpu,rs2);
                int na=fd_nan(a), nb=fd_nan(b);
                if (na&&nb) fd_set(cpu,rd,(double)(0.0/0.0));
                else if (na) fd_set(cpu,rd,b);
                else if (nb) fd_set(cpu,rd,a);
                else fd_set(cpu,rd,f3==0?(a<b?a:b):(a>b?a:b));
            } else {
                float a=fs_get(cpu,rs1), b=fs_get(cpu,rs2);
                int na=fs_nan(a), nb=fs_nan(b);
                if (na&&nb) fs_set(cpu,rd,(float)(0.0f/0.0f));
                else if (na) fs_set(cpu,rd,b);
                else if (nb) fs_set(cpu,rd,a);
                else fs_set(cpu,rd,f3==0?(a<b?a:b):(a>b?a:b));
            }
            return;
        }
        case 0x20:                                           // FCVT.S.D / FCVT.D.S
            if (dbl) fd_set(cpu,rd,(double)fs_get(cpu,rs1));  // f7=0x21 → D = (double)S
            else     fs_set(cpu,rd,(float) fd_get(cpu,rs1));  // f7=0x20 → S = (float)D
            return;
        case 0x50: {                                         // FEQ/FLT/FLE → int reg
            u64 r = 0;
            if (dbl) {
                double a=fd_get(cpu,rs1), b=fd_get(cpu,rs2);
                if (!fd_nan(a) && !fd_nan(b))
                    r = f3==2 ? (a==b) : f3==1 ? (a<b) : (a<=b);
            } else {
                float a=fs_get(cpu,rs1), b=fs_get(cpu,rs2);
                if (!fs_nan(a) && !fs_nan(b))
                    r = f3==2 ? (a==b) : f3==1 ? (a<b) : (a<=b);
            }
            cpu.regs[rd] = r;
            return;
        }
        case 0x60: {                                         // FCVT.{W,WU,L,LU}.{S,D}
            double v = dbl ? fd_get(cpu,rs1) : (double)fs_get(cpu,rs1);
            int nan  = (v != v);
            u64 r;
            switch (rs2) {
                case 0: r = nan ? 0x7FFFFFFFu : (u64)(i64)(i32)v;            break; // W
                case 1: r = (nan||v<0) ? 0u : (u64)(i64)(i32)(u32)v;         break; // WU
                case 2: r = nan ? (u64)INT64_MAX : (u64)(i64)v;              break; // L
                default:r = (nan||v<0) ? 0ull : (u64)v;                     break; // LU
            }
            cpu.regs[rd] = r;
            return;
        }
        case 0x68: {                                         // FCVT.{S,D}.{W,WU,L,LU}
            u64 x = cpu.regs[rs1];
            double v;
            switch (rs2) {
                case 0: v = (double)(i32)(u32)x; break;
                case 1: v = (double)(u32)x;      break;
                case 2: v = (double)(i64)x;      break;
                default:v = (double)x;           break;
            }
            if (dbl) fd_set(cpu,rd,v);
            else     fs_set(cpu,rd,(float)v);
            return;
        }
        // kind masks off the format bit, so f7 0x70/0x71 (and 0x78/0x79)
        // collapse here — pick S vs D from `dbl`. FMV.X.D must move the full
        // 64-bit pattern; treating it as FMV.X.W truncated doubles to 32 bits
        // (the -nan in every printf("%f") of a variadic double).
        case 0x70:                                           // FMV.X.{W,D} / FCLASS.{S,D}
            if (f3 == 0)
                cpu.regs[rd] = dbl ? cpu.fregs[rs1]
                                   : (u64)(i64)(i32)(u32)cpu.fregs[rs1];
            else
                cpu.regs[rd] = dbl ? fclass_d(cpu.fregs[rs1])
                                   : fclass_s((u32)cpu.fregs[rs1]);
            return;
        case 0x78:                                           // FMV.{W,D}.X
            cpu.fregs[rd] = dbl ? cpu.regs[rs1]
                                : (0xFFFFFFFF00000000ull | (u32)cpu.regs[rs1]);
            return;
        default:
            return;
    }
}

// ── CSR / trap ──────────────────────────────────────────────────────

static u64& priv_csr(CPU_State& cpu, u32 csrno) {
    static u64 scratch;
    switch (csrno) {
        case 0x300: case 0x100: return cpu.csr_mstatus;
        case 0x302:             return cpu.csr_medeleg;
        case 0x303:             return cpu.csr_mideleg;
        case 0x304: case 0x104: return cpu.csr_mie;
        case 0x305:             return cpu.csr_mtvec;
        case 0x340:             return cpu.csr_mscratch;
        case 0x341:             return cpu.csr_mepc;
        case 0x342:             return cpu.csr_mcause;
        case 0x343:             return cpu.csr_mtval;
        case 0x344: case 0x144: return cpu.csr_mip;
        case 0x105:             return cpu.csr_stvec;
        case 0x140:             return cpu.csr_sscratch;
        case 0x141:             return cpu.csr_sepc;
        case 0x142:             return cpu.csr_scause;
        case 0x143:             return cpu.csr_stval;
        case 0x180:             return cpu.csr_satp;
        case 0x301:             scratch = (2ull<<62) | 0x141125ull; return scratch; // misa (RO)
        case 0xF11:             scratch = 0;            return scratch;             // mvendorid
        case 0xC00: case 0xB00: case 0xC01: case 0xB01: case 0xC02: case 0xB02:
                                scratch = cpu.mtime;    return scratch;             // cycle/time/instret
        default:                scratch = 0;            return scratch;
    }
}

static void do_trap(CPU_State& cpu, u64 cause, u64 tval) {
    g_trap_count++;
    bool is_intr = (cause >> 63) & 1u;
    u64  cidx    = cause & 0x3Fu;
    u64  bit     = 1ull << cidx;
    bool to_s    = g_sbi_mode ||
                   ((cpu.priv_mode < 3) &&
                    (is_intr ? (cpu.csr_mideleg & bit) : (cpu.csr_medeleg & bit)));

    cpu.wfi_pending = 0;

    if (to_s) {
        cpu.csr_sepc   = cpu.pc;
        cpu.csr_scause = cause;
        cpu.csr_stval  = tval;
        u64 sie = (cpu.csr_mstatus >> 1) & 1u;
        u64 spp = cpu.priv_mode & 1u;
        cpu.csr_mstatus = (cpu.csr_mstatus & ~0x122ull) | (spp << 8) | (sie << 5);
        cpu.priv_mode   = 1;
        cpu.pc = (is_intr && (cpu.csr_stvec & 1u)) ? (cpu.csr_stvec & ~3ull) + cidx*4u
                                                   : (cpu.csr_stvec & ~3ull);
    } else {
        cpu.csr_mepc   = cpu.pc;
        cpu.csr_mcause = cause;
        cpu.csr_mtval  = tval;
        u64 mie_b = (cpu.csr_mstatus >> 3) & 1u;
        cpu.csr_mstatus = (cpu.csr_mstatus & ~0x1888ull) | (cpu.priv_mode << 11) | (mie_b << 7);
        cpu.priv_mode   = 3;
        cpu.pc = (is_intr && (cpu.csr_mtvec & 1u)) ? (cpu.csr_mtvec & ~3ull) + cidx*4u
                                                   : (cpu.csr_mtvec & ~3ull);
    }
}

static constexpr u64 INTR = 1ull << 63;

static bool check_interrupts(CPU_State& cpu) {
    if (g_sbi_mode) {
        if (cpu.mtime >= cpu.mtimecmp) cpu.csr_mip |= (1u << 5);
        u64 pending = cpu.csr_mip & cpu.csr_mie & 0x222u;
        if (!pending) return false;
        u64  sie  = (cpu.csr_mstatus >> 1) & 1u;
        bool fire = (cpu.priv_mode == 0) || (cpu.priv_mode == 1 && sie);
        if (!fire) return false;
        u64 cause = (pending & (1u<<9)) ? 9u : (pending & (1u<<5)) ? 5u : 1u;
        do_trap(cpu, INTR | cause, 0);
        return true;
    }

    if (cpu.mtime >= cpu.mtimecmp) cpu.csr_mip |=  (1u << 7);
    else                          cpu.csr_mip &= ~(1u << 7);

    u64 pending = cpu.csr_mip & cpu.csr_mie;
    if (!pending) return false;

    u64 mie_b = (cpu.csr_mstatus >> 3) & 1u;
    u64 sie_b = (cpu.csr_mstatus >> 1) & 1u;
    static const u32 prio[6] = { 11, 3, 7, 9, 1, 5 };
    for (int i = 0; i < 6; i++) {
        u64 b = 1ull << prio[i];
        if (!(pending & b)) continue;
        bool delegated = (cpu.csr_mideleg & b) != 0u;
        bool fire = delegated
            ? (cpu.priv_mode == 0 || (cpu.priv_mode == 1 && sie_b))
            : (cpu.priv_mode <  3 || mie_b);
        if (fire) { do_trap(cpu, INTR | prio[i], 0); return true; }
    }
    return false;
}

// ── SBI ─────────────────────────────────────────────────────────────

static constexpr u64 SBI_UART_THR = 0x10000000u;
static constexpr u64 SBI_ENOTSUPP = (u64)-2;

static void sbi_call(CPU_State& cpu) {
    g_sbi_count++;
    const u64 eid = cpu.regs[17], fid = cpu.regs[16];
    const u64 a0 = cpu.regs[10], a1 = cpu.regs[11];
    u64 err = 0, val = 0;

    switch (eid) {
    case 0x10:
        switch (fid) {
            case 0: val = 0x01000000u; break;
            case 1: val = 1u;          break;
            case 2: val = 1u;          break;
            case 3:
                val = (a0==0x10 || a0==0x54494D45u || a0==0x735049u ||
                       a0==0x52464E43u || a0==0x53525354u || a0==0x4442434Eu) ? 1u : 0u;
                break;
            case 4: case 5: case 6: val = 0u; break;
            default: err = SBI_ENOTSUPP; break;
        }
        break;
    case 0x54494D45:                                         // TIME — set_timer(a0)
        if (fid == 0) {
            cpu.mtimecmp = a0;                               // RV64: full 64-bit in a0
            cpu.csr_mip &= ~(1u << 5);
            g_sbi_timer_ct++;
        } else err = SBI_ENOTSUPP;
        break;
    case 0x735049:
        if (fid != 0) err = SBI_ENOTSUPP;
        break;
    case 0x52464E43:                                         // RFENCE — flush our TLB
        g_tlb_gen++;
        break;
    case 0x53525354:
        if (fid == 0) cpu.halted = 1;
        else          err = SBI_ENOTSUPP;
        break;
    case 0x4442434E:                                         // DBCN — debug console
        if (fid == 0) {
            for (u64 i = 0; i < a0; i++)
                mem_write<u8>(cpu, SBI_UART_THR, mem_read<u8>(cpu, a1 + i));
            val = a0;
        } else if (fid == 2) {
            mem_write<u8>(cpu, SBI_UART_THR, (u8)a0);
        } else if (fid != 1) {
            err = SBI_ENOTSUPP;
        }
        break;
    case 0x00:
        cpu.mtimecmp = a0;
        cpu.csr_mip &= ~(1u << 5);
        cpu.regs[10] = 0;
        return;
    case 0x01:
        mem_write<u8>(cpu, SBI_UART_THR, (u8)a0);
        cpu.regs[10] = 0;
        return;
    case 0x02:
        cpu.regs[10] = (u64)-1;
        return;
    case 0x03: case 0x04:
        cpu.regs[10] = 0;
        return;
    case 0x05: case 0x06: case 0x07:
        g_tlb_gen++;
        cpu.regs[10] = 0;
        return;
    case 0x08:
        cpu.halted = 1;
        cpu.regs[10] = 0;
        return;
    default:
        err = SBI_ENOTSUPP;
        break;
    }
    cpu.regs[10] = err;
    cpu.regs[11] = val;
}

static u64 exec_system(CPU_State& cpu, u32 instr, int rd, u32 f3s,
                       u64& nextpc, u64& trap_tval) {
    u32 fn = (instr >> 20) & 0xFFFu;
    if (f3s == 0) {
        if ((instr >> 25) == 0x09u) { g_tlb_gen++; return 0; }   // SFENCE.VMA
        if (fn == 0) {                                           // ECALL
            if (g_sbi_mode && cpu.priv_mode == 1) { sbi_call(cpu); return 0u; }
            return (cpu.priv_mode == 3) ? 11u : (cpu.priv_mode == 1) ? 9u : 8u;
        }
        if (fn == 1)     { trap_tval = cpu.pc; return 3u; }      // EBREAK
        if (fn == 0x102) {                                       // SRET
            u64 spie = (cpu.csr_mstatus >> 5) & 1u;
            u64 spp  = (cpu.csr_mstatus >> 8) & 1u;
            cpu.csr_mstatus = (cpu.csr_mstatus & ~0x122ull) | (1u << 5) | (spie << 1);
            cpu.priv_mode   = spp;
            nextpc          = cpu.csr_sepc;
        } else if (fn == 0x302) {                                // MRET
            u64 mpie = (cpu.csr_mstatus >> 7) & 1u;
            u64 mpp  = (cpu.csr_mstatus >> 11) & 3u;
            cpu.csr_mstatus = (cpu.csr_mstatus & ~0x1888ull) | (1u << 7) | (mpie << 3);
            cpu.priv_mode   = mpp;
            nextpc          = cpu.csr_mepc;
        } else if (fn == 0x105) {                                // WFI
            cpu.csr_mstatus |= 8u;
            cpu.wfi_pending  = 1;
        }
        return 0;
    }
    int  rs1imm = (instr >> 15) & 0x1F;
    u64  rs1v   = cpu.regs[rs1imm];
    u64& slot   = priv_csr(cpu, fn);
    u64  old    = slot;
    cpu.regs[rd] = old;
    u64 nval = old;
    switch (f3s) {
        case 1: nval = rs1v;                 break;
        case 2: nval = old |  rs1v;          break;
        case 3: nval = old & ~rs1v;          break;
        case 5: nval = (u64)rs1imm;          break;
        case 6: nval = old |  (u64)rs1imm;   break;
        case 7: nval = old & ~(u64)rs1imm;   break;
    }
    if (f3s == 1 || f3s == 5 || rs1imm != 0) {
        if (fn == 0x180) {
            // satp is WARL: a write that selects an unsupported MODE has no
            // effect at all (priv spec). This core implements Sv39 (MODE 8)
            // and Bare (0) only — discarding MODE 9/10 (Sv48/Sv57) writes is
            // exactly what makes the kernel's satp-mode probe settle on Sv39.
            u64 m = nval >> 60;
            if (m == 0 || m == 8) { slot = nval; g_tlb_gen++; }
        } else {
            slot = nval;
        }
    }
    return 0;
}

// ── do_step ─────────────────────────────────────────────────────────

static void do_step(CPU_State& cpu) {
    if (check_interrupts(cpu)) { cpu.regs[0] = 0; return; }
    if (cpu.wfi_pending) {
        if (cpu.csr_mip & cpu.csr_mie) cpu.wfi_pending = 0;
        else { cpu.regs[0] = 0; return; }
    }

    u64 ifault = 0;
    u64 pa0 = mmu_translate(cpu, cpu.pc, 0, ifault);
    if (ifault) { do_trap(cpu, ifault, cpu.pc); cpu.regs[0] = 0; return; }

    u32 lo = mem_read<u16>(cpu, pa0);
    u32 instr;
    u64 ilen;
    if ((lo & 3u) == 3u) {                                  // 32-bit instruction
        u64 pa2 = mmu_translate(cpu, cpu.pc + 2, 0, ifault);
        if (ifault) { do_trap(cpu, ifault, cpu.pc + 2); cpu.regs[0] = 0; return; }
        instr = lo | ((u32)mem_read<u16>(cpu, pa2) << 16);
        ilen  = 4;
    } else {                                                // 16-bit compressed
        instr = decompress((u16)lo);
        ilen  = 2;
        if (instr == 0) { do_trap(cpu, 2u, lo); cpu.regs[0] = 0; return; }
    }

    const u32 opcode = instr & 0x7F;
    const int rd     = (instr >>  7) & 0x1F;
    const int rs1    = (instr >> 15) & 0x1F;
    const int rs2    = (instr >> 20) & 0x1F;
    const u32 f3     = (instr >> 12) & 0x7;
    const u32 f7     = (instr >> 25) & 0x7F;
    const i64 s1     = (i64)cpu.regs[rs1];
    const u64 u1     = cpu.regs[rs1];
    const i64 s2     = (i64)cpu.regs[rs2];
    const u64 u2     = cpu.regs[rs2];
    u64 nextpc       = cpu.pc + ilen;
    u64 trap_cause   = 0;
    u64 trap_tval    = 0;

    switch (opcode) {

    case 0x37: cpu.regs[rd] = (u64)(i64)(i32)(instr & 0xFFFFF000u);            break;  // LUI
    case 0x17: cpu.regs[rd] = cpu.pc + (u64)(i64)(i32)(instr & 0xFFFFF000u);   break;  // AUIPC
    case 0x6F: cpu.regs[rd] = cpu.pc + ilen; nextpc = cpu.pc + (i64)j_imm(instr); break;// JAL
    case 0x67: { u64 t = (u64)(s1 + i_imm(instr)) & ~1ull;                            // JALR
                 cpu.regs[rd] = cpu.pc + ilen; nextpc = t;                    break; }

    case 0x63: {                                                              // branches
        int taken = 0;
        switch (f3) {
            case 0: taken = u1 == u2; break;  case 1: taken = u1 != u2; break;
            case 4: taken = s1 <  s2; break;  case 5: taken = s1 >= s2; break;
            case 6: taken = u1 <  u2; break;  case 7: taken = u1 >= u2; break;
        }
        if (taken) nextpc = cpu.pc + (i64)b_imm(instr);
        break;
    }

    case 0x03: {                                                              // loads
        u64 vaddr = (u64)(s1 + i_imm(instr));
        u64 addr  = mmu_translate(cpu, vaddr, 1, trap_cause);
        if (trap_cause) { trap_tval = vaddr; break; }
        switch (f3) {
            case 0: cpu.regs[rd] = (u64)(i64)(i8)  mem_read<u8> (cpu, addr); break;
            case 1: cpu.regs[rd] = (u64)(i64)(i16) mem_read<u16>(cpu, addr); break;
            case 2: cpu.regs[rd] = (u64)(i64)(i32) mem_read<u32>(cpu, addr); break;
            case 3: cpu.regs[rd] =                 mem_read<u64>(cpu, addr); break;
            case 4: cpu.regs[rd] =                 mem_read<u8> (cpu, addr); break;
            case 5: cpu.regs[rd] =                 mem_read<u16>(cpu, addr); break;
            case 6: cpu.regs[rd] =                 mem_read<u32>(cpu, addr); break;
        }
        break;
    }

    case 0x23: {                                                              // stores
        u64 vaddr = (u64)(s1 + s_imm(instr));
        u64 addr  = mmu_translate(cpu, vaddr, 2, trap_cause);
        if (trap_cause) { trap_tval = vaddr; break; }
        switch (f3) {
            case 0: mem_write<u8> (cpu, addr, (u8) u2); break;
            case 1: mem_write<u16>(cpu, addr, (u16)u2); break;
            case 2: mem_write<u32>(cpu, addr, (u32)u2); break;
            case 3: mem_write<u64>(cpu, addr,      u2); break;
        }
        break;
    }

    case 0x13: {                                                              // OP-IMM
        const i64 imm = i_imm(instr);
        const int sh  = (instr >> 20) & 0x3F;
        const int sra = (instr >> 30) & 1;
        u64 r = 0;
        switch (f3) {
            case 0: r = (u64)(s1 + imm);                          break;
            case 1: r = u1 << sh;                                 break;
            case 2: r = s1 < imm           ? 1u : 0u;             break;
            case 3: r = u1 < (u64)imm      ? 1u : 0u;             break;
            case 4: r = u1 ^ (u64)imm;                            break;
            case 5: r = sra ? (u64)(s1 >> sh) : (u1 >> sh);       break;
            case 6: r = u1 | (u64)imm;                            break;
            case 7: r = u1 & (u64)imm;                            break;
        }
        cpu.regs[rd] = r;
        break;
    }

    case 0x1B: {                                                              // OP-IMM-32
        const i32 imm = i_imm(instr);
        const int sh  = (instr >> 20) & 0x1F;
        const int sra = (instr >> 30) & 1;
        i32 r = 0;
        switch (f3) {
            case 0: r = (i32)((u32)s1 + (u32)imm);                break;  // ADDIW
            case 1: r = (i32)((u32)s1 << sh);                     break;  // SLLIW
            case 5: r = sra ? ((i32)s1 >> sh) : (i32)((u32)s1 >> sh); break;// SRLIW/SRAIW
        }
        cpu.regs[rd] = (u64)(i64)r;
        break;
    }

    case 0x33: {                                                              // OP
        u64 r = 0;
        if (f7 == 0x01) {
            r = exec_m(f3, u1, u2);
        } else {
            const int sh  = u2 & 0x3F;
            const int sub = (instr >> 30) & 1;
            switch (f3) {
                case 0: r = sub ? (u64)(s1 - s2) : (u64)(s1 + s2);    break;
                case 1: r = u1 << sh;                                 break;
                case 2: r = s1 < s2 ? 1u : 0u;                        break;
                case 3: r = u1 < u2 ? 1u : 0u;                        break;
                case 4: r = u1 ^ u2;                                  break;
                case 5: r = sub ? (u64)(s1 >> sh) : (u1 >> sh);       break;
                case 6: r = u1 | u2;                                  break;
                case 7: r = u1 & u2;                                  break;
            }
        }
        cpu.regs[rd] = r;
        break;
    }

    case 0x3B: {                                                              // OP-32
        i32 r = 0;
        if (f7 == 0x01) {
            cpu.regs[rd] = exec_mw(f3, (u32)u1, (u32)u2);
            break;
        }
        const int sh  = u2 & 0x1F;
        const int sub = (instr >> 30) & 1;
        switch (f3) {
            case 0: r = sub ? (i32)((u32)s1 - (u32)s2) : (i32)((u32)s1 + (u32)s2); break;
            case 1: r = (i32)((u32)s1 << sh);                                     break;
            case 5: r = sub ? ((i32)s1 >> sh) : (i32)((u32)s1 >> sh);             break;
        }
        cpu.regs[rd] = (u64)(i64)r;
        break;
    }

    case 0x2F: {                                                              // A-ext
        const u32 irmid = (instr >> 27) & 0x1F;
        const int wide  = (f3 == 3);                        // funct3: 2=.W 3=.D
        const u64 vaddr = u1;
        const u64 addr  = mmu_translate(cpu, vaddr, (irmid == 2) ? 1 : 2, trap_cause);
        if (trap_cause) { trap_tval = vaddr; break; }
        if (irmid == 2) {                                   // LR
            cpu.regs[rd] = wide ? mem_read<u64>(cpu, addr)
                                : (u64)(i64)(i32)mem_read<u32>(cpu, addr);
            cpu.rsv_addr = addr;
        } else if (irmid == 3) {                            // SC
            if (cpu.rsv_addr == addr) {
                if (wide) mem_write<u64>(cpu, addr, u2);
                else      mem_write<u32>(cpu, addr, (u32)u2);
                cpu.regs[rd] = 0;
            } else cpu.regs[rd] = 1;
            cpu.rsv_addr = ~0ull;
        } else {                                            // AMO
            if (wide) {
                u64 old = mem_read<u64>(cpu, addr), nw = old;
                switch (irmid) {
                    case  1: nw = u2;                                   break;
                    case  0: nw = old + u2;                             break;
                    case  4: nw = old ^ u2;                             break;
                    case 12: nw = old & u2;                             break;
                    case  8: nw = old | u2;                             break;
                    case 16: nw = (i64)u2 < (i64)old ? u2 : old;        break;
                    case 20: nw = (i64)u2 > (i64)old ? u2 : old;        break;
                    case 24: nw = u2 < old ? u2 : old;                  break;
                    case 28: nw = u2 > old ? u2 : old;                  break;
                }
                cpu.regs[rd] = old;
                mem_write<u64>(cpu, addr, nw);
            } else {
                u32 old = mem_read<u32>(cpu, addr), nw = old;
                u32 b = (u32)u2;
                switch (irmid) {
                    case  1: nw = b;                                    break;
                    case  0: nw = old + b;                              break;
                    case  4: nw = old ^ b;                              break;
                    case 12: nw = old & b;                              break;
                    case  8: nw = old | b;                              break;
                    case 16: nw = (i32)b < (i32)old ? b : old;          break;
                    case 20: nw = (i32)b > (i32)old ? b : old;          break;
                    case 24: nw = b < old ? b : old;                    break;
                    case 28: nw = b > old ? b : old;                    break;
                }
                cpu.regs[rd] = (u64)(i64)(i32)old;
                mem_write<u32>(cpu, addr, nw);
            }
        }
        break;
    }

    case 0x07: {                                                              // FLW / FLD
        u64 vaddr = (u64)(s1 + i_imm(instr));
        u64 addr  = mmu_translate(cpu, vaddr, 1, trap_cause);
        if (trap_cause) { trap_tval = vaddr; break; }
        if (f3 == 2) cpu.fregs[rd] = 0xFFFFFFFF00000000ull | mem_read<u32>(cpu, addr);
        else if (f3 == 3) cpu.fregs[rd] = mem_read<u64>(cpu, addr);
        break;
    }

    case 0x27: {                                                              // FSW / FSD
        u64 vaddr = (u64)(s1 + s_imm(instr));
        u64 addr  = mmu_translate(cpu, vaddr, 2, trap_cause);
        if (trap_cause) { trap_tval = vaddr; break; }
        if (f3 == 2) mem_write<u32>(cpu, addr, (u32)cpu.fregs[rs2]);
        else if (f3 == 3) mem_write<u64>(cpu, addr, cpu.fregs[rs2]);
        break;
    }

    case 0x43: case 0x47: case 0x4B: case 0x4F: {                             // FMADD family
        const int rs3 = (int)((instr >> 27) & 0x1F);
        const int dbl = ((instr >> 25) & 3) == 1;
        if (dbl) {
            double a=fd_get(cpu,rs1), b=fd_get(cpu,rs2), c=fd_get(cpu,rs3), r;
            r = opcode==0x43 ? a*b+c : opcode==0x47 ? a*b-c
              : opcode==0x4B ? -a*b+c : -a*b-c;
            fd_set(cpu,rd,r);
        } else {
            float a=fs_get(cpu,rs1), b=fs_get(cpu,rs2), c=fs_get(cpu,rs3), r;
            r = opcode==0x43 ? a*b+c : opcode==0x47 ? a*b-c
              : opcode==0x4B ? -a*b+c : -a*b-c;
            fs_set(cpu,rd,r);
        }
        break;
    }

    case 0x53:
        exec_fp(cpu, rd, rs1, rs2, f3, f7);
        break;

    case 0x0F: break;                                                         // FENCE

    case 0x73:
        trap_cause = exec_system(cpu, instr, rd, f3, nextpc, trap_tval);
        break;

    }

    // Any FP instruction marks the FP unit dirty — mstatus.FS = 3 (Dirty)
    // plus the SD summary bit. The kernel only saves/restores the
    // f-registers across a context switch when FS reads Dirty; without this
    // the f-regs are silently clobbered between processes, which surfaced as
    // -nan / garbage doubles in apk, mke2fs and Xorg.
    if (opcode == 0x53 || opcode == 0x07 || opcode == 0x27 ||
        opcode == 0x43 || opcode == 0x47 || opcode == 0x4B || opcode == 0x4F)
        cpu.csr_mstatus |= (3ull << 13) | (1ull << 63);

    if (trap_cause) {
        do_trap(cpu, trap_cause, trap_tval);
        cpu.regs[0] = 0;
        return;
    }
    cpu.regs[0] = 0;
    cpu.pc = nextpc;
}

// ── Wall-clock mtime (identical model to the RV32 core) ─────────────

static constexpr u64 TIMEBASE_HZ = 10'000'000ULL;   // matches the RV64 DTB patch

#define WIN32_LEAN_AND_MEAN     // drop winsock — its `fd_set` collides with ours
#include <windows.h>
static LARGE_INTEGER s_qpc_epoch, s_qpc_freq;
static inline u64 wallclock_ticks() {
    LARGE_INTEGER now; QueryPerformanceCounter(&now);
    u64 d = (u64)(now.QuadPart - s_qpc_epoch.QuadPart);
    return (d / s_qpc_freq.QuadPart) * TIMEBASE_HZ
         + (d % s_qpc_freq.QuadPart) * TIMEBASE_HZ / s_qpc_freq.QuadPart;
}
static inline void wallclock_reset() {
    QueryPerformanceFrequency(&s_qpc_freq);
    QueryPerformanceCounter(&s_qpc_epoch);
}

// ── Public C ABI ────────────────────────────────────────────────────

extern "C" int rv64_step_n(int n) {
    cpu.mtime = wallclock_ticks();
    for (int i = 0; i < n; i++) {
        do_step(cpu);
        if (__builtin_expect(cpu.halted, 0)) return -(i + 1);
    }
    return n;
}

extern "C" void rv64_init(u8* mem, u64 entry) {
    cpu           = {};
    cpu.pc        = entry;
    cpu.mtimecmp  = ~0ull;
    cpu.rsv_addr  = ~0ull;
    cpu.mem       = mem;
    cpu.priv_mode = 3;
    // SXL/UXL = 2 (rv64) so S/U-mode see a 64-bit XLEN.
    cpu.csr_mstatus = (2ull << 32) | (2ull << 34);
    g_sbi_mode    = 0;
    g_tlb_gen++;
    wallclock_reset();
    cpu.mtime = 0;
}

extern "C" void rv64_set_sbi_mode(int on) {
    g_sbi_mode = on ? 1 : 0;
    if (on) cpu.priv_mode = 1;
}

extern "C" void rv64_destroy() { cpu.mem = nullptr; }

extern "C" u64 rv64_dbg(u32 which) {
    switch (which) {
        case 0:  return cpu.csr_mstatus;
        case 1:  return cpu.csr_mie;
        case 2:  return cpu.csr_mip;
        case 3:  return cpu.csr_scause;
        case 4:  return cpu.csr_stvec;
        case 5:  return cpu.mtime;
        case 6:  return cpu.mtimecmp;
        case 7:  return g_trap_count;
        case 8:  return cpu.csr_satp;
        case 9:  return g_sbi_count;
        case 10: return g_sbi_timer_ct;
        case 11: return cpu.csr_sepc;
        case 12: return cpu.csr_stval;
        case 13: return cpu.csr_scause;
        default: return 0;
    }
}

extern "C" u64  rv64_get_pc()              { return cpu.pc; }
extern "C" int  rv64_is_halted()           { return cpu.halted; }
extern "C" void rv64_set_halted(int v)     { cpu.halted = v; }
extern "C" u64  rv64_get_priv_mode()       { return cpu.priv_mode; }

extern "C" u64  rv64_get_mtime()           { return wallclock_ticks(); }
extern "C" void rv64_set_mtime(u64 v)      { cpu.mtime = v; }
extern "C" u64  rv64_get_mtimecmp()        { return cpu.mtimecmp; }
extern "C" void rv64_set_mtimecmp(u64 v)   { cpu.mtimecmp = v; }
extern "C" u32  rv64_get_mtime_lo()        { return (u32) wallclock_ticks(); }
extern "C" u32  rv64_get_mtime_hi()        { return (u32)(wallclock_ticks() >> 32); }

extern "C" void rv64_set_reg(int i, u64 v) { if (i) cpu.regs[i & 31] = v; }

extern "C" void rv64_set_meip(int level) {
    if (level) cpu.csr_mip |=  (1u << 11);
    else       cpu.csr_mip &= ~(1u << 11);
}
extern "C" void rv64_set_seip(int level) {
    if (level) cpu.csr_mip |=  (1u << 9);
    else       cpu.csr_mip &= ~(1u << 9);
}

int __stdcall DllMain(void*, unsigned int, void*) { return 1; }
