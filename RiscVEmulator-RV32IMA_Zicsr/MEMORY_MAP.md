# RV32I Emulator — Memory Map & Peripheral Reference

## Address Space Overview

```
 Address Range          Size       Device                 Description
─────────────────────────────────────────────────────────────────────────────
 0x00000000–0x00FFFFFF  16 MB      RAM                    Main memory (configurable)
 0x02000000–0x0200FFFF  64 KB      CLINT Timer            RISC-V standard timer (per-step counter)
 0x10000000–0x100000FF  256 B      UART (16550)           Serial console I/O
 0x10001000–0x100010FF  256 B      Keyboard Controller    Scancode FIFO + modifiers
 0x10002000–0x100020FF  256 B      Mouse Controller       Delta position + buttons
 0x10003000–0x100030FF  256 B      Real-Time Clock        Wall-clock microseconds & epoch
 0x20000000–0x2003FFFF  256 KB     Framebuffer            320×200 RGBA8888 pixel buffer
 0x20100000–0x201000FF  256 B      Display Control        Resolution, vsync, palette, mode
 0x30000000–0x300FFFFF  1 MB       Audio PCM Buffer       Raw PCM sample data
 0x30100000–0x301000FF  256 B      Audio Control          Playback config & status
```

---

## RAM (0x00000000)

General-purpose read/write memory. Default 16 MB, configurable via `--ram` flag.
ELF code/data is loaded starting at 0x00001000 (entry point).
Stack pointer is set to 0x009FFF00 by default (grows downward).

---

## CLINT Timer (0x02000000) — `TimerDevice`

RISC-V standard Core-Local Interruptor timer. The 64-bit `mtime` counter
increments once per CPU instruction (not wall-clock time).

| Offset | Name         | R/W | Description                              |
|--------|-------------|-----|------------------------------------------|
| 0x4000 | MTIMECMP_LO | R/W | Timer compare register, low 32 bits      |
| 0x4004 | MTIMECMP_HI | R/W | Timer compare register, high 32 bits     |
| 0xBFF8 | MTIME_LO    | R/W | Tick counter, low 32 bits                |
| 0xBFFC | MTIME_HI    | R/W | Tick counter, high 32 bits               |

**Note:** MTIME counts CPU steps, not real time. For wall-clock time, use the
Real-Time Clock device at 0x10003000.

---

## UART — 16550 (0x10000000) — `UartDevice`

Minimal 16550-compatible UART for text console I/O.

| Offset | Name    | R/W | Description                               |
|--------|---------|-----|-------------------------------------------|
| 0x00   | THR/RBR | R/W | Write = transmit byte; Read = receive byte|
| 0x05   | LSR     | RO  | bit 0 = RX data ready, bit 5 = TX empty   |

- **TX:** Write a byte to +0x00 → calls `OutputHandler` (prints to host console).
- **RX:** Host calls `EnqueueInput(byte)` → guest reads from +0x00.
- LSR bit 5 is always set (TX always ready).

---

## Keyboard Controller (0x10001000) — `KeyboardDevice`

Scancode FIFO with modifier state. Host (SDL) enqueues key events;
guest polls STATUS then reads DATA to consume them.

| Offset | Name     | R/W | Description                                        |
|--------|----------|-----|----------------------------------------------------|
| 0x00   | STATUS   | RO  | bit 0 = key event available in FIFO                |
| 0x04   | DATA     | RO  | Pop: bits [7:0] = keycode, bit 8 = pressed (1) / released (0) |
| 0x08   | MODIFIER | RO  | bit 0 = Shift, bit 1 = Ctrl, bit 2 = Alt           |

**Keycodes:** Printable ASCII passes through (a=0x61, A=0x41, 1=0x31, etc.).
Special keys use Windows VK-style codes:

| Key         | Code |   | Key        | Code |
|-------------|------|---|------------|------|
| LEFT        | 0x25 |   | RIGHT      | 0x27 |
| UP          | 0x26 |   | DOWN       | 0x28 |
| SHIFT       | 0x10 |   | CTRL       | 0x11 |
| ALT         | 0x12 |   | CAPSLOCK   | 0x14 |
| HOME        | 0x24 |   | END        | 0x23 |
| PAGE UP     | 0x21 |   | PAGE DOWN  | 0x22 |
| INSERT      | 0x2D |   |            |      |

---

## Mouse Controller (0x10002000) — `MouseDevice`

Relative mouse input with button state. Deltas accumulate until read
(read clears to zero).

| Offset | Name    | R/W | Description                                     |
|--------|---------|-----|-------------------------------------------------|
| 0x00   | STATUS  | RO  | bit 0 = mouse data available                    |
| 0x04   | DX      | RO  | Signed delta X since last read (resets on read)  |
| 0x08   | DY      | RO  | Signed delta Y since last read (resets on read)  |
| 0x0C   | BUTTONS | RO  | bit 0 = left, bit 1 = right, bit 2 = middle     |

---

## Real-Time Clock (0x10003000) — `RealTimeClockDevice`

Wall-clock time from the host system. Unlike CLINT (which counts CPU steps),
this provides real elapsed time in microseconds, milliseconds, and Unix epoch.

| Offset | Name      | R/W | Description                                    |
|--------|-----------|-----|------------------------------------------------|
| 0x00   | TIME_LO   | RO  | Microseconds since boot, low 32 bits           |
| 0x04   | TIME_HI   | RO  | Microseconds since boot, high 32 bits          |
| 0x08   | TIME_MS_LO| RO  | Milliseconds since boot, low 32 bits           |
| 0x0C   | TIME_MS_HI| RO  | Milliseconds since boot, high 32 bits          |
| 0x10   | EPOCH_LO  | RO  | Unix epoch seconds (since 1970), low 32 bits   |
| 0x14   | EPOCH_HI  | RO  | Unix epoch seconds (since 1970), high 32 bits  |

**Usage from C:**
```c
#define RTC_BASE      0x10003000
#define RTC_TIME_LO   (*(volatile unsigned int*)(RTC_BASE + 0x00))
#define RTC_TIME_HI   (*(volatile unsigned int*)(RTC_BASE + 0x04))
#define RTC_MS_LO     (*(volatile unsigned int*)(RTC_BASE + 0x08))
#define RTC_EPOCH_LO  (*(volatile unsigned int*)(RTC_BASE + 0x10))

unsigned int get_ms(void) { return RTC_MS_LO; }
unsigned long long get_us(void) {
    unsigned int lo = RTC_TIME_LO;
    unsigned int hi = RTC_TIME_HI;
    return ((unsigned long long)hi << 32) | lo;
}
```

---

## Framebuffer (0x20000000) — `FramebufferDevice`

Direct-mapped RGBA8888 pixel buffer. Guest writes pixels; host reads to render.
Default resolution 320×200 (256,000 bytes). Row-major, top-left origin.

| Byte offset          | Pixel    |
|----------------------|----------|
| 0x00000              | (0,0) R  |
| 0x00001              | (0,0) G  |
| 0x00002              | (0,0) B  |
| 0x00003              | (0,0) A  |
| 0x00004              | (1,0) R  |
| ...                  | ...      |
| `(y*320+x)*4`       | (x,y)    |

The host renders this buffer continuously (~120 fps) regardless of guest writes.

---

## Display Control (0x20100000) — `DisplayControlDevice`

Configuration and synchronization for the framebuffer.

| Offset | Name          | R/W | Description                              |
|--------|---------------|-----|------------------------------------------|
| 0x00   | WIDTH         | RO  | Framebuffer width (default 320)          |
| 0x04   | HEIGHT        | RO  | Framebuffer height (default 200)         |
| 0x08   | BPP           | RO  | Bits per pixel (always 32)               |
| 0x0C   | VSYNC_FLAG    | R/W | Guest writes 1 to hint frame is ready    |
| 0x10   | PALETTE_INDEX | WO  | Select palette entry (0–255)             |
| 0x14   | PALETTE_DATA  | WO  | Write 0x00RRGGBB to selected entry       |
| 0x18   | MODE          | R/W | 0 = direct RGBA, 1 = 8-bit indexed      |

**VSYNC_FLAG** is a hint only — the display renders continuously. The host
clears it after each present.

---

## Audio PCM Buffer (0x30000000) — `AudioBufferDevice`

1 MB region for raw PCM audio samples. Guest writes sample data here,
then configures the Audio Control device to play it.

---

## Audio Control (0x30100000) — `AudioControlDevice`

Controls audio playback from the PCM buffer.

| Offset | Name        | R/W | Description                                |
|--------|-------------|-----|--------------------------------------------|
| 0x00   | CTRL        | R/W | bit 0 = play, bit 1 = loop, bit 2 = stop  |
| 0x04   | STATUS      | RO  | bit 0 = playing                            |
| 0x08   | SAMPLE_RATE | R/W | Hz (default 22050; Doom uses 11025)        |
| 0x0C   | CHANNELS    | R/W | 1 = mono, 2 = stereo                       |
| 0x10   | BIT_DEPTH   | R/W | 8 or 16                                    |
| 0x14   | BUF_START   | R/W | Byte offset into audio buffer              |
| 0x18   | BUF_LENGTH  | R/W | Length in bytes                             |
| 0x1C   | POSITION    | RO  | Current playback position                  |

**Writing CTRL bit 2 (stop)** resets CTRL to 0 and POSITION to 0.

---

## Guest Memory Layout (Doom configuration)

```
 0x00001000   ELF entry point (code start)
 0x000xxxxx   .text, .rodata, .data, .bss sections
 0x000xxxxx   _heap_start (after BSS, set by linker.ld)
     ↓        Heap grows upward (8 MB limit)
     ↑        Stack grows downward
 0x009FFF00   Initial stack pointer
 0x009FFFFC   WAD size (u32, written by --load)
 0x00A00000   WAD data start (loaded by --load)
 0x01000000   End of 16 MB RAM
```

---

## CLI Usage

```
RiscVEmulator.Frontend <elf-file> [options]
  --scale N              Window scale factor (default: 3)
  --ram N                RAM size in MB (default: 16)
  --load <file> <addr>   Load binary file at hex address (repeatable)
                         Size written as u32 at addr-4 automatically
```

**Example — Run Doom:**
```
RiscVEmulator.Frontend doom.elf --load doom1.wad 0x00A00000 --scale 3
```
