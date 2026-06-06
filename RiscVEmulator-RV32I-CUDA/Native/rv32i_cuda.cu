#include <cstdint>
#include <cstring>
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
    const uint32_t instr = mem_read<uint32_t>(mm, cpu.pc);
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
        switch (f3) {
            case 0: r = (uint32_t)(s1 + iimm); break;
            case 1: r = u1 << sh; break;
            case 2: r = s1 < iimm ? 1u : 0u; break;
            case 3: r = u1 < (uint32_t)iimm ? 1u : 0u; break;
            case 4: r = u1 ^ (uint32_t)iimm; break;
            case 5: r = f7 == 0x20 ? (uint32_t)(s1 >> sh) : u1 >> sh; break;
            case 6: r = u1 | (uint32_t)iimm; break;
            case 7: r = u1 & (uint32_t)iimm; break;
        }
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
        } else switch (f3) {
            case 0: r = f7 == 0x20 ? (uint32_t)(s1 - s2) : (uint32_t)(s1 + s2); break;
            case 1: r = u1 << (s2 & 0x1F); break;
            case 2: r = s1 < s2 ? 1u : 0u; break;
            case 3: r = u1 < u2 ? 1u : 0u; break;
            case 4: r = u1 ^ u2; break;
            case 5: r = f7 == 0x20 ? (uint32_t)(s1 >> (s2 & 0x1F)) : u1 >> (s2 & 0x1F); break;
            case 6: r = u1 | u2; break;
            case 7: r = u1 & u2; break;
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
    Hart h; h.regs = &s_regs[threadIdx.x * 32];
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

#define API extern "C" __declspec(dllexport)

static CoreState* g_state  = nullptr;
static CoreMem*   g_mem    = nullptr;
static int        g_ncores = 0;

API int cuda_rv32i_init(int nCores, unsigned int ramSize, unsigned int fbW, unsigned int fbH, unsigned int pcmBytes) {
    g_ncores = nCores;
    cudaError_t e;
    if ((e = cudaMallocManaged(&g_state, (size_t)nCores * sizeof(CoreState))) != cudaSuccess) return (int)e;
    if ((e = cudaMallocManaged(&g_mem,   (size_t)nCores * sizeof(CoreMem)))   != cudaSuccess) return (int)e;
    memset(g_state, 0, (size_t)nCores * sizeof(CoreState));
    memset(g_mem,   0, (size_t)nCores * sizeof(CoreMem));

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
    int block = g_ncores < 256 ? g_ncores : 256;
    int    grid  = (g_ncores + block - 1) / block;
    size_t shmem = (size_t)block * 32 * sizeof(uint32_t);
    rv32i_kernel<<<grid, block, shmem>>>(g_state, g_mem, g_ncores, budget);
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
    g_ncores = 0;
}
