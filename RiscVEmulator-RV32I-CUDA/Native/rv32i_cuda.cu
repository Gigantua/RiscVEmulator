#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <cuda_runtime.h>

static constexpr uint32_t CLINT_BASE  = 0x02000000u, CLINT_SIZE  = 0x10000u;
static constexpr uint32_t TRAP_BASE   = 0x0F000000u, TRAP_SIZE   = 0x1000u;
static constexpr uint32_t UART_BASE   = 0x10000000u, UART_SIZE   = 0x100u;
static constexpr uint32_t KBD_BASE    = 0x10001000u, KBD_SIZE    = 0x100u;
static constexpr uint32_t MOUSE_BASE  = 0x10002000u, MOUSE_SIZE  = 0x100u;
static constexpr uint32_t RTC_BASE    = 0x10003000u, RTC_SIZE    = 0x100u;
static constexpr uint32_t MIDI_BASE   = 0x10005000u, MIDI_SIZE   = 0x100u;
static constexpr uint32_t FB_BASE     = 0x20000000u;
static constexpr uint32_t DISP_BASE   = 0x20100000u, DISP_SIZE   = 0x100u;
static constexpr uint32_t PCM_BASE    = 0x30000000u;
static constexpr uint32_t AUDIO_BASE  = 0x30100000u, AUDIO_SIZE  = 0x100u;
static constexpr uint32_t EXIT_BASE   = 0x40000000u, EXIT_SIZE   = 0x10u;

static constexpr uint32_t TXR = 1u << 16, TXM = TXR - 1u;
static constexpr uint32_t RXR = 1u << 12, RXM = RXR - 1u;
static constexpr uint32_t KBR = 1u << 10, KBM = KBR - 1u;
static constexpr uint32_t MDR = 1u << 12, MDM = MDR - 1u;

static constexpr uint32_t PRIV_U = 0u, PRIV_M = 3u;
static constexpr uint32_t PIN_MTIP = 1u << 7;
static constexpr uint32_t STATUS_IE = 1u << 3, STATUS_PIE = 1u << 7, STATUS_PP = 3u << 11;
static constexpr uint32_t CAUSE_ILLEGAL = 2u, CAUSE_EBREAK = 3u, CAUSE_ECALL_U = 8u, CAUSE_ECALL_M = 11u;
static constexpr uint32_t CAUSE_IRQ_MTIP = 0x80000007u;

static constexpr uint32_t IE_FLAG      = TRAP_BASE + 0x000u;
static constexpr uint32_t TRAP_VECTOR  = TRAP_BASE + 0x004u;
static constexpr uint32_t IE_MASK      = TRAP_BASE + 0x008u;
static constexpr uint32_t TRAP_SCRATCH = TRAP_BASE + 0x00Cu;
static constexpr uint32_t FRAME_BASE   = TRAP_BASE + 0x100u;
static constexpr uint32_t FRAME_STATUS = FRAME_BASE + 32u * 4u;
static constexpr uint32_t FRAME_TVAL   = FRAME_BASE + 33u * 4u;
static constexpr uint32_t FRAME_CAUSE  = FRAME_BASE + 34u * 4u;
static constexpr uint32_t PV_RESUME_GATEWAY = 0xFFFF0004u;

static constexpr int IRQ_CHECK = 64;

struct Periph {
    uint32_t tx_head, tx_tail, rx_head, rx_tail;
    uint8_t  ier, lcr, mcr, scr;
    uint32_t kbd_head, kbd_tail, kbd_mod;
    int32_t  mouse_dx, mouse_dy;
    uint32_t mouse_buttons, mouse_has;
    uint32_t midi_head, midi_tail;
    uint32_t au_ctrl, au_rate, au_chan, au_bits, au_bufstart, au_buflen, au_pos, au_wrgen;
    uint32_t dc_vsync, dc_mode, dc_fbaddr, dc_palidx, dc_pal[256];
    uint32_t rtc_us_lo, rtc_us_hi, rtc_ms_lo, rtc_ms_hi, rtc_epoch_lo, rtc_epoch_hi, rtc_sec, rtc_subus;
    uint32_t mtime_lo, mtime_hi, mtimecmp_lo, mtimecmp_hi;
    uint8_t  tx[TXR], rx[RXR];
    uint32_t kbd[KBR], midi[MDR];
};

struct CoreState { uint32_t regs[32], pc, priv; };
struct CoreMem {
    uint8_t* ram; uint8_t* fb; uint8_t* pcm; uint8_t* trap; Periph* per;
    uint32_t ram_size, fb_bytes, pcm_bytes, fb_w, fb_h;
    // Shared read-only image (throughput unit). One device copy of the guest's
    // RO span [ro_lo, ro_hi) (.text + rodata) backs ALL cores. Fetches and reads
    // in that range hit this one cache-resident buffer instead of each core's own
    // RAM copy, so N cores share a single instruction-fetch working set. ro == 0
    // disables sharing — the single-core path then routes everything to per-core
    // RAM exactly as before (bit-identical).
    const uint8_t* ro; uint32_t ro_lo, ro_hi;
};
struct Hart { uint32_t* regs; uint32_t pc, priv; };

template<class T> static __device__ __forceinline__ T ld_le(const uint8_t* p, uint32_t a) {
    if constexpr (sizeof(T) == 1) return (T)p[a];
    else if constexpr (sizeof(T) == 2) {
        if ((a & 1u) == 0) return *(const T*)(p + a);
        return (T)((uint16_t)p[a] | ((uint16_t)p[a+1] << 8));
    } else {
        if ((a & 3u) == 0) return *(const T*)(p + a);
        return (T)((uint32_t)p[a] | ((uint32_t)p[a+1] << 8) | ((uint32_t)p[a+2] << 16) | ((uint32_t)p[a+3] << 24));
    }
}
template<class T> static __device__ __forceinline__ void st_le(uint8_t* p, uint32_t a, T v) {
    if constexpr (sizeof(T) == 1) p[a] = (uint8_t)v;
    else if constexpr (sizeof(T) == 2) {
        if ((a & 1u) == 0) { *(T*)(p + a) = v; return; }
        p[a] = (uint8_t)v; p[a+1] = (uint8_t)(v >> 8);
    } else {
        if ((a & 3u) == 0) { *(T*)(p + a) = v; return; }
        p[a] = (uint8_t)v; p[a+1] = (uint8_t)(v >> 8); p[a+2] = (uint8_t)(v >> 16); p[a+3] = (uint8_t)(v >> 24);
    }
}

static __device__ uint32_t mmio_read(CoreMem& m, uint32_t a) {
    Periph* p = m.per;
    if (a - UART_BASE < UART_SIZE) switch (a - UART_BASE) {
        case 0x00: if (p->rx_tail != p->rx_head) return p->rx[p->rx_tail++ & RXM]; return 0;
        case 0x01: return p->ier;  case 0x02: return 0xC0u;  case 0x03: return p->lcr;  case 0x04: return p->mcr;
        case 0x05: return (p->rx_tail != p->rx_head ? 1u : 0u) | 0x60u;  case 0x06: return 0x30u;  case 0x07: return p->scr;
    }
    else if (a - KBD_BASE < KBD_SIZE) switch (a - KBD_BASE) {
        case 0x00: return p->kbd_tail != p->kbd_head ? 1u : 0u;
        case 0x04: if (p->kbd_tail != p->kbd_head) return p->kbd[p->kbd_tail++ & KBM]; return 0;
        case 0x08: return p->kbd_mod;
    }
    else if (a - MOUSE_BASE < MOUSE_SIZE) switch (a - MOUSE_BASE) {
        case 0x00: return p->mouse_has;
        case 0x04: { uint32_t v = (uint32_t)p->mouse_dx; p->mouse_dx = 0;
                     p->mouse_has = (p->mouse_dx || p->mouse_dy || p->mouse_buttons) ? 1u : 0u; return v; }
        case 0x08: { uint32_t v = (uint32_t)p->mouse_dy; p->mouse_dy = 0;
                     p->mouse_has = (p->mouse_dx || p->mouse_dy || p->mouse_buttons) ? 1u : 0u; return v; }
        case 0x0C: return p->mouse_buttons;
    }
    else if (a - RTC_BASE < RTC_SIZE) switch (a - RTC_BASE) {
        case 0x00: return p->rtc_us_lo;    case 0x04: return p->rtc_us_hi;
        case 0x08: return p->rtc_ms_lo;    case 0x0C: return p->rtc_ms_hi;
        case 0x10: return p->rtc_epoch_lo; case 0x14: return p->rtc_epoch_hi;
        case 0x18: return p->rtc_sec;      case 0x1C: return p->rtc_subus;
    }
    else if (a - MIDI_BASE < MIDI_SIZE) { return (a - MIDI_BASE) == 0 ? 1u : 0u; }
    else if (a - DISP_BASE < DISP_SIZE) switch (a - DISP_BASE) {
        case 0x00: return m.fb_w;  case 0x04: return m.fb_h;  case 0x08: return 32u;
        case 0x0C: return p->dc_vsync;  case 0x18: return p->dc_mode;  case 0x1C: return p->dc_fbaddr;
    }
    else if (a - AUDIO_BASE < AUDIO_SIZE) switch (a - AUDIO_BASE) {
        case 0x00: return p->au_ctrl;  case 0x04: return (p->au_ctrl & 1u) ? 1u : 0u;  case 0x08: return p->au_rate;
        case 0x0C: return p->au_chan;  case 0x10: return p->au_bits;  case 0x14: return p->au_bufstart;
        case 0x18: return p->au_buflen;  case 0x1C: return p->au_pos;
    }
    else if (a - CLINT_BASE < CLINT_SIZE) switch (a - CLINT_BASE) {
        case 0x0BFF8: return p->mtime_lo;     case 0x0BFFC: return p->mtime_hi;
        case 0x04000: return p->mtimecmp_lo;  case 0x04004: return p->mtimecmp_hi;
    }
    return 0;
}

static __device__ void mmio_write(Hart& h, CoreMem& m, uint32_t a, uint32_t val) {
    Periph* p = m.per;
    if (a - UART_BASE < UART_SIZE) switch (a - UART_BASE) {
        case 0x00: p->tx[p->tx_head++ & TXM] = (uint8_t)val; break;  case 0x01: p->ier = (uint8_t)val; break;
        case 0x03: p->lcr = (uint8_t)val; break;  case 0x04: p->mcr = (uint8_t)val; break;  case 0x07: p->scr = (uint8_t)val; break;
    }
    else if (a - MIDI_BASE < MIDI_SIZE) {
        uint32_t off = a - MIDI_BASE;
        if (off == 0x04 || off == 0x08 || off == 0x0C) p->midi[p->midi_head++ & MDM] = (off << 24) | (val & 0x00FFFFFFu);
    }
    else if (a - DISP_BASE < DISP_SIZE) switch (a - DISP_BASE) {
        case 0x0C: p->dc_vsync = val; break;  case 0x10: p->dc_palidx = val & 0xFFu; break;
        case 0x14: p->dc_pal[p->dc_palidx & 0xFFu] = val; break;  case 0x18: p->dc_mode = val; break;  case 0x1C: p->dc_fbaddr = val; break;
    }
    else if (a - AUDIO_BASE < AUDIO_SIZE) switch (a - AUDIO_BASE) {
        case 0x00: if (val & 4u) { p->au_ctrl = 0; p->au_pos = 0; } else { if (val & 1u) p->au_wrgen++; p->au_ctrl = val & 3u; } break;
        case 0x08: p->au_rate = val; break;  case 0x0C: p->au_chan = val; break;  case 0x10: p->au_bits = val; break;
        case 0x14: p->au_bufstart = val; break;  case 0x18: p->au_buflen = val; break;
    }
    else if (a - CLINT_BASE < CLINT_SIZE) switch (a - CLINT_BASE) {
        case 0x04000: p->mtimecmp_lo = val; break;  case 0x04004: p->mtimecmp_hi = val; break;
    }
    else if (a - EXIT_BASE < EXIT_SIZE) { if ((a - EXIT_BASE) == 0) h.pc = 0x80000000u | (val & 0x7FFFFFFFu); }
}

template<class T> static __device__ __forceinline__ T mem_read(CoreMem& m, uint32_t a) {
    // Reads fully inside the shared RO span come from the one shared image (all
    // cores alias it). The RO span is a sub-range of [0, ram_size); writes to it
    // never happen in correct guest code, so the per-core RAM copy of that range
    // is never observed. The whole access [a, a+sizeof(T)) must fit so a
    // misaligned load straddling ro_hi doesn't read past the (hi-lo)-byte buffer
    // — such a straddling read falls through to per-core RAM, which holds the
    // identical bytes. With ro == 0 (single core) span==0, so this branch is dead.
    uint32_t roff = a - m.ro_lo, rspan = m.ro_hi - m.ro_lo;
    if (roff < rspan && roff + sizeof(T) <= rspan) return ld_le<T>(m.ro, roff);
    if (a < m.ram_size)             return ld_le<T>(m.ram, a);
    if (a - FB_BASE  < m.fb_bytes)  return ld_le<T>(m.fb,  a - FB_BASE);
    if (a - PCM_BASE < m.pcm_bytes) return ld_le<T>(m.pcm, a - PCM_BASE);
    if (a - TRAP_BASE < TRAP_SIZE)  return ld_le<T>(m.trap, a - TRAP_BASE);
    return (T)mmio_read(m, a);
}
template<class T> static __device__ __forceinline__ void mem_write(Hart& h, CoreMem& m, uint32_t a, T v) {
    if (a < m.ram_size)             { st_le<T>(m.ram, a, v); return; }
    if (a - FB_BASE  < m.fb_bytes)  { st_le<T>(m.fb,  a - FB_BASE,  v); return; }
    if (a - PCM_BASE < m.pcm_bytes) { st_le<T>(m.pcm, a - PCM_BASE, v); return; }
    if (a - TRAP_BASE < TRAP_SIZE)  { st_le<T>(m.trap, a - TRAP_BASE, v); return; }
    mmio_write(h, m, a, (uint32_t)v);
}

// Instruction fetch: pc is always 4-aligned and (for normal code) in RAM. Read
// through the non-coherent read-only cache (ld.global.nc / __ldg) — the texture
// path has higher bandwidth and the read-only cache hides the fetch latency that
// otherwise dominates the step. Safe because guest .text is never written
// (non-self-modifying code). Edge / non-RAM fetches fall back to the full path.
static __device__ __forceinline__ uint32_t fetch(CoreMem& m, uint32_t pc) {
    // Shared RO image first: all cores' fetches converge on one cache-resident
    // copy of .text, which is the whole point of the throughput model. The span
    // is 4-aligned and pc is always 4-aligned, so pc+4 <= ro_hi is the in-range
    // test. With ro == 0 the subtraction underflows to a huge value and this is
    // skipped (single-core falls through to per-core RAM, bit-identical).
    if (pc - m.ro_lo < m.ro_hi - m.ro_lo)
        return __ldg(reinterpret_cast<const uint32_t*>(m.ro + (pc - m.ro_lo)));
    if (pc + 4u <= m.ram_size) return __ldg(reinterpret_cast<const uint32_t*>(m.ram + pc));
    return mem_read<uint32_t>(m, pc);
}

static __device__ void do_trap(Hart& h, CoreMem& mm, uint32_t cause, uint32_t tval) {
    uint32_t tp = h.regs[4];
    h.regs[4] = mem_read<uint32_t>(mm, TRAP_SCRATCH);
    mem_write<uint32_t>(h, mm, TRAP_SCRATCH, tp);
    mem_write<uint32_t>(h, mm, FRAME_BASE, h.pc);
    for (uint32_t i = 1; i < 32; i++) mem_write<uint32_t>(h, mm, FRAME_BASE + i * 4u, h.regs[i]);
    uint32_t pie = (mem_read<uint32_t>(mm, IE_FLAG) & STATUS_IE) ? STATUS_PIE : 0u;
    uint32_t pp  = (h.priv == PRIV_M) ? STATUS_PP : 0u;
    mem_write<uint32_t>(h, mm, FRAME_STATUS, pie | pp);
    mem_write<uint32_t>(h, mm, FRAME_TVAL,  tval);
    mem_write<uint32_t>(h, mm, FRAME_CAUSE, cause);
    mem_write<uint32_t>(h, mm, IE_FLAG, 0u);
    h.priv = PRIV_M;
    h.pc = mem_read<uint32_t>(mm, TRAP_VECTOR);
}

static __device__ void trap_return(Hart& h, CoreMem& mm) {
    uint32_t fb = h.regs[10];
    uint32_t status = mem_read<uint32_t>(mm, fb + FRAME_STATUS - FRAME_BASE);
    for (uint32_t i = 1; i < 32; i++) h.regs[i] = mem_read<uint32_t>(mm, fb + i * 4u);
    mem_write<uint32_t>(h, mm, IE_FLAG, (status & STATUS_PIE) ? STATUS_IE : 0u);
    h.priv = (status & STATUS_PP) ? PRIV_M : PRIV_U;
    h.pc = mem_read<uint32_t>(mm, fb);
}

static __device__ void trap_system(Hart& h, CoreMem& mm, uint32_t instr) {
    uint32_t f3 = (instr >> 12) & 0x7, fn = (instr >> 20) & 0xFFF;
    if (f3 == 0 && fn == 0x000) { do_trap(h, mm, h.priv == PRIV_M ? CAUSE_ECALL_M : CAUSE_ECALL_U, 0); return; }
    if (f3 == 0 && fn == 0x001) { do_trap(h, mm, CAUSE_EBREAK, h.pc); return; }
    do_trap(h, mm, CAUSE_ILLEGAL, instr);
}

// Branchless OP / OP-IMM ALU: compute every arm, index-select the live one. On a
// single warp this is FASTER than a switch (profiled): the 8 independent arms
// give the scheduler eligible instructions to issue while dependent results bake,
// hiding the fixed-latency execution-dependency stall that dominates one warp.
// Bit-identical to the switch arms. sub picks SUB over ADD (selected only when
// f3==0); sra picks SRA over SRL.
static __device__ __forceinline__ uint32_t alu(uint32_t f3, uint32_t u1, int32_t s1,
        uint32_t a2u, int32_t a2s, uint32_t sh, bool sub, bool sra) {
    uint32_t add  = sub ? (u1 - a2u) : (u1 + a2u);
    uint32_t sll  = u1 << sh;
    uint32_t slt  = (uint32_t)(s1 < a2s);
    uint32_t sltu = (uint32_t)(u1 < a2u);
    uint32_t xr   = u1 ^ a2u;
    uint32_t sr   = sra ? (uint32_t)(s1 >> sh) : (u1 >> sh);
    uint32_t orr  = u1 | a2u;
    uint32_t andr = u1 & a2u;
    uint32_t e0 = (f3 & 4u) ? xr   : add;
    uint32_t e1 = (f3 & 4u) ? sr   : sll;
    uint32_t e2 = (f3 & 4u) ? orr  : slt;
    uint32_t e3 = (f3 & 4u) ? andr : sltu;
    return (f3 & 1u) ? ((f3 & 2u) ? e3 : e1) : ((f3 & 2u) ? e2 : e0);
}

static __device__ bool check_interrupts(Hart& h, CoreMem& mm) {
    if (!(mem_read<uint32_t>(mm, IE_FLAG) & STATUS_IE)) return false;
    if (!(mem_read<uint32_t>(mm, IE_MASK) & PIN_MTIP)) return false;
    uint64_t mt  = ((uint64_t)mm.per->mtime_hi << 32) | mm.per->mtime_lo;
    uint64_t cmp = ((uint64_t)mm.per->mtimecmp_hi << 32) | mm.per->mtimecmp_lo;
    if (mt < cmp) return false;
    do_trap(h, mm, CAUSE_IRQ_MTIP, 0);
    return true;
}

static __device__ void do_step(Hart& cpu, CoreMem& mm) {
    const uint32_t instr = fetch(mm, cpu.pc);
    const int      rd = (instr >> 7) & 0x1F;
    const uint32_t f3 = (instr >> 12) & 0x7, f7 = (instr >> 25) & 0x7F;
    const uint32_t u1 = cpu.regs[(instr >> 15) & 0x1F], u2 = cpu.regs[(instr >> 20) & 0x1F];
    const int32_t  s1 = (int32_t)u1, s2 = (int32_t)u2;
    const int32_t  iimm = (int32_t)instr >> 20;
    const int32_t  simm = ((int32_t)(instr & 0xFE000000) >> 20) | (int32_t)((instr >> 7) & 0x1F);
    const int      sh = (instr >> 20) & 0x1F;
    uint32_t nextpc = cpu.pc + 4, r = 0;

    switch (instr & 0x7F) {
    case 0x37: r = instr & 0xFFFFF000u; break;
    case 0x17: r = cpu.pc + (instr & 0xFFFFF000u); break;
    case 0x6F: r = cpu.pc + 4;
        nextpc = cpu.pc + ((((instr>>31)&1u)<<20 | ((instr>>12)&0xFFu)<<12 | ((instr>>20)&1u)<<11 | ((instr>>21)&0x3FFu)<<1)
                          | ((instr & 0x80000000u) ? 0xFFE00000u : 0u)); break;
    case 0x67: r = cpu.pc + 4; nextpc = (uint32_t)(s1 + iimm) & ~1u; break;
    case 0x63: {
        uint32_t bimm = (((instr>>31)&1u)<<12 | ((instr>>7)&1u)<<11 | ((instr>>25)&0x3Fu)<<5 | ((instr>>8)&0xFu)<<1)
                      | ((instr & 0x80000000u) ? 0xFFFFE000u : 0u);
        int taken = 0;
        switch (f3) { case 0: taken = u1==u2; break; case 1: taken = u1!=u2; break;
                      case 4: taken = s1<s2; break;  case 5: taken = s1>=s2; break;
                      case 6: taken = u1<u2; break;  case 7: taken = u1>=u2; break; }
        if (taken) nextpc = cpu.pc + bimm;
        cpu.pc = nextpc; return;
    }
    case 0x03: {
        uint32_t addr = (uint32_t)(s1 + iimm);
        switch (f3) {
            case 0: r = (uint32_t)(int8_t) mem_read<uint8_t> (mm, addr); break;
            case 1: r = (uint32_t)(int16_t)mem_read<uint16_t>(mm, addr); break;
            case 2: r =                    mem_read<uint32_t>(mm, addr); break;
            case 4: r =                    mem_read<uint8_t> (mm, addr); break;
            case 5: r =                    mem_read<uint16_t>(mm, addr); break;
        }
        break;
    }
    case 0x23: {
        uint32_t addr = (uint32_t)(s1 + simm);
        switch (f3) { case 0: mem_write<uint8_t>(cpu,mm,addr,(uint8_t)u2); break;
                      case 1: mem_write<uint16_t>(cpu,mm,addr,(uint16_t)u2); break;
                      case 2: mem_write<uint32_t>(cpu,mm,addr,u2); break; }
        if ((int32_t)cpu.pc >= 0) cpu.pc = nextpc; return;
    }
    case 0x13:
        r = alu(f3, u1, s1, (uint32_t)iimm, iimm, (uint32_t)sh, false, f7 == 0x20);
        break;
    case 0x33:
        if (f7 == 0x01) switch (f3) {
            case 0: r = (uint32_t)(u1 * u2); break;
            case 1: r = (uint32_t)(((int64_t)s1 * (int64_t)s2) >> 32); break;
            case 2: r = (uint32_t)(((int64_t)s1 * (int64_t)(uint64_t)u2) >> 32); break;
            case 3: r = (uint32_t)(((uint64_t)u1 * (uint64_t)u2) >> 32); break;
            case 4: r = (s2==0) ? 0xFFFFFFFFu : (s1==(int32_t)0x80000000 && s2==-1) ? 0x80000000u : (uint32_t)(s1/s2); break;
            case 5: r = (u2==0) ? 0xFFFFFFFFu : (u1/u2); break;
            case 6: r = (s2==0) ? u1 : (s1==(int32_t)0x80000000 && s2==-1) ? 0u : (uint32_t)(s1%s2); break;
            case 7: r = (u2==0) ? u1 : (u1%u2); break;
        } else {
            r = alu(f3, u1, s1, u2, s2, (uint32_t)(s2 & 0x1F), f7 == 0x20, f7 == 0x20);
        }
        break;
    case 0x2F: {
        uint32_t addr = u1, f5 = (instr >> 27) & 0x1F, t = mem_read<uint32_t>(mm, addr), w = t;
        switch (f5) {
            case 0x02: r = t; if (rd) cpu.regs[rd] = r; cpu.regs[0] = 0; cpu.pc = nextpc; return;
            case 0x03: mem_write<uint32_t>(cpu,mm,addr,u2); if (rd) cpu.regs[rd] = 0; cpu.regs[0] = 0; cpu.pc = nextpc; return;
            case 0x00: w = t + u2; break;  case 0x01: w = u2; break;  case 0x04: w = t ^ u2; break;
            case 0x08: w = t | u2; break;  case 0x0C: w = t & u2; break;
            case 0x10: w = ((int32_t)t < (int32_t)u2) ? t : u2; break;  case 0x14: w = ((int32_t)t > (int32_t)u2) ? t : u2; break;
            case 0x18: w = (t < u2) ? t : u2; break;  case 0x1C: w = (t > u2) ? t : u2; break;
        }
        mem_write<uint32_t>(cpu, mm, addr, w); r = t; break;
    }
    case 0x0F: cpu.pc = nextpc; return;
    case 0x73: trap_system(cpu, mm, instr); return;
    default:   do_trap(cpu, mm, CAUSE_ILLEGAL, instr); return;
    }

    if (rd) cpu.regs[rd] = r;
    if ((int32_t)cpu.pc >= 0) cpu.pc = nextpc;
}

__global__ void __launch_bounds__(256)
rv32i_kernel(CoreState* st, CoreMem* mm, int ncores, int budget) {
    int id = blockIdx.x * blockDim.x + threadIdx.x;
    if (id >= ncores) return;
    CoreState& g = st[id];
    CoreMem    m = mm[id];
    extern __shared__ uint32_t s_regs[];
    Hart h; h.regs = &s_regs[threadIdx.x * 33];   // stride 33: per-lane regfile, bank-conflict-free
    #pragma unroll
    for (int i = 0; i < 32; i++) h.regs[i] = g.regs[i];
    h.pc = g.pc; h.priv = g.priv;
    for (int i = 0; i < budget && (int32_t)h.pc >= 0; i++) {
        if ((i & (IRQ_CHECK - 1)) == 0 && check_interrupts(h, m)) continue;
        if (h.pc == PV_RESUME_GATEWAY) { trap_return(h, m); continue; }
        do_step(h, m);
    }
    #pragma unroll
    for (int i = 0; i < 32; i++) g.regs[i] = h.regs[i];
    g.pc = h.pc; g.priv = h.priv;
}

// ── ISA profiler: dynamic opcode histogram + adjacent-pair matrix + exact
//    fusion-pattern hit counts, for one guest. idx = (instr>>2)&0x1F (32 buckets).
//    prof[prev*32+cur] = dynamic pair count; prof[1024]=LUI+ADDI(same rd) fusions,
//    [1025]=AUIPC+ADDI(same), [1026]=AUIPC+LW(same), [1027]=total steps. ──
static constexpr int PROFN = 1024 + 8;

// jump[o*64 + ...]: per control-op o (0=BRANCH taken, 1=JAL, 2=JALR), bucket b =
// 31-clz(|target-pc| bytes): [0..24]=forward, [25..49]=backward, [50]=count,
// [51]=sum|dist| bytes (for the average). Sequential (not-taken) flow excluded.
__global__ void rv32i_profile_kernel(CoreState* st, CoreMem* mm,
                                     unsigned long long* prof, unsigned long long* jump, int budget) {
    CoreState& g = st[0];
    CoreMem    m = mm[0];
    extern __shared__ uint32_t s_regs[];
    Hart h; h.regs = s_regs;
    #pragma unroll
    for (int i = 0; i < 32; i++) h.regs[i] = g.regs[i];
    h.pc = g.pc; h.priv = g.priv;
    uint32_t previnstr = 0; int previdx = 32;
    for (int i = 0; i < budget && (int32_t)h.pc >= 0; i++) {
        if ((i & (IRQ_CHECK - 1)) == 0 && check_interrupts(h, m)) { previdx = 32; continue; }
        if (h.pc == PV_RESUME_GATEWAY) { trap_return(h, m); previdx = 32; continue; }
        uint32_t instr = fetch(m, h.pc);
        uint32_t idx = (instr >> 2) & 0x1F;
        if (previdx < 32) prof[previdx * 32 + idx]++;
        uint32_t op = instr & 0x7F, pop = previnstr & 0x7F;
        uint32_t prd = (previnstr >> 7) & 0x1F, crs1 = (instr >> 15) & 0x1F, crd = (instr >> 7) & 0x1F, cf3 = (instr >> 12) & 7;
        if ((pop == 0x37 || pop == 0x17) && op == 0x13 && cf3 == 0 && crs1 == prd && crd == prd && crd != 0)
            prof[1024 + (pop == 0x17 ? 1u : 0u)]++;
        if (pop == 0x17 && op == 0x03 && crs1 == prd && crd == prd && crd != 0) prof[1026]++;
        prof[1027]++;
        uint32_t old_pc = h.pc;
        do_step(h, m);
        if ((op == 0x63 || op == 0x6F || op == 0x67) && (int32_t)h.pc >= 0 && h.pc != old_pc + 4u) {
            int o = (op == 0x63) ? 0 : (op == 0x6F) ? 1 : 2;
            int32_t  dist = (int32_t)(h.pc - old_pc);
            uint32_t mag  = dist < 0 ? (uint32_t)(-dist) : (uint32_t)dist;
            int b = mag ? 31 - __clz((int)mag) : 0; if (b > 24) b = 24;
            jump[o * 64 + (dist < 0 ? 25 : 0) + b]++;
            jump[o * 64 + 50]++;
            jump[o * 64 + 51] += mag;
        }
        previnstr = instr; previdx = (int)idx;
    }
    #pragma unroll
    for (int i = 0; i < 32; i++) g.regs[i] = h.regs[i];
    g.pc = h.pc; g.priv = h.priv;
}

// ── Warp chunk executor (single guest, 32 lanes, regfile in shared). Each step
//    the 32 lanes speculatively decode+execute pc..pc+124 in parallel
//    (convergent → warp fully active). The independent ALU bundle length L for
//    each PC is PRECOMPUTED on the host (static schedule, sched[]) — so the warp
//    just reads L = sched[pc] instead of computing it at runtime with ballot/
//    match/shfl (which was the coordination overhead that made the naive version
//    2x slower). Retire the bundle [0,L) in one shot; the boundary instruction
//    (load/store/branch/JALR/SYSTEM/M/A) runs inline on lane L. ──
__global__ void __launch_bounds__(32)
rv32i_warp_kernel(CoreState* st, CoreMem* mm, const uint8_t* sched, uint32_t schedLo, uint32_t schedWords,
                  int ncores, int budget) {
    int gid = blockIdx.x;
    if (gid >= ncores) return;
    int lane = threadIdx.x;
    const unsigned FULL = 0xFFFFFFFFu;
    CoreState& g = st[gid];
    CoreMem    m = mm[gid];
    extern __shared__ uint32_t regs[];          // [32], shared by the warp
    regs[lane] = g.regs[lane];
    __syncwarp();
    uint32_t pc = g.pc, priv = g.priv;

    for (int i = 0; i < budget && (int32_t)pc >= 0; ) {
        int trapped = 0;
        if (lane == 0) {
            Hart h; h.regs = regs; h.pc = pc; h.priv = priv;
            if (check_interrupts(h, m)) trapped = 1;
            else if (pc == PV_RESUME_GATEWAY) { trap_return(h, m); trapped = 1; }
            pc = h.pc; priv = h.priv;
        }
        trapped = __shfl_sync(FULL, trapped, 0);
        pc      = __shfl_sync(FULL, pc, 0);
        priv    = __shfl_sync(FULL, priv, 0);
        if (trapped) { i++; continue; }

        // Precomputed bundle length for this PC (independent ALU prefix).
        uint32_t soff = (pc - schedLo) >> 2;
        int L = soff < schedWords ? (int)__ldg(sched + soff) : 0;

        // Parallel decode + ALU of the bundle (lanes 0..L-1) + the boundary lane.
        uint32_t ipc   = pc + (uint32_t)(lane * 4);
        uint32_t instr = (ipc + 4u <= m.ram_size) ? __ldg((const uint32_t*)(m.ram + ipc)) : 0u;
        uint32_t op = instr & 0x7F, rd = (instr >> 7) & 0x1F;
        uint32_t rs1i = (instr >> 15) & 0x1F, rs2i = (instr >> 20) & 0x1F;
        uint32_t f3 = (instr >> 12) & 7, f7 = (instr >> 25) & 0x7F;
        uint32_t u1 = regs[rs1i], u2 = regs[rs2i];
        int32_t  s1 = (int32_t)u1, s2 = (int32_t)u2;
        int32_t  iimm = (int32_t)instr >> 20; uint32_t sh = (instr >> 20) & 0x1F;
        uint32_t res = 0;
        if      (op == 0x37) res = instr & 0xFFFFF000u;
        else if (op == 0x17) res = ipc + (instr & 0xFFFFF000u);
        else if (op == 0x13) res = alu(f3, u1, s1, (uint32_t)iimm, iimm, sh, false, f7 == 0x20);
        else if (op == 0x33) res = alu(f3, u1, s1, u2, s2, (uint32_t)(s2 & 0x1F), f7 == 0x20, f7 == 0x20);

        if (lane < L && rd != 0) regs[rd] = res;
        __syncwarp();
        pc += (uint32_t)(L * 4); i += L;

        // Boundary instruction (the first barrier, at lane L) executed inline by
        // lane L using its decoded fields + post-retire operands. Common cases
        // (branch/jal/jalr/load/store) avoid the full do_step machinery; rare ops
        // (SYSTEM/M/A/FENCE) fall back to do_step.
        if (L < 32 && i < budget && (int32_t)pc >= 0) {
            if (lane == L) {
                uint32_t a1 = regs[rs1i], a2 = regs[rs2i];   // operands AFTER the prefix retired
                uint32_t npc = pc + 4u;
                if (op == 0x63) {                            // BRANCH
                    int taken = 0;
                    switch (f3) { case 0: taken = a1==a2; break; case 1: taken = a1!=a2; break;
                                  case 4: taken = (int32_t)a1<(int32_t)a2; break; case 5: taken = (int32_t)a1>=(int32_t)a2; break;
                                  case 6: taken = a1<a2; break; case 7: taken = a1>=a2; break; }
                    uint32_t bimm = (((instr>>31)&1u)<<12 | ((instr>>7)&1u)<<11 | ((instr>>25)&0x3Fu)<<5 | ((instr>>8)&0xFu)<<1)
                                  | ((instr & 0x80000000u) ? 0xFFFFE000u : 0u);
                    if (taken) npc = pc + bimm;
                } else if (op == 0x6F) {                     // JAL
                    uint32_t jimm = (((instr>>31)&1u)<<20 | ((instr>>12)&0xFFu)<<12 | ((instr>>20)&1u)<<11 | ((instr>>21)&0x3FFu)<<1)
                                  | ((instr & 0x80000000u) ? 0xFFE00000u : 0u);
                    if (rd) regs[rd] = pc + 4u; npc = pc + jimm;
                } else if (op == 0x67) {                     // JALR
                    uint32_t t = (uint32_t)((int32_t)a1 + iimm) & ~1u; if (rd) regs[rd] = pc + 4u; npc = t;
                } else if (op == 0x03) {                     // LOAD
                    uint32_t addr = (uint32_t)((int32_t)a1 + iimm), v = 0;
                    switch (f3) { case 0: v = (uint32_t)(int8_t) mem_read<uint8_t> (m, addr); break;
                                  case 1: v = (uint32_t)(int16_t)mem_read<uint16_t>(m, addr); break;
                                  case 2: v =                    mem_read<uint32_t>(m, addr); break;
                                  case 4: v =                    mem_read<uint8_t> (m, addr); break;
                                  case 5: v =                    mem_read<uint16_t>(m, addr); break; }
                    if (rd) regs[rd] = v;
                } else if (op == 0x23) {                     // STORE
                    int32_t simm = ((int32_t)(instr & 0xFE000000) >> 20) | (int32_t)((instr >> 7) & 0x1F);
                    uint32_t addr = (uint32_t)((int32_t)a1 + simm);
                    Hart h; h.regs = regs; h.pc = pc; h.priv = priv;
                    switch (f3) { case 0: mem_write<uint8_t> (h, m, addr, (uint8_t) a2); break;
                                  case 1: mem_write<uint16_t>(h, m, addr, (uint16_t)a2); break;
                                  case 2: mem_write<uint32_t>(h, m, addr,           a2); break; }
                    npc = (int32_t)h.pc < 0 ? h.pc : pc + 4u;     // exit device may set pc<0
                } else {                                     // SYSTEM / M / A / FENCE
                    Hart h; h.regs = regs; h.pc = pc; h.priv = priv;
                    do_step(h, m); npc = h.pc; priv = h.priv;
                }
                regs[0] = 0; pc = npc;
            }
            __syncwarp();
            pc   = __shfl_sync(FULL, pc, L);
            priv = __shfl_sync(FULL, priv, L);
            i += 1;
        }
    }
    __syncwarp();
    g.regs[lane] = regs[lane];
    if (lane == 0) { g.pc = pc; g.priv = priv; }
}

#define API extern "C" __declspec(dllexport)

static constexpr int JUMPN = 3 * 64;
static CoreState* g_state  = nullptr;
static CoreMem*   g_mem    = nullptr;
static unsigned long long* g_prof = nullptr;
static unsigned long long* g_jump = nullptr;
static uint8_t*   g_sched = nullptr;        // precomputed bundle length per PC word
static uint32_t   g_sched_lo = 0, g_sched_words = 0;
static int        g_ncores = 0;
static uint8_t*   g_ro = nullptr;           // shared read-only image (one copy, all cores)

API int cuda_rv32i_init(int nCores, unsigned int ramSize, unsigned int fbW, unsigned int fbH, unsigned int pcmBytes) {
    g_ncores = nCores;
    cudaError_t e;
    if ((e = cudaMallocManaged(&g_state, (size_t)nCores * sizeof(CoreState))) != cudaSuccess) return (int)e;
    if ((e = cudaMallocManaged(&g_mem,   (size_t)nCores * sizeof(CoreMem)))   != cudaSuccess) return (int)e;
    memset(g_state, 0, (size_t)nCores * sizeof(CoreState));
    memset(g_mem,   0, (size_t)nCores * sizeof(CoreMem));
    if (cudaMallocManaged(&g_prof, PROFN * sizeof(unsigned long long)) == cudaSuccess)
        memset(g_prof, 0, PROFN * sizeof(unsigned long long));
    if (cudaMallocManaged(&g_jump, JUMPN * sizeof(unsigned long long)) == cudaSuccess)
        memset(g_jump, 0, JUMPN * sizeof(unsigned long long));

    unsigned int fbBytes = fbW * fbH * 4u;
    for (int i = 0; i < nCores; i++) {
        uint8_t* ram; uint8_t* fb; uint8_t* pcm; uint8_t* trap; Periph* per;
        if ((e = cudaMalloc(&ram,  ramSize))   != cudaSuccess) return (int)e;
        if ((e = cudaMalloc(&fb,   fbBytes))   != cudaSuccess) return (int)e;
        if ((e = cudaMalloc(&pcm,  pcmBytes))  != cudaSuccess) return (int)e;
        if ((e = cudaMallocManaged(&trap, TRAP_SIZE)) != cudaSuccess) return (int)e;
        if ((e = cudaMallocManaged(&per, sizeof(Periph))) != cudaSuccess) return (int)e;
        cudaMemset(ram, 0, ramSize); cudaMemset(fb, 0, fbBytes); cudaMemset(pcm, 0, pcmBytes);
        memset(trap, 0, TRAP_SIZE);
        memset(per, 0, sizeof(Periph));
        per->au_rate = 22050; per->au_chan = 1; per->au_bits = 16;
        g_mem[i] = { ram, fb, pcm, trap, per, ramSize, fbBytes, pcmBytes, fbW, fbH };
        g_state[i].priv = PRIV_M;
    }
    cudaDeviceSynchronize();
    return (int)cudaGetLastError();
}

API void cuda_rv32i_set_reg  (int core, int i, unsigned int v) { if (i) g_state[core].regs[i & 31] = v; }
API void cuda_rv32i_set_entry(int core, unsigned int pc)       { g_state[core].pc = pc; }
API unsigned int cuda_rv32i_get_pc   (int core) { return g_state[core].pc; }
API int  cuda_rv32i_is_halted(int core)         { return (int32_t)g_state[core].pc < 0 ? 1 : 0; }
API void cuda_rv32i_set_halted(int core, int v) { if (v) g_state[core].pc |= 0x80000000u; else g_state[core].pc &= 0x7FFFFFFFu; }
API int  cuda_rv32i_exitcode (int core)         { return (int)(g_state[core].pc & 0x7FFFFFFFu); }

API int cuda_rv32i_load_ram(int core, const void* src, unsigned int off, unsigned int len) {
    return (int)cudaMemcpy(g_mem[core].ram + off, src, len, cudaMemcpyHostToDevice);
}
API int cuda_rv32i_read_ram(int core, void* dst, unsigned int off, unsigned int len) {
    return (int)cudaMemcpy(dst, g_mem[core].ram + off, len, cudaMemcpyDeviceToHost);
}
API int cuda_rv32i_read_fb(int core, void* dst, unsigned int len) {
    return (int)cudaMemcpy(dst, g_mem[core].fb, len, cudaMemcpyDeviceToHost);
}
API int cuda_rv32i_read_pcm(int core, void* dst, unsigned int len) {
    return (int)cudaMemcpy(dst, g_mem[core].pcm, len, cudaMemcpyDeviceToHost);
}

API void cuda_rv32i_set_mtime(int core, unsigned int lo, unsigned int hi) {
    Periph* p = g_mem[core].per; p->mtime_lo = lo; p->mtime_hi = hi;
}

API int cuda_rv32i_step_all(int budget) {
    if (g_ncores <= 0) return 0;
    // Default: single-thread kernel (fastest on jumpy guests). RVEMU_WARP=1 opts
    // into the warp chunk executor (32 decoded/step, chains solved via ballot/
    // match/shfl) — correct, but ~2x slower on DOOM: the per-step warp-sync
    // coordination overhead exceeds the ~2-3 instructions of ILP it extracts.
    static int warp = -1;
    if (warp < 0) { const char* e = getenv("RVEMU_WARP"); warp = (e && e[0] == '1') ? 1 : 0; }
    if (warp) {
        rv32i_warp_kernel<<<g_ncores, 32, 32 * sizeof(uint32_t)>>>(
            g_state, g_mem, g_sched, g_sched_lo, g_sched_words, g_ncores, budget);
    } else {
        int block = g_ncores < 256 ? g_ncores : 256;
        int grid  = (g_ncores + block - 1) / block;
        size_t shmem = (size_t)block * 33 * sizeof(uint32_t);
        rv32i_kernel<<<grid, block, shmem>>>(g_state, g_mem, g_ncores, budget);
    }
    cudaError_t le = cudaGetLastError(), se = cudaDeviceSynchronize();
    return le != cudaSuccess ? (int)le : (int)se;
}

API int cuda_rv32i_set_schedule(const void* data, unsigned int lo, unsigned int words) {
    if (g_sched) { cudaFree(g_sched); g_sched = nullptr; g_sched_words = 0; }
    if (words == 0) return 0;
    cudaError_t e;
    if ((e = cudaMalloc(&g_sched, words)) != cudaSuccess) return (int)e;
    cudaMemcpy(g_sched, data, words, cudaMemcpyHostToDevice);
    g_sched_lo = lo; g_sched_words = words;
    return (int)cudaGetLastError();
}

API void  cuda_rv32i_prof_reset() {
    if (g_prof) memset(g_prof, 0, PROFN * sizeof(unsigned long long));
    if (g_jump) memset(g_jump, 0, JUMPN * sizeof(unsigned long long));
}
API void* cuda_rv32i_prof_ptr()   { return g_prof; }
API void* cuda_rv32i_jump_ptr()   { return g_jump; }
API int   cuda_rv32i_profile(int budget) {
    if (!g_prof || g_ncores <= 0) return 0;
    rv32i_profile_kernel<<<1, 1, 33 * sizeof(uint32_t)>>>(g_state, g_mem, g_prof, g_jump, budget);
    cudaError_t le = cudaGetLastError(), se = cudaDeviceSynchronize();
    return le != cudaSuccess ? (int)le : (int)se;
}

API int cuda_rv32i_uart_drain(int core, unsigned char* dst, int maxlen) {
    Periph* p = g_mem[core].per; int n = 0;
    while (p->tx_tail != p->tx_head && n < maxlen) { dst[n++] = p->tx[p->tx_tail & TXM]; p->tx_tail++; }
    return n;
}
API void cuda_rv32i_uart_feed(int core, const unsigned char* src, int len) {
    Periph* p = g_mem[core].per;
    for (int i = 0; i < len; i++) { p->rx[p->rx_head & RXM] = src[i]; p->rx_head++; }
}
API void cuda_rv32i_kbd_feed(int core, unsigned int entry) {
    Periph* p = g_mem[core].per; p->kbd[p->kbd_head & KBM] = entry; p->kbd_head++;
}
API void cuda_rv32i_kbd_set_mod(int core, unsigned int mod) { g_mem[core].per->kbd_mod = mod; }
API void cuda_rv32i_mouse_feed(int core, int dx, int dy, unsigned int buttons) {
    Periph* p = g_mem[core].per;
    p->mouse_dx += dx; p->mouse_dy += dy; p->mouse_buttons = buttons;
    p->mouse_has = (p->mouse_dx || p->mouse_dy || p->mouse_buttons) ? 1u : 0u;
}
API int cuda_rv32i_midi_drain(int core, unsigned int* dst, int maxlen) {
    Periph* p = g_mem[core].per; int n = 0;
    while (p->midi_tail != p->midi_head && n < maxlen) { dst[n++] = p->midi[p->midi_tail & MDM]; p->midi_tail++; }
    return n;
}
API void cuda_rv32i_audio_snapshot(int core, unsigned int* o) {
    Periph* p = g_mem[core].per;
    o[0]=p->au_ctrl; o[1]=p->au_rate; o[2]=p->au_chan; o[3]=p->au_bits;
    o[4]=p->au_bufstart; o[5]=p->au_buflen; o[6]=p->au_pos; o[7]=p->au_wrgen;
}
API unsigned int cuda_rv32i_display_take_vsync(int core) {
    Periph* p = g_mem[core].per; unsigned int v = p->dc_vsync; p->dc_vsync = 0; return v;
}
API unsigned int cuda_rv32i_display_fbaddr(int core) { return g_mem[core].per->dc_fbaddr; }
API void cuda_rv32i_set_time(int core, unsigned int usLo, unsigned int usHi, unsigned int msLo, unsigned int msHi,
                             unsigned int epLo, unsigned int epHi, unsigned int sec, unsigned int subus) {
    Periph* p = g_mem[core].per;
    p->rtc_us_lo=usLo; p->rtc_us_hi=usHi; p->rtc_ms_lo=msLo; p->rtc_ms_hi=msHi;
    p->rtc_epoch_lo=epLo; p->rtc_epoch_hi=epHi; p->rtc_sec=sec; p->rtc_subus=subus;
}

API void cuda_rv32i_shutdown() {
    if (g_mem) {
        for (int i = 0; i < g_ncores; i++) {
            cudaFree(g_mem[i].ram); cudaFree(g_mem[i].fb); cudaFree(g_mem[i].pcm); cudaFree(g_mem[i].trap); cudaFree(g_mem[i].per);
        }
        cudaFree(g_mem); g_mem = nullptr;
    }
    if (g_state) { cudaFree(g_state); g_state = nullptr; }
    if (g_prof) { cudaFree(g_prof); g_prof = nullptr; }
    if (g_jump) { cudaFree(g_jump); g_jump = nullptr; }
    if (g_sched) { cudaFree(g_sched); g_sched = nullptr; g_sched_words = 0; }
    if (g_ro) { cudaFree(g_ro); g_ro = nullptr; }
    g_ncores = 0;
}

// ── Shared read-only image (throughput model) ───────────────────────────────
// Allocate ONE device copy of the guest's RO span [lo, hi) and point every
// core's CoreMem at it. After this, fetches and reads in [lo, hi) hit the one
// shared, cache-resident buffer instead of N duplicate per-core RAM copies — so
// N independent guests share a single instruction-fetch working set, which is
// the dominant per-step cost. `data` holds (hi-lo) bytes (the host slices it
// from the committed image). Passing words==0 / lo>=hi disables sharing on all
// cores (clears ro), which restores the per-core-RAM path (bit-identical).
//
// Correctness: writes are NEVER routed here (mem_write always targets per-core
// RAM); correct guest code never writes its own .text/rodata, so the per-core
// RAM copy of [lo, hi) is write-dead and the shared read is authoritative. The
// span must lie wholly within [0, ram_size) and be 4-aligned (it is — the host
// page-aligns it). Idempotent: re-commits free the previous buffer first.
API int cuda_rv32i_set_shared_ro(const void* data, unsigned int lo, unsigned int hi) {
    if (g_ro) { cudaFree(g_ro); g_ro = nullptr; }
    if (hi <= lo) {                              // disable sharing on every core
        for (int i = 0; i < g_ncores; i++) { g_mem[i].ro = nullptr; g_mem[i].ro_lo = 0; g_mem[i].ro_hi = 0; }
        cudaDeviceSynchronize();
        return (int)cudaGetLastError();
    }
    unsigned int bytes = hi - lo;
    cudaError_t e;
    if ((e = cudaMalloc(&g_ro, bytes)) != cudaSuccess) return (int)e;
    if ((e = cudaMemcpy(g_ro, data, bytes, cudaMemcpyHostToDevice)) != cudaSuccess) return (int)e;
    for (int i = 0; i < g_ncores; i++) { g_mem[i].ro = g_ro; g_mem[i].ro_lo = lo; g_mem[i].ro_hi = hi; }
    cudaDeviceSynchronize();
    return (int)cudaGetLastError();
}
