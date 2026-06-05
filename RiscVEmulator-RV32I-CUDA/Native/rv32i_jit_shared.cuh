// rv32i_jit_shared.cuh — shared RV32IMA memory model + trap unit.
//
// This header holds the pieces that BOTH the CUDA interpreter
// (rv32i_cuda.cu) and the runtime-generated GPU JIT module
// (Core/Cuda/RvJitRuntime.cs → rv32i_jit_guest.dll) must share BYTE-FOR-BYTE
// IDENTICALLY: the memory-map constants, the CoreState/CoreMem/Periph/Hart
// structs, the little-endian access helpers, the full mem_read/mem_write
// dispatch (RAM, shared RO code image, framebuffer, PCM, trap page, MMIO,
// exit device), the MMIO device semantics, and the trap unit
// (do_trap / trap_return / trap_system / check_interrupts).
//
// Why a shared header instead of duplicated source: the JIT DLL is compiled
// separately at runtime, but it launches kernels against the SAME managed
// CoreState[]/CoreMem[] that the interpreter DLL allocated. For that to be
// safe the struct layouts and the memory semantics must be identical, which
// is only guaranteed if both translation units #include this one file. The
// interpreter (rv32i_cuda.cu) is unchanged in behaviour: it simply #includes
// this header for the definitions it used to declare inline.
//
// The JIT module additionally uses the jit_l*/jit_s*/jit_div* inline helpers
// at the bottom: their names/signatures match what Core/Cuda/RvJit.cs emits,
// and their slow path goes through the REAL mem_read/mem_write so that MMIO,
// framebuffer writes, the exit device and traps all behave exactly as in the
// interpreter.

#pragma once

#include <cstdint>
#include <cstring>
#include <cuda_runtime.h>

// ── Instruction prefetch window geometry (used by rv32i_cuda.cu's fetch
//    path; defined here so both TUs agree on the section size). ──
static constexpr uint32_t SEC_WORDS = 32;            // 32 instructions / section
static constexpr uint32_t SEC_BYTES = SEC_WORDS * 4; // 128 B

// ════════════════════════════════════════════════════════════════════
// Memory map
// ════════════════════════════════════════════════════════════════════

static constexpr uint32_t CLINT_BASE     = 0x02000000u, CLINT_SIZE  = 0x10000u;
static constexpr uint32_t TRAP_PAGE_BASE = 0x0F000000u, TRAP_PAGE_SIZE = 0x1000u;
static constexpr uint32_t UART_BASE      = 0x10000000u, UART_SIZE   = 0x100u;
static constexpr uint32_t KBD_BASE       = 0x10001000u, KBD_SIZE    = 0x100u;
static constexpr uint32_t MOUSE_BASE     = 0x10002000u, MOUSE_SIZE  = 0x100u;
static constexpr uint32_t RTC_BASE       = 0x10003000u, RTC_SIZE    = 0x100u;
static constexpr uint32_t MIDI_BASE      = 0x10005000u, MIDI_SIZE   = 0x100u;
static constexpr uint32_t FB_BASE        = 0x20000000u;
static constexpr uint32_t DISP_BASE      = 0x20100000u, DISP_SIZE   = 0x100u;
static constexpr uint32_t PCM_BASE       = 0x30000000u;
static constexpr uint32_t AUDIO_BASE     = 0x30100000u, AUDIO_SIZE  = 0x100u;
static constexpr uint32_t EXIT_BASE      = 0x40000000u, EXIT_SIZE   = 0x10u;

// Ring / FIFO sizes (per core, in the managed Periph page).
static constexpr uint32_t TXR = 1u << 16, TXM = TXR - 1u;   // UART TX
static constexpr uint32_t RXR = 1u << 12, RXM = RXR - 1u;   // UART RX
static constexpr uint32_t KBR = 1u << 10, KBM = KBR - 1u;   // keyboard FIFO
static constexpr uint32_t MDR = 1u << 12, MDM = MDR - 1u;   // MIDI ring

// Per-core peripheral page (managed). Device reads/writes these directly; the
// host reconciles between launches (drain tx → console, stage input, …).
struct Periph {
    uint32_t tx_head, tx_tail, rx_head, rx_tail;        // UART
    uint8_t  ier, lcr, mcr, scr;
    uint32_t kbd_head, kbd_tail, kbd_mod;               // keyboard FIFO + modifiers
    int32_t  mouse_dx, mouse_dy;                        // mouse
    uint32_t mouse_buttons, mouse_has;
    uint32_t midi_head, midi_tail;                      // MIDI ring
    uint32_t au_ctrl, au_rate, au_chan, au_bits, au_bufstart, au_buflen, au_pos, au_wrgen; // audio
    uint32_t dc_vsync, dc_mode, dc_fbaddr, dc_palidx;   // display
    uint32_t dc_pal[256];
    uint32_t rtc_us_lo, rtc_us_hi, rtc_ms_lo, rtc_ms_hi, rtc_epoch_lo, rtc_epoch_hi, rtc_sec, rtc_subus; // RTC
    uint32_t mtime_lo, mtime_hi, mtimecmp_lo, mtimecmp_hi; // CLINT
    uint8_t  tx[TXR];
    uint8_t  rx[RXR];
    uint32_t kbd[KBR];
    uint32_t midi[MDR];
};

// Privilege + interrupt-pin bits.
static constexpr uint32_t PRIV_U = 0u, PRIV_M = 3u;
static constexpr uint32_t PIN_MTIP = 1u << 7, PIN_MEIP = 1u << 11;

// Number of soft CSRs (Zicsr index space). Named so the interpreter, the JIT
// module and the host allocator all size the CSR backing store identically.
static constexpr uint32_t SOFT_CSR_COUNT = 4096u;

// Per-core state in managed memory. The hot fields are mirrored into a
// thread-local Hart for the duration of a launch; the soft CSR file (cold) is
// read / written in place through `soft_csr`.
//
// PICO (state-minimizer): `soft_csr` used to be an INLINE 4096-word array
// (16 KiB), which dominated sizeof(CoreState) and made the densely-packed
// g_state[] array 16 KiB/core. That bloats the per-core resident footprint and
// the per-launch state region for NO benefit on the throughput path (compute
// guests never touch a CSR). It is now a POINTER into a separately allocated
// backing store, dropping sizeof(CoreState) from ~16.4 KiB to ~64 B (≈256×).
// Semantics are unchanged: trap_system still does cpu.g->soft_csr[fn] exactly as
// before — `fn` is a full 12-bit index (0..SOFT_CSR_COUNT-1) and the backing
// store is SOFT_CSR_COUNT words, so every CSR read/write touches the identical
// word it always did. Where the backing lives (per-core buffers = baseline
// VRAM, or one contiguous slab = PICO) is an allocation choice ONLY and never
// changes a computed value — so the result is bit-for-bit identical either way.
struct CoreState {
    uint32_t regs[32];
    uint32_t pc;
    uint32_t priv;
    uint32_t pending;       // host interrupt pins
    int32_t  halted;
    int32_t  exitcode;
    int32_t  exited;
    uint32_t* soft_csr;     // → SOFT_CSR_COUNT-word backing store (managed)
};

struct CoreMem {
    uint8_t* ram;       // [0 .. ram_size)
    uint8_t* trap;      // 4 KiB @ 0x0F000000
    Periph*  per;
    uint8_t* fb;        // framebuffer @ 0x20000000
    uint8_t* pcm;       // audio PCM @ 0x30000000
    uint32_t ram_size;
    uint32_t fb_bytes;
    uint32_t pcm_bytes;
    uint32_t fb_w, fb_h;
    // Tier 1b — shared read-only code image. `code` is ONE managed buffer
    // shared by every core (same pointer in all CoreMem entries). Reads in
    // [code_lo, code_hi) are routed here instead of to per-core `ram`, so N
    // cores running the same image fetch from a single address range that
    // stays L2-resident and coalesces within a warp — instead of N distinct
    // copies that saturate DRAM bandwidth. Per-core `ram` still holds a full
    // copy (writes always go there); the guest never writes its own RO .text/
    // .rodata, so shared and per-core bytes in this range are always identical.
    // code_hi == code_lo (both 0 by default) disables it.
    uint8_t* code;
    uint32_t code_lo, code_hi;
};

// Working CPU state for one launch. The register file is indexed dynamically
// (regs[rs1]) — CUDA can't index the hardware register file, so an inline
// array would land in local memory (per-thread DRAM, L1-cached). Instead
// `regs` points at a per-thread slice of an on-chip __shared__ array (see the
// kernel). Scalars stay by-value in the struct. `g` backs the cold soft_csr.
//
// NOTE: the JIT module does NOT use h.regs (its guest registers live in R[32]
// locals for the speedup), but it DOES use h.pc/priv/pending/halted/g and the
// trap unit reads/writes them. The struct layout must therefore stay shared.
struct Hart {
    uint32_t* regs;     // → __shared__ register file (on-chip)
    uint32_t pc;
    uint32_t priv;
    uint32_t pending;
    int32_t  halted;
    int32_t  exitcode;
    int32_t  exited;
    CoreState* g;       // backing global state (soft_csr only)
    // Instruction prefetch window (PF path only): two 32-instruction sections
    // in shared memory; sbase[k] is the guest VA held by section k (0xFFFFFFFF
    // = empty), pend is a bitmask of sections with a cp.async copy in flight.
    uint32_t* sec;      // → __shared__ 2*SEC_WORDS region
    uint32_t  sbase[2];
    uint32_t  pend;
};

// ════════════════════════════════════════════════════════════════════
// Little-endian, alignment-safe access (GPU faults on misaligned typed deref)
// ════════════════════════════════════════════════════════════════════

template<class T>
static __device__ __forceinline__ T ld_le(const uint8_t* p, uint32_t a) {
    if constexpr (sizeof(T) == 1) {
        return (T)p[a];
    } else if constexpr (sizeof(T) == 2) {
        if ((a & 1u) == 0) return *(const T*)(p + a);
        return (T)((uint16_t)p[a] | ((uint16_t)p[a + 1] << 8));
    } else {
        if ((a & 3u) == 0) return *(const T*)(p + a);
        return (T)((uint32_t)p[a] | ((uint32_t)p[a + 1] << 8)
                 | ((uint32_t)p[a + 2] << 16) | ((uint32_t)p[a + 3] << 24));
    }
}

template<class T>
static __device__ __forceinline__ void st_le(uint8_t* p, uint32_t a, T v) {
    if constexpr (sizeof(T) == 1) {
        p[a] = (uint8_t)v;
    } else if constexpr (sizeof(T) == 2) {
        if ((a & 1u) == 0) { *(T*)(p + a) = v; return; }
        p[a] = (uint8_t)v; p[a + 1] = (uint8_t)(v >> 8);
    } else {
        if ((a & 3u) == 0) { *(T*)(p + a) = v; return; }
        p[a]     = (uint8_t)v;        p[a + 1] = (uint8_t)(v >> 8);
        p[a + 2] = (uint8_t)(v >> 16); p[a + 3] = (uint8_t)(v >> 24);
    }
}

// ════════════════════════════════════════════════════════════════════
// MMIO — `if (range)` replacement for the host VEH. Cold path (not inlined).
// ════════════════════════════════════════════════════════════════════

static __device__ uint32_t mmio_read(Hart& h, CoreMem& m, uint32_t a, int /*w*/) {
    Periph* p = m.per;

    if (a - UART_BASE < UART_SIZE) {
        switch (a - UART_BASE) {
            case 0x00: if (p->rx_tail != p->rx_head) return p->rx[p->rx_tail++ & RXM]; return 0;
            case 0x01: return p->ier;
            case 0x02: return 0xC0u;
            case 0x03: return p->lcr;
            case 0x04: return p->mcr;
            case 0x05: return (p->rx_tail != p->rx_head ? 1u : 0u) | 0x60u;
            case 0x06: return 0x30u;
            case 0x07: return p->scr;
        }
        return 0;
    }
    if (a - KBD_BASE < KBD_SIZE) {
        switch (a - KBD_BASE) {
            case 0x00: return p->kbd_tail != p->kbd_head ? 1u : 0u;
            case 0x04: if (p->kbd_tail != p->kbd_head) return p->kbd[p->kbd_tail++ & KBM]; return 0;
            case 0x08: return p->kbd_mod;
        }
        return 0;
    }
    if (a - MOUSE_BASE < MOUSE_SIZE) {
        switch (a - MOUSE_BASE) {
            case 0x00: return p->mouse_has;
            case 0x04: { uint32_t v = (uint32_t)p->mouse_dx; p->mouse_dx = 0;
                         p->mouse_has = (p->mouse_dx || p->mouse_dy || p->mouse_buttons) ? 1u : 0u; return v; }
            case 0x08: { uint32_t v = (uint32_t)p->mouse_dy; p->mouse_dy = 0;
                         p->mouse_has = (p->mouse_dx || p->mouse_dy || p->mouse_buttons) ? 1u : 0u; return v; }
            case 0x0C: return p->mouse_buttons;
        }
        return 0;
    }
    if (a - RTC_BASE < RTC_SIZE) {
        switch (a - RTC_BASE) {
            case 0x00: return p->rtc_us_lo;   case 0x04: return p->rtc_us_hi;
            case 0x08: return p->rtc_ms_lo;   case 0x0C: return p->rtc_ms_hi;
            case 0x10: return p->rtc_epoch_lo;case 0x14: return p->rtc_epoch_hi;
            case 0x18: return p->rtc_sec;     case 0x1C: return p->rtc_subus;
        }
        return 0;
    }
    if (a - MIDI_BASE < MIDI_SIZE) {
        if ((a - MIDI_BASE) == 0x00) return 1u;
        return 0;
    }
    if (a - DISP_BASE < DISP_SIZE) {
        switch (a - DISP_BASE) {
            case 0x00: return m.fb_w;
            case 0x04: return m.fb_h;
            case 0x08: return 32u;
            case 0x0C: return p->dc_vsync;
            case 0x18: return p->dc_mode;
            case 0x1C: return p->dc_fbaddr;
        }
        return 0;
    }
    if (a - AUDIO_BASE < AUDIO_SIZE) {
        switch (a - AUDIO_BASE) {
            case 0x00: return p->au_ctrl;
            case 0x04: return (p->au_ctrl & 1u) ? 1u : 0u;
            case 0x08: return p->au_rate;
            case 0x0C: return p->au_chan;
            case 0x10: return p->au_bits;
            case 0x14: return p->au_bufstart;
            case 0x18: return p->au_buflen;
            case 0x1C: return p->au_pos;
        }
        return 0;
    }
    if (a - CLINT_BASE < CLINT_SIZE) {
        switch (a - CLINT_BASE) {
            case 0x0BFF8: return p->mtime_lo;     case 0x0BFFC: return p->mtime_hi;
            case 0x04000: return p->mtimecmp_lo;  case 0x04004: return p->mtimecmp_hi;
        }
        return 0;
    }
    return 0;
}

static __device__ void mmio_write(Hart& h, CoreMem& m, uint32_t a, int /*w*/, uint32_t val) {
    Periph* p = m.per;

    if (a - UART_BASE < UART_SIZE) {
        switch (a - UART_BASE) {
            case 0x00: p->tx[p->tx_head++ & TXM] = (uint8_t)val; break;
            case 0x01: p->ier = (uint8_t)val; break;
            case 0x03: p->lcr = (uint8_t)val; break;
            case 0x04: p->mcr = (uint8_t)val; break;
            case 0x07: p->scr = (uint8_t)val; break;
        }
        return;
    }
    if (a - MIDI_BASE < MIDI_SIZE) {
        uint32_t off = a - MIDI_BASE;
        if (off == 0x04 || off == 0x08 || off == 0x0C)
            p->midi[p->midi_head++ & MDM] = (off << 24) | (val & 0x00FFFFFFu);
        return;
    }
    if (a - DISP_BASE < DISP_SIZE) {
        switch (a - DISP_BASE) {
            case 0x0C: p->dc_vsync = val; break;
            case 0x10: p->dc_palidx = val & 0xFFu; break;
            case 0x14: p->dc_pal[p->dc_palidx & 0xFFu] = val; break;
            case 0x18: p->dc_mode = val; break;
            case 0x1C: p->dc_fbaddr = val; break;
        }
        return;
    }
    if (a - AUDIO_BASE < AUDIO_SIZE) {
        switch (a - AUDIO_BASE) {
            case 0x00: if (val & 4u) { p->au_ctrl = 0; p->au_pos = 0; }
                       else { if (val & 1u) p->au_wrgen++; p->au_ctrl = val & 3u; } break;
            case 0x08: p->au_rate = val; break;
            case 0x0C: p->au_chan = val; break;
            case 0x10: p->au_bits = val; break;
            case 0x14: p->au_bufstart = val; break;
            case 0x18: p->au_buflen = val; break;
        }
        return;
    }
    if (a - CLINT_BASE < CLINT_SIZE) {
        switch (a - CLINT_BASE) {
            case 0x04000: p->mtimecmp_lo = val; break;
            case 0x04004: p->mtimecmp_hi = val; break;
        }
        return;
    }
    if (a - EXIT_BASE < EXIT_SIZE) {
        if ((a - EXIT_BASE) == 0) { h.exitcode = (int32_t)val; h.exited = 1; h.halted = 1; }
        return;
    }
}

// ── Memory access seam ────────────────────────────────────────────────
template<class T>
static __device__ __forceinline__ T mem_read(Hart& h, CoreMem& m, uint32_t a) {
    // Tier 1b: shared RO code first — the instruction fetch (and rodata reads)
    // hit ONE buffer shared by all cores, so same-PC lanes/warps coalesce and
    // stay L2-resident instead of streaming N private copies from DRAM.
    if (a - m.code_lo < m.code_hi - m.code_lo) return ld_le<T>(m.code, a - m.code_lo);
    if (a < m.ram_size)                      return ld_le<T>(m.ram, a);
    if (a - FB_BASE   < m.fb_bytes)          return ld_le<T>(m.fb,  a - FB_BASE);
    if (a - PCM_BASE  < m.pcm_bytes)         return ld_le<T>(m.pcm, a - PCM_BASE);
    if (a - TRAP_PAGE_BASE < TRAP_PAGE_SIZE) return ld_le<T>(m.trap, a - TRAP_PAGE_BASE);
    return (T)mmio_read(h, m, a, (int)sizeof(T));
}
template<class T>
static __device__ __forceinline__ void mem_write(Hart& h, CoreMem& m, uint32_t a, T v) {
    if (a < m.ram_size)                      { st_le<T>(m.ram, a, v); return; }
    if (a - FB_BASE   < m.fb_bytes)          { st_le<T>(m.fb,  a - FB_BASE,  v); return; }
    if (a - PCM_BASE  < m.pcm_bytes)         { st_le<T>(m.pcm, a - PCM_BASE, v); return; }
    if (a - TRAP_PAGE_BASE < TRAP_PAGE_SIZE) { st_le<T>(m.trap, a - TRAP_PAGE_BASE, v); return; }
    mmio_write(h, m, a, (int)sizeof(T), (uint32_t)v);
}

// Everything below to the matching #endif (trap unit + the cpu_step interpreter
// + jit_interp_step) is the COLD path. JIT slice (.cu part) files use only the
// structs, memory model and jit_l*/jit_s*/jit_div* helpers — so they
// `#define RVJIT_MEM_ONLY` before including this header and skip the giant
// interpreter switch, which otherwise cicc would recompile in EVERY part file.
#ifndef RVJIT_MEM_ONLY

// ════════════════════════════════════════════════════════════════════
// TRAP UNIT (cold; not inlined) — shared so JIT'd ECALL/illegal/IRQ traps
// match the interpreter exactly. The JIT routes case 0x73 (SYSTEM) and any
// unhandled opcode to its `interp:` label, which runs the interpreter step
// that calls into this unit; the JIT itself only consults check_interrupts +
// the PV_RESUME_GATEWAY fast-path at its dispatch points.
// ════════════════════════════════════════════════════════════════════

static constexpr uint32_t STATUS_IE = 1u<<3, STATUS_PIE = 1u<<7, STATUS_PP = 3u<<11;
static constexpr uint32_t CAUSE_ILLEGAL = 2u, CAUSE_EBREAK = 3u, CAUSE_ECALL_U = 8u, CAUSE_ECALL_M = 11u;
static constexpr uint32_t CAUSE_IRQ_MTIP = 0x80000007u, CAUSE_IRQ_MEIP = 0x8000000Bu;

static constexpr uint32_t IE_FLAG      = TRAP_PAGE_BASE + 0x000u;
static constexpr uint32_t TRAP_VECTOR  = TRAP_PAGE_BASE + 0x004u;
static constexpr uint32_t IE_MASK      = TRAP_PAGE_BASE + 0x008u;
static constexpr uint32_t TRAP_SCRATCH = TRAP_PAGE_BASE + 0x00Cu;
static constexpr uint32_t FRAME_BASE   = TRAP_PAGE_BASE + 0x100u;
static constexpr uint32_t FRAME_STATUS = FRAME_BASE + 32u*4u;
static constexpr uint32_t FRAME_TVAL   = FRAME_BASE + 33u*4u;
static constexpr uint32_t FRAME_CAUSE  = FRAME_BASE + 34u*4u;
static constexpr uint32_t PV_RESUME_GATEWAY = 0xFFFF0004u;

static __device__ void do_trap(Hart& cpu, CoreMem& mm, uint32_t cause, uint32_t tval) {
    uint32_t tp = cpu.regs[4];
    cpu.regs[4] = mem_read<uint32_t>(cpu, mm, TRAP_SCRATCH);
    mem_write<uint32_t>(cpu, mm, TRAP_SCRATCH, tp);
    mem_write<uint32_t>(cpu, mm, FRAME_BASE, cpu.pc);
    for (uint32_t i = 1; i < 32; i++)
        mem_write<uint32_t>(cpu, mm, FRAME_BASE + i * 4u, cpu.regs[i]);
    uint32_t pie = (mem_read<uint32_t>(cpu, mm, IE_FLAG) & STATUS_IE) ? STATUS_PIE : 0u;
    uint32_t pp  = (cpu.priv == PRIV_M) ? STATUS_PP : 0u;
    mem_write<uint32_t>(cpu, mm, FRAME_STATUS, pie | pp);
    mem_write<uint32_t>(cpu, mm, FRAME_TVAL,  tval);
    mem_write<uint32_t>(cpu, mm, FRAME_CAUSE, cause);
    mem_write<uint32_t>(cpu, mm, IE_FLAG, 0u);
    cpu.priv = PRIV_M;
    uint32_t tvec = mem_read<uint32_t>(cpu, mm, TRAP_VECTOR);
    if (tvec == 0) tvec = mem_read<uint32_t>(cpu, mm, IE_MASK);
    cpu.pc = tvec;
}

static __device__ void trap_return(Hart& cpu, CoreMem& mm) {
    uint32_t fb     = cpu.regs[10];
    uint32_t status = mem_read<uint32_t>(cpu, mm, fb + FRAME_STATUS - FRAME_BASE);
    for (uint32_t i = 1; i < 32; i++)
        cpu.regs[i] = mem_read<uint32_t>(cpu, mm, fb + i * 4u);
    mem_write<uint32_t>(cpu, mm, IE_FLAG, (status & STATUS_PIE) ? STATUS_IE : 0u);
    cpu.priv = (status & STATUS_PP) ? PRIV_M : PRIV_U;
    cpu.pc = mem_read<uint32_t>(cpu, mm, fb);
}

static __device__ void trap_system(Hart& cpu, CoreMem& mm, uint32_t instr) {
    uint32_t f3 = (instr >> 12) & 0x7;
    uint32_t fn = (instr >> 20) & 0xFFF;
    if (f3 == 0 && fn == 0x000) { do_trap(cpu, mm, cpu.priv == PRIV_M ? CAUSE_ECALL_M : CAUSE_ECALL_U, 0); return; }
    if (f3 == 0 && fn == 0x001) { do_trap(cpu, mm, CAUSE_EBREAK, cpu.pc); return; }
    if (f3 == 0)                { do_trap(cpu, mm, CAUSE_ILLEGAL, instr); return; }
    uint32_t rd  = (instr >> 7)  & 0x1F;
    uint32_t rs1 = (instr >> 15) & 0x1F;
    uint32_t old = cpu.g->soft_csr[fn];
    uint32_t src = (f3 & 4) ? rs1 : cpu.regs[rs1];
    uint32_t op  = f3 & 3;
    bool write = (op == 1) || (rs1 != 0);
    if (write) {
        uint32_t nv = (op == 1) ? src : (op == 2) ? (old | src) : (old & ~src);
        cpu.g->soft_csr[fn] = nv;
    }
    if (rd) cpu.regs[rd] = old;
    cpu.pc += 4;
}

static __device__ __forceinline__ bool check_interrupts(Hart& cpu, CoreMem& mm) {
    if (!cpu.pending) return false;
    uint32_t pend = cpu.pending & mem_read<uint32_t>(cpu, mm, IE_MASK);
    if (!pend || !(mem_read<uint32_t>(cpu, mm, IE_FLAG) & STATUS_IE)) return false;
    if (pend & PIN_MEIP) { do_trap(cpu, mm, CAUSE_IRQ_MEIP, 0); return true; }
    if (pend & PIN_MTIP) { do_trap(cpu, mm, CAUSE_IRQ_MTIP, 0); return true; }
    return false;
}

// ════════════════════════════════════════════════════════════════════
// RV32IMA single-instruction interpreter step.
//
// Shared so the JIT module's `interp:` fallback runs the EXACT same step the
// interpreter does for opcodes the translator doesn't cover (SYSTEM/0x73,
// and any address it didn't translate — e.g. JALR into the WAD region). The
// JIT syncs its R[32] locals into h.regs[] (the on-chip register slice) before
// calling jit_interp_step and reads them back after, so register state stays
// coherent across the JIT↔interpreter seam.
// ════════════════════════════════════════════════════════════════════

enum : uint32_t { EXC_NONE = 0, EXC_ILLEGAL = 1, EXC_SYSTEM = 2 };
struct CpuException { uint32_t kind, instr; };

static constexpr uint32_t j_imm(uint32_t i) {
    uint32_t v = ((i>>31)&1u)<<20 | ((i>>12)&0xFFu)<<12 | ((i>>20)&1u)<<11 | ((i>>21)&0x3FFu)<<1;
    return (v & 0x100000u) ? v | 0xFFE00000u : v;
}
static constexpr uint32_t b_imm(uint32_t i) {
    uint32_t v = ((i>>31)&1u)<<12 | ((i>>7)&1u)<<11 | ((i>>25)&0x3Fu)<<5 | ((i>>8)&0xFu)<<1;
    return (v & 0x1000u) ? v | 0xFFFFE000u : v;
}
static constexpr int32_t i_imm(uint32_t i) { return (int32_t)i >> 20; }
static constexpr int32_t s_imm(uint32_t i) {
    return ((int32_t)(i & 0xFE000000) >> 20) | (int32_t)((i >> 7) & 0x1Fu);
}

// ── Branchless primitives (BOOL branch-eliminator experiment) ──
//
// These compute the BRANCH-condition and the OP/OP-IMM ALU result with no
// data-dependent control flow, so ptxas keeps the warp converged on the hot
// integer path (the common case: every lane in a warp running the same image
// usually executes the SAME opcode, but f3 still varies and the nested
// switch(f3) is what ptxas had to lower to a jump table / chained branch).
//
// Bit-exactness: each select below is the algebraic identity of the
// corresponding switch arm. `?:` over scalars is just an x86/PTX select
// (no branch). The full f3∈[0..7] arithmetic mux is computed unconditionally
// and the right lane is picked by index — same value the switch produced,
// no observable side effects (pure register math, no memory touched).

// BRANCH taken predicate, branchless over f3.
static __device__ __forceinline__ int branch_taken_bl(uint32_t f3,
        uint32_t u1, uint32_t u2, int32_t s1, int32_t s2) {
    // f3: 0 BEQ 1 BNE 4 BLT 5 BGE 6 BLTU 7 BGEU. f3 bit2 selects signed-vs-
    // unsigned LT family; bit0 inverts (NE/GE). bit1 selects EQ-family vs
    // LT-family. Compute the two base predicates then mux.
    int eq = (u1 == u2);                  // EQ result (f3 0/1)
    int lt = (f3 & 4u) ? (u1 < u2)        // unsigned LT (f3 6/7)
                       : (s1 < s2);       // signed   LT (f3 4/5)
    // bit2 set ⇒ LT family (4..7); bit2 clear ⇒ EQ family (0/1).
    int base = (f3 & 4u) ? lt : eq;
    int taken = base ^ (int)(f3 & 1u);    // bit0 inverts (NE, BGE, BGEU)
    return taken;
}

// OP / OP-IMM ALU mux, branchless over f3. `arg2` is rs2 (OP) or imm (OP-IMM);
// `sh` is the 5-bit shift amount. `sub` selects SUB over ADD (only OP with
// f7==0x20,f3==0 — OP-IMM ADDI is ALWAYS add, so callers pass sub=false there).
// `sra` selects SRA over SRL (f7==0x20, f3==5, both OP and OP-IMM).
static __device__ __forceinline__ uint32_t alu_bl(uint32_t f3,
        uint32_t u1, int32_t s1, uint32_t arg2u, int32_t arg2s,
        uint32_t sh, bool sub, bool sra) {
    // Compute every arm; index-select the live one. ptxas turns this into a
    // straight run of ALU ops + selects (no divergent branch).
    uint32_t add = sub ? (u1 - arg2u) : (u1 + arg2u);       // f3 0
    uint32_t sll = u1 << sh;                                // f3 1
    uint32_t slt = (uint32_t)(s1 < arg2s);                  // f3 2
    uint32_t sltu= (uint32_t)(u1 < arg2u);                  // f3 3
    uint32_t xr  = u1 ^ arg2u;                              // f3 4
    uint32_t sr  = sra ? (uint32_t)(s1 >> sh) : (u1 >> sh); // f3 5
    uint32_t orr = u1 | arg2u;                              // f3 6
    uint32_t andr= u1 & arg2u;                              // f3 7
    // 8-way select by a clean 3-bit decode (each ?: is a PTX selp, no branch):
    //   0 add 1 sll 2 slt 3 sltu 4 xor 5 sr 6 or 7 and
    uint32_t e0 = (f3 & 4u) ? xr  : add;   // 0->add 4->xor
    uint32_t e1 = (f3 & 4u) ? sr  : sll;   // 1->sll 5->sr
    uint32_t e2 = (f3 & 4u) ? orr : slt;   // 2->slt 6->or
    uint32_t e3 = (f3 & 4u) ? andr: sltu;  // 3->sltu 7->and
    uint32_t res = (f3 & 1u) ? ((f3 & 2u) ? e3 : e1)
                             : ((f3 & 2u) ? e2 : e0);
    return res;
}

// ── Frequency-biased predicated fast path (PREDICT experiment) ────────────
// A tiny branchless-style ladder that handles the ~5 opcodes that dominate the
// dynamic instruction count (ADDI, ADD/SUB, LW, SW, conditional BRANCH) BEFORE
// the full opcode switch, so the common case never pays the jump-table
// dispatch. ptxas folds this straight-line predicated prologue into a handful
// of compare-and-select ops; within a warp every lane runs the same guest at
// the same PC so the predicate is uniform and divergence-free.
//
// Equivalence with the switch arms it shortcuts (each is the EXACT body of the
// corresponding `case` below, verified per opcode in PERF_PREDICT.md):
//   • ADDI  (0x13,f3=0)            regs[rd] = (uint32_t)(s1 + imm)
//   • ADD   (0x33,f7=0x00,f3=0)    regs[rd] = (uint32_t)(s1 + s2)
//   • SUB   (0x33,f7=0x20,f3=0)    regs[rd] = (uint32_t)(s1 - s2)
//   • LW    (0x03,f3=2)            regs[rd] = mem_read<uint32_t>(addr)
//   • SW    (0x23,f3=2)            mem_write<uint32_t>(addr, u2)
//   • BRANCH(0x63, all f3)         nextpc = taken ? pc+b_imm : pc+4
// Every other encoding (including OP-IMM/OP sub-functions, half/byte loads &
// stores, illegal f7 combos that must trap) is NOT matched here and falls
// through to the switch unchanged — so the fast path can only ever produce a
// result identical to the switch. Flag OFF (g_fastpath_on==0) skips the ladder
// entirely, reproducing the baseline switch bit-for-bit.
//
// Defined here (not extern) so EVERY module that compiles cpu_step gets its own
// definition: both the interpreter DLL (rv32i_cuda.cu) and the runtime JIT DLL
// (RvJitRuntime's generated .cu, whose jit_interp_step→cpu_step references it)
// resolve the symbol locally. This block is inside #ifndef RVJIT_MEM_ONLY, so
// the JIT *part* files (which define RVJIT_MEM_ONLY and skip cpu_step) do not
// also define it — only the one TU per module that actually compiles cpu_step.
// The host toggles the interpreter DLL's copy via cudaMemcpyToSymbol.
__device__ int g_fastpath_on = 1;

template<bool BRANCHLESS = true>
static __device__ __forceinline__ CpuException cpu_step(Hart& cpu, CoreMem& mm, uint32_t instr) {
    const int      rd    = (instr >>  7) & 0x1F;
    const uint32_t f3    = (instr >> 12) & 0x7;
    const uint32_t f7    = (instr >> 25) & 0x7F;
    const uint32_t u1    = cpu.regs[(instr >> 15) & 0x1F];
    const uint32_t u2    = cpu.regs[(instr >> 20) & 0x1F];
    const int32_t  s1    = (int32_t)u1;
    const int32_t  s2    = (int32_t)u2;
    uint32_t nextpc      = cpu.pc + 4;
    const uint32_t op    = instr & 0x7F;

    if (g_fastpath_on) {
        // ADDI / ADD / SUB → register write, nextpc = pc+4. Bitwise predicates
        // (no short-circuit) so ptxas emits compare-and-select, not branches.
        //   addi: op==0x13 && f3==0
        //   add : op==0x33 && f3==0 && f7==0x00
        //   sub : op==0x33 && f3==0 && f7==0x20
        const bool isAddi = (op == 0x13) & (f3 == 0);
        const bool isAdd  = (op == 0x33) & (f3 == 0) & (f7 == 0x00);
        const bool isSub  = (op == 0x33) & (f3 == 0) & (f7 == 0x20);
        if (isAddi | isAdd | isSub) {
            // Compute in unsigned to wrap defined-ly; result is bit-identical to
            // (uint32_t)(s1+imm) / (s1+s2) / (s1-s2) since two's-complement add
            // and subtract are the same bit pattern as unsigned add/subtract.
            const uint32_t b = isAddi ? (uint32_t)i_imm(instr) : u2;
            cpu.regs[rd] = isSub ? (u1 - b) : (u1 + b);
            cpu.regs[0]  = 0;
            cpu.pc       = nextpc;
            return { EXC_NONE, 0 };
        }
        // LW (0x03,f3=2): word load, sign-irrelevant. Identical to case 0x03/f3==2.
        if ((op == 0x03) & (f3 == 2)) {
            uint32_t addr = (uint32_t)(s1 + i_imm(instr));
            cpu.regs[rd]  = mem_read<uint32_t>(cpu, mm, addr);
            cpu.regs[0]   = 0;
            cpu.pc        = nextpc;
            return { EXC_NONE, 0 };
        }
        // SW (0x23,f3=2): word store. Identical to case 0x23/f3==2.
        if ((op == 0x23) & (f3 == 2)) {
            uint32_t addr = (uint32_t)(s1 + s_imm(instr));
            mem_write<uint32_t>(cpu, mm, addr, u2);
            cpu.regs[0]   = 0;   // store never writes rd; keep x0 sink invariant
            cpu.pc        = nextpc;
            return { EXC_NONE, 0 };
        }
        // BRANCH (0x63, all f3): identical to case 0x63. f3 1/2/3 are illegal
        // RISC-V encodings but the switch's default `taken=0` falls straight to
        // pc+4 (no trap), so we reproduce that with taken=0 — bit-exact.
        if (op == 0x63) {
            int taken = 0;
            switch (f3) {
                case 0: taken = u1 == u2; break;  case 1: taken = u1 != u2; break;
                case 4: taken = s1 <  s2; break;  case 5: taken = s1 >= s2; break;
                case 6: taken = u1 <  u2; break;  case 7: taken = u1 >= u2; break;
            }
            if (taken) nextpc = cpu.pc + b_imm(instr);
            cpu.regs[0] = 0;
            cpu.pc      = nextpc;
            return { EXC_NONE, 0 };
        }
    }

    switch (op) {
    case 0x37: cpu.regs[rd] = instr & 0xFFFFF000u;                        break;  // LUI
    case 0x17: cpu.regs[rd] = cpu.pc + (instr & 0xFFFFF000u);             break;  // AUIPC
    case 0x6F: cpu.regs[rd] = cpu.pc + 4; nextpc = cpu.pc + j_imm(instr); break;  // JAL
    case 0x67: { uint32_t t = (uint32_t)(s1 + i_imm(instr)) & ~1u;               // JALR
                 cpu.regs[rd] = cpu.pc + 4; nextpc = t;                   break; }
    case 0x63: {                                                                 // BRANCH
        int taken;
        if constexpr (BRANCHLESS) {
            taken = branch_taken_bl(f3, u1, u2, s1, s2);
        } else {
            taken = 0;
            switch (f3) {
                case 0: taken = u1 == u2; break;  case 1: taken = u1 != u2; break;
                case 4: taken = s1 <  s2; break;  case 5: taken = s1 >= s2; break;
                case 6: taken = u1 <  u2; break;  case 7: taken = u1 >= u2; break;
            }
        }
        // Branchless next-pc select: avoids the data-dependent `if (taken)`.
        uint32_t tgt = cpu.pc + b_imm(instr);
        nextpc = taken ? tgt : nextpc;
        break;
    }
    case 0x03: {                                                                 // LOAD
        uint32_t addr = (uint32_t)(s1 + i_imm(instr));
        switch (f3) {
            case 0: cpu.regs[rd] = (uint32_t)(int8_t) mem_read<uint8_t> (cpu, mm, addr); break;
            case 1: cpu.regs[rd] = (uint32_t)(int16_t)mem_read<uint16_t>(cpu, mm, addr); break;
            case 2: cpu.regs[rd] =                    mem_read<uint32_t>(cpu, mm, addr); break;
            case 4: cpu.regs[rd] =                    mem_read<uint8_t> (cpu, mm, addr); break;
            case 5: cpu.regs[rd] =                    mem_read<uint16_t>(cpu, mm, addr); break;
        }
        break;
    }
    case 0x23: {                                                                 // STORE
        uint32_t addr = (uint32_t)(s1 + s_imm(instr));
        switch (f3) {
            case 0: mem_write<uint8_t> (cpu, mm, addr, (uint8_t) u2); break;
            case 1: mem_write<uint16_t>(cpu, mm, addr, (uint16_t)u2); break;
            case 2: mem_write<uint32_t>(cpu, mm, addr,           u2); break;
        }
        break;
    }
    case 0x13: {                                                                 // OP-IMM
        const int32_t imm = i_imm(instr);
        const int     sh  = (instr >> 20) & 0x1F;
        // Illegal-funct: SLLI must have f7==0; SRLI/SRAI f7∈{0,0x20}.
        // Branchless predicate (bitwise OR of the two violation flags) sets a
        // single bool; the rare illegal case takes the cold return.
        const bool bad = ((f3 == 1u) & (f7 != 0x00u))
                       | ((f3 == 5u) & (f7 != 0x00u) & (f7 != 0x20u));
        if (bad) return { EXC_ILLEGAL, instr };
        uint32_t r;
        if constexpr (BRANCHLESS) {
            r = alu_bl(f3, u1, s1, (uint32_t)imm, imm, (uint32_t)sh,
                       /*sub=*/false, /*sra=*/f7 == 0x20u);
        } else {
            r = 0;
            switch (f3) {
                case 0: r = (uint32_t)(s1 + imm);                         break;
                case 1: r = u1 << sh;                                     break;
                case 2: r = s1 < imm           ? 1u : 0u;                 break;
                case 3: r = u1 < (uint32_t)imm ? 1u : 0u;                 break;
                case 4: r = u1 ^ (uint32_t)imm;                           break;
                case 5: r = f7 == 0x20 ? (uint32_t)(s1 >> sh) : u1 >> sh; break;
                case 6: r = u1 | (uint32_t)imm;                           break;
                case 7: r = u1 & (uint32_t)imm;                           break;
            }
        }
        cpu.regs[rd] = r;
        break;
    }
    case 0x33: {                                                                 // OP (incl. M)
        uint32_t r;
        if (f7 == 0x01) {                                                        // M extension
            switch (f3) {
                case 0: r = (uint32_t)(u1 * u2);                                       break; // MUL
                case 1: r = (uint32_t)(((int64_t)s1 * (int64_t)s2) >> 32);             break; // MULH
                case 2: r = (uint32_t)(((int64_t)s1 * (int64_t)(uint64_t)u2) >> 32);   break; // MULHSU
                case 3: r = (uint32_t)(((uint64_t)u1 * (uint64_t)u2) >> 32);           break; // MULHU
                case 4: r = (s2 == 0) ? 0xFFFFFFFFu                                            // DIV
                          : (s1 == (int32_t)0x80000000 && s2 == -1) ? 0x80000000u
                          : (uint32_t)(s1 / s2);                                       break;
                case 5: r = (u2 == 0) ? 0xFFFFFFFFu : (u1 / u2);                       break; // DIVU
                case 6: r = (s2 == 0) ? u1                                                     // REM
                          : (s1 == (int32_t)0x80000000 && s2 == -1) ? 0u
                          : (uint32_t)(s1 % s2);                                       break;
                case 7: r = (u2 == 0) ? u1 : (u1 % u2);                               break; // REMU
                default: r = 0; break;
            }
        } else if (f7 != 0x00 && !(f7 == 0x20 && (f3 == 0 || f3 == 5))) {
            return { EXC_ILLEGAL, instr };
        } else {
            const uint32_t sh = (uint32_t)(s2 & 0x1F);
            if constexpr (BRANCHLESS) {
                r = alu_bl(f3, u1, s1, u2, s2, sh,
                           /*sub=*/f7 == 0x20u, /*sra=*/f7 == 0x20u);
            } else {
                switch (f3) {
                    case 0: r = f7 == 0x20 ? (uint32_t)(s1 - s2) : (uint32_t)(s1 + s2); break;
                    case 1: r = u1 << sh;                                              break;
                    case 2: r = s1 < s2 ? 1u : 0u;                                     break;
                    case 3: r = u1 < u2 ? 1u : 0u;                                     break;
                    case 4: r = u1 ^ u2;                                               break;
                    case 5: r = f7 == 0x20 ? (uint32_t)(s1 >> sh) : u1 >> sh;          break;
                    case 6: r = u1 | u2;                                               break;
                    default:r = u1 & u2;                                               break;
                }
            }
        }
        cpu.regs[rd] = r;
        break;
    }
    case 0x2F: {                                                                 // A extension (.W)
        if (f3 != 2) return { EXC_ILLEGAL, instr };
        uint32_t addr = u1;
        uint32_t f5   = (instr >> 27) & 0x1F;
        if (f5 == 0x02) {                                  // LR.W (single-hart: plain load)
            cpu.regs[rd] = mem_read<uint32_t>(cpu, mm, addr);
        } else if (f5 == 0x03) {                           // SC.W (always succeeds)
            mem_write<uint32_t>(cpu, mm, addr, u2);
            cpu.regs[rd] = 0;
        } else {                                           // AMO*
            uint32_t t = mem_read<uint32_t>(cpu, mm, addr);
            uint32_t res;
            switch (f5) {
                case 0x00: res = t + u2;                                  break; // AMOADD
                case 0x01: res = u2;                                      break; // AMOSWAP
                case 0x04: res = t ^ u2;                                  break; // AMOXOR
                case 0x08: res = t | u2;                                  break; // AMOOR
                case 0x0C: res = t & u2;                                  break; // AMOAND
                case 0x10: res = ((int32_t)t < (int32_t)u2) ? t : u2;     break; // AMOMIN
                case 0x14: res = ((int32_t)t > (int32_t)u2) ? t : u2;     break; // AMOMAX
                case 0x18: res = (t < u2) ? t : u2;                       break; // AMOMINU
                case 0x1C: res = (t > u2) ? t : u2;                       break; // AMOMAXU
                default:   return { EXC_ILLEGAL, instr };
            }
            mem_write<uint32_t>(cpu, mm, addr, res);
            cpu.regs[rd] = t;
        }
        break;
    }
    case 0x0F: break;                                                            // FENCE → NOP
    case 0x73: return { EXC_SYSTEM,  instr };
    default:   return { EXC_ILLEGAL, instr };
    }

    cpu.regs[0] = 0;
    cpu.pc = nextpc;
    return { EXC_NONE, 0 };
}

// One full interpreter step (no prefetch window) including interrupt check,
// trap-return gateway and trap dispatch — identical to rv32i_cuda.cu's
// do_step<false>. The JIT's interp: fallback calls this in a loop.
static __device__ __forceinline__ void jit_interp_step(Hart& cpu, CoreMem& mm) {
    if (check_interrupts(cpu, mm)) return;
    if (cpu.pc == PV_RESUME_GATEWAY) { trap_return(cpu, mm); return; }
    CpuException e = cpu_step(cpu, mm, mem_read<uint32_t>(cpu, mm, cpu.pc));
    if (e.kind == EXC_SYSTEM)       trap_system(cpu, mm, e.instr);
    else if (e.kind == EXC_ILLEGAL) do_trap(cpu, mm, CAUSE_ILLEGAL, e.instr);
}

#endif  // RVJIT_MEM_ONLY — end of cold trap/interpreter path

// ════════════════════════════════════════════════════════════════════
// JIT load/store/divide helpers — names/signatures match Core/Cuda/RvJit.cs.
//
// The fast path is a single bounds compare straight into RAM (constant-folded
// by ptxas). The slow path routes through the REAL mem_read/mem_write above so
// the JIT and interpreter share IDENTICAL semantics for the shared RO code
// image, framebuffer (@0x20000000), PCM (@0x30000000), trap page (@0x0F000000),
// every MMIO device, and the exit device (@0x40000000). This is the crux that
// makes Doom work under the JIT: framebuffer writes and the halt-on-exit write
// both fall into mem_write here.
//
// NOTE: the RAM fast path deliberately does NOT short-circuit the shared RO
// code range. mem_read consults [code_lo, code_hi) first, but RAM holds an
// identical copy of those bytes (see CoreMem::code doc), so reading RAM for a
// data load in that range returns the same value — and code is read-only, so
// a JIT'd load never needs the shared image's coalescing for correctness.
// ════════════════════════════════════════════════════════════════════

static __device__ __forceinline__ uint32_t jit_l8s (Hart&h,CoreMem&m,uint32_t a){ return (uint32_t)(int8_t) (a<m.ram_size?m.ram[a]:mem_read<uint8_t>(h,m,a)); }
static __device__ __forceinline__ uint32_t jit_l8u (Hart&h,CoreMem&m,uint32_t a){ return (uint32_t)         (a<m.ram_size?m.ram[a]:mem_read<uint8_t>(h,m,a)); }
static __device__ __forceinline__ uint32_t jit_l16s(Hart&h,CoreMem&m,uint32_t a){ return (uint32_t)(int16_t)(a+2<=m.ram_size?ld_le<uint16_t>(m.ram,a):mem_read<uint16_t>(h,m,a)); }
static __device__ __forceinline__ uint32_t jit_l16u(Hart&h,CoreMem&m,uint32_t a){ return (uint32_t)         (a+2<=m.ram_size?ld_le<uint16_t>(m.ram,a):mem_read<uint16_t>(h,m,a)); }
static __device__ __forceinline__ uint32_t jit_l32 (Hart&h,CoreMem&m,uint32_t a){ return                    a+4<=m.ram_size?ld_le<uint32_t>(m.ram,a):mem_read<uint32_t>(h,m,a); }
static __device__ __forceinline__ void jit_s8 (Hart&h,CoreMem&m,uint32_t a,uint32_t v){ if(a<m.ram_size) m.ram[a]=(uint8_t)v; else mem_write<uint8_t>(h,m,a,(uint8_t)v); }
static __device__ __forceinline__ void jit_s16(Hart&h,CoreMem&m,uint32_t a,uint32_t v){ if(a+2<=m.ram_size) st_le<uint16_t>(m.ram,a,(uint16_t)v); else mem_write<uint16_t>(h,m,a,(uint16_t)v); }
static __device__ __forceinline__ void jit_s32(Hart&h,CoreMem&m,uint32_t a,uint32_t v){ if(a+4<=m.ram_size) st_le<uint32_t>(m.ram,a,v); else mem_write<uint32_t>(h,m,a,v); }
static __device__ __forceinline__ uint32_t jit_div (int32_t a,int32_t b){ return b==0?0xFFFFFFFFu:(a==(int32_t)0x80000000&&b==-1)?0x80000000u:(uint32_t)(a/b); }
static __device__ __forceinline__ uint32_t jit_divu(uint32_t a,uint32_t b){ return b==0?0xFFFFFFFFu:a/b; }
static __device__ __forceinline__ uint32_t jit_rem (int32_t a,int32_t b){ return b==0?(uint32_t)a:(a==(int32_t)0x80000000&&b==-1)?0u:(uint32_t)(a%b); }
static __device__ __forceinline__ uint32_t jit_remu(uint32_t a,uint32_t b){ return b==0?a:a%b; }
