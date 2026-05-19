\# Feasibility study: paravirtualizing RISC-V RV32I NOMMU Linux to remove S/U privilege from the emulator

## Current implementation status

The privilege-removal program this study proposes is essentially complete. The
native CPU (`Native/rv32i_core.cpp`) is now a base RV32I integer datapath plus
a small trap unit, with **no CSRs at all**:

- **Zicsr removed.** Every CSR instruction (`opcode 0x73, funct3 != 0`) traps
  as illegal, as do `MRET`, `SRET` and `WFI`. S-mode, trap delegation
  (`medeleg`/`mideleg`), PMP, machine-ID, ENVCFG and SEED are all gone.
- **Trap state is memory, not CSRs.** The trap unit keeps only two registers —
  current privilege and the host interrupt-pin latch. Interrupt-enable,
  per-source mask, handler vector and the saved trap frame live in the
  **trap-frame page** at guest-physical `0x0F000000` (`TrapFrameDevice`),
  which is plain RAM. Its 36-word landing pad uses the RISC-V `struct pt_regs`
  layout, so the kernel's `pt_regs` *is* a valid trap frame.
- **Userspace uses a real `ecall`.** An earlier experiment routed userspace
  syscalls through a magic `0xFFFF0000` gateway address; that was dropped.
  Userspace now executes the architectural `ecall` and the CPU raises the
  usual environment-call trap. `DirectUserEcallTest` covers this.
- **Trap return is the one non-architectural piece.** The base ISA has no
  `MRET`; a handler returns from a trap by putting its private frame pointer
  in `a0` and jumping to the fixed resume gateway at `0xFFFF0004`
  (`PV_RESUME_GATEWAY`). The CPU detects that fetch address and applies `mret`
  semantics — no privileged instruction is decoded. `do_trap` itself performs
  the trap-entry `tp`↔scratch swap, so `entry.S` needs no CSR bootstrap.
- **The CLINT timer left the core.** `mtime`/`mtimecmp` and the wall-clock
  math live in the C# `ClintDevice`; the core keeps only an `mtip`
  interrupt-input pin (`rv32i_set_mtip`), mirroring `rv32i_set_meip`.
  `check_interrupts()` does no timer compute.

`Examples/Linux.Build_RV32i` rewrites the kernel's `entry.S`/`head.S` CSR
sites and `irqflags.h` to target the `0x0F000000` trap-frame page, and the
`ret_from_exception` `mret` to the `0xFFFF0004` gateway. The rebuilt M/U NOMMU
kernel boots to userspace; `LinuxTest` covers it. The bare-metal trap tests
(`atomic_trap.c`, `f_trap.c`, `direct_user_ecall.c`, `s_mode_removed.c`,
`nested_trap.c`, `paravirt_syscall_gateway.c`, `timer_irq.c`) install their
handlers through the same trap-frame page.

The verdict and design analysis below were written while scoping this work;
they are kept as the rationale for the design that shipped. Where they describe
an intermediate plan (a separate `0xFFFF0000` syscall gateway, a CSR-aliasing
MMIO device), the shipped design went further — userspace keeps a real `ecall`
and trap state is the plain-RAM trap-frame page, not an aliasing device.

\## TL;DR — verdict



\*\*Possible but hard, leaning toward "moderately hard prototype, very hard to keep working long-term."\*\* A first prototype that boots a NOMMU Linux + BusyBox/uClibc-ng image inside an emulator that implements only RV32I (no S/U privilege, no `sret`/`mret`, no CSR machinery) is realistic in roughly the difficulty class of "write your own arch port" — comparable to UML, KML, or LKL in conceptual scope but smaller in code volume because you start from an existing arch port (NOMMU RISC-V) rather than from scratch. The blockers are almost all in two places: the entry/exit assembly (`arch/riscv/kernel/entry.S`) and the signal-delivery trampoline, plus the small set of fixed instructions that uClibc-ng and the kernel emit for `ecall`. None of those are conceptually difficult to redirect; the hard part is keeping the redirection self-consistent across every code path that enters/exits the "kernel."



Below: detailed answers, references, a proposed minimal prototype, file/function list, risks, and a recommended first experiment.



\---



\## 1. How UML implements kernel/user separation, syscalls, traps, scheduling, signals



UML is the most-cited prior art for your goal, so it is worth being precise about what UML actually does — because most of its mechanism is \*\*not\*\* what you want.



\*\*Two historical modes:\*\* TT (tracing thread) mode, now removed, ran every guest process as a thread of the UML kernel process. SKAS (separate kernel address space) mode is the modern default. In SKAS, the UML kernel runs as one host process; each guest process is a \*separate\* host process created via `clone()` and put under `ptrace()` by the UML kernel. Every syscall the guest tries to make is intercepted by the host kernel via the ptrace mechanism and routed to the UML kernel for handling. UML's "physical RAM" is just an `mmap()`'d region on the host, and UML builds its own page tables inside that region. The UML kernel maintains and switches between guest processes by manipulating host page tables via a small "syscall stub" (in `arch/um/kernel/skas/`), which only ever issues `mmap`, `munmap`, and `mprotect` host syscalls. Signals are how interrupts and traps get \*into\* the UML kernel: host SIGSEGV becomes guest page-fault delivery, SIGALRM becomes a guest timer interrupt, etc. — see LWN's writeup of skas0 for details on `syscall\_stub`, `stub\_segv\_handler`, and `run\_syscall\_stub` (`arch/um/kernel/skas/process.c`, `arch/um/kernel/skas/mem\_user.c`).



\*\*Why UML's design does not directly apply to you.\*\* UML is architected around the assumption that a real host kernel below it provides protection, separate address spaces, real signals, and `ptrace`. You have none of that — you have a bare RV32I emulator. The conceptual lesson from UML, however, is the key one: \*the kernel/user transition is a software contract, not a hardware contract.\* UML demonstrates that Linux can run with kernel transitions that are not the architectural trap mechanism. The mechanism it picks (ptrace + host signals) is specific to a hosted environment. Yours will be different.



Concepts portable to your setting: (a) trap frames are just memory; (b) "interrupts" can be delivered by jumping to a registered handler at convenient safe points; (c) the kernel can ignore the architectural privilege concept entirely if all userspace is trusted.



Concepts that are x86/host-specific and don't apply: (a) anything based on `ptrace`, `clone()`, host `mmap`; (b) the SKAS0 stub; (c) the use of host signal numbers; (d) anything in `arch/um/os-Linux/`.



Closer prior art for what you actually want:

\- \*\*KML (Kernel Mode Linux), Maeda 2003\*\* — runs \*user\* processes in kernel mode and turns `int 0x80`/syscall into function calls. Your scenario is the inverse (collapse kernel to user mode), but the function-call-instead-of-trap idea is the same. KML uses an `mtvec`-equivalent fast path and a per-arch patch to glibc's syscall stub.

\- \*\*UKL (Unikernel Linux, Raza et al., 2022)\*\* — links kernel and a single application into one image, replaces `syscall` with `call`, returns via `ret` instead of `sret/sysret`. About 550–1250 LOC of kernel patches. Conceptually nearly identical to what you're proposing, only at finer granularity than a unikernel (you still want multiple processes).

\- \*\*LKL (Linux Kernel Library)\*\* — `arch/lkl`, \~3500 LOC. Linux becomes a callable library; host operations are a tiny set (`thread\_create`, semaphore, timer-schedule, IRQ-inject). Important precedent that Linux can run with no hardware privilege boundary at all, and that the porting surface is small once you accept a NOMMU arch port. See `arch/lkl/include/uapi/asm/host\_ops.h` and the LWN summary.



\## 2. RISC-V NOMMU Linux entry/exit paths you must patch



For mainline `linux/master`:



\*\*Userspace ecall (the syscall path).\*\*

\- `arch/riscv/kernel/entry.S` — `SYM\_CODE\_START(handle\_exception)` is the single trap entry. Hardware behavior: on `ecall` from U-mode, `sepc ← pc`, `scause ← 8` ("Environment call from U-mode"), privilege → S, `pc ← stvec`. Entry code uses `CSR\_SCRATCH` (`sscratch` or `mscratch` depending on `CONFIG\_RISCV\_M\_MODE`) to swap to the kernel `tp`, builds a `pt\_regs` on the kernel stack, then dispatches via `excp\_vect\_table` — the table entry for cause 8 is `do\_trap\_ecall\_u` (in `arch/riscv/kernel/traps.c`).

\- `do\_trap\_ecall\_u` advances `regs->epc += 4` and calls `syscall\_handler()` which indexes `sys\_call\_table`.



\*\*Return to user (`sret`).\*\* `ret\_from\_exception` in `entry.S` restores all registers from `pt\_regs`, writes `tp` back into `CSR\_SCRATCH`, and ends with `sret` (or `mret` if `CONFIG\_RISCV\_M\_MODE=y`). The architectural transition is `xRET`: `pc ← sepc/mepc`, privilege ← `xPP`. Removing `sret` is one of your central edits.



\*\*Trap frame save/restore.\*\* Everything is in `entry.S`, primarily `.Lsave\_context` and the load sequence before `xret`. `pt\_regs` layout is in `arch/riscv/include/asm/ptrace.h`; offsets in `arch/riscv/kernel/asm-offsets.c`.



\*\*Signal delivery / sigreturn.\*\* `arch/riscv/kernel/signal.c`:

\- `setup\_rt\_frame()` builds an `rt\_sigframe` on the user stack containing `siginfo`, `ucontext` (with `mcontext` = full register state), then sets `regs->epc = handler`, `regs->sp = frame`, `regs->ra = trampoline`. For MMU systems the trampoline is in the VDSO (`arch/riscv/kernel/vdso/rt\_sigreturn.S`). \*\*For NOMMU there is no VDSO\*\*, so the kernel copies a two-instruction `\_\_user\_rt\_sigreturn\[2]` array (`li a7, \_\_NR\_rt\_sigreturn; ecall`) onto the user stack at `frame->sigreturn\_code` and points `ra` at it. The kernel also calls `flush\_icache\_range` on that location (NOMMU fix from April 2023, commit `8d736482749f`).

\- `sys\_rt\_sigreturn` (in `signal.c`) restores `pt\_regs` from the user's `mcontext` and returns to `ret\_from\_exception`.



\*\*Context switch.\*\* `arch/riscv/kernel/entry.S` — `SYM\_FUNC\_START(\_\_switch\_to)`. Saves callee-saved registers (`s0–s11`, `sp`, `ra`, `tp`) of `prev` into `prev->thread.\*`, loads them from `next->thread.\*`, returns. Wrapper `switch\_to()` is in `arch/riscv/include/asm/switch\_to.h`. There is no privilege transition involved in a context switch — only the kernel-side callee-saved state moves. This is the \*easiest\* of your paths.



\*\*Timer interrupt / preemption.\*\* Cause is `Interrupt(SupervisorTimer)`. `handle\_exception` checks the MSB of `scause` and calls `do\_irq`. The RISC-V interrupt-controller driver `drivers/clocksource/timer-riscv.c` (with `CONFIG\_RISCV\_TIMER`) handles the timer; on NOMMU/M-mode it programs `mtimecmp` directly via MMIO instead of going through SBI. Preemption decision is in `irqentry\_exit\_to\_user\_mode` (generic) / `ret\_from\_exception` (arch).



\*\*Sources:\*\* `arch/riscv/kernel/entry.S` on `torvalds/linux` master; SiFive's "All Aboard Part 7" blog post (Palmer Dabbelt) describes the design intent. xv6-riscv-book §4 is the cleanest pedagogical walkthrough of the exact same primitives.



\## 3. uClibc-ng syscall stubs on RISC-V



uClibc-ng for RV32 NOMMU lives under `libc/sysdeps/linux/riscv64` (the directory is reused for 32-bit thanks to ABI commonality; or check `riscv32` if your tree has it split). The single relevant file for the syscall machinery is `libc/sysdeps/linux/riscv64/sysdep.h`. The \*\*only\*\* instruction emitted to enter the kernel is `scall` (an old alias for `ecall`) inside two macro families:



```

PSEUDO(name, syscall\_name, args):

&#x20;   li a7, SYS\_ify(syscall\_name)

&#x20;   scall                             ← this is the ecall

&#x20;   li a7, -4096

&#x20;   bgtu a0, a7, .Lsyscall\_error...



internal\_syscallN(number, err, ...):

&#x20;   register long \_\_a7 asm("a7") = number;

&#x20;   register long \_\_a0 asm("a0") = arg0;

&#x20;   ...

&#x20;   asm volatile ("scall\\n\\t" : "+r"(\_\_a0) : "r"(\_\_a7), ...);

```



So you have two stub paths: PIC/non-PIC assembly stubs from `PSEUDO` (used by `syscalls.list`-generated wrappers) and inline `internal\_syscallN` macros (used directly from C). Both contain \*exactly one\* `ecall`/`scall` instruction. Also `\_\_clone` in `clone.S` uses `scall`, and `vfork.S` does. NPTL's `nptl-sysdep.S` may add a couple more.



\*\*Replacing `ecall` with a paravirtual gateway is straightforward\*\* because:

1\. There is only one syntactic site per macro.

2\. All stubs already place the syscall number in `a7` and arguments in `a0..a5` exactly as the kernel would expect — i.e., the calling convention going \*into\* the kernel matches a normal function call to `(a0, a1, a2, a3, a4, a5)` with the additional `a7` selector. You can quite literally replace `scall` with `jal ra, \_\_pv\_syscall\_gateway` (or `call`) and have a C function on the other side that calls a kernel-side dispatcher.

3\. The return contract is also straightforward: kernel returns through `a0`, with error encoded as a negative value in the range `-1..-4095` — same on `ret` as on `sret`.



\*\*One subtlety.\*\* The kernel internally still expects the syscall to arrive on a `pt\_regs` (the entry code synthesizes one). Your paravirtual gateway must do this synthesis itself in C, copying `a0..a5, a7`, and a saved `epc = ra` into a `struct pt\_regs` on the kernel stack before invoking `syscall\_handler()`.



References: `cgit.uclibc-ng.org/cgi/cgit/uclibc-ng.git/tree/libc/sysdeps/linux/riscv64/sysdep.h`, the glibc patch series at sourceware that the uClibc-ng port mirrors closely, and the `clone.S`/`vfork.S` files.



\## 4. The model: cooperative single-ring Linux for BusyBox/uClibc



Yes — assuming you trust all userspace, BusyBox + uClibc-ng NOMMU can absolutely run cooperatively in a single privilege "ring." This is exactly the UKL model, applied to a NOMMU embedded system. The properties you can rely on:



\- BusyBox issues syscalls only through uClibc-ng's `INLINE\_SYSCALL`/`PSEUDO` macros — there is no inline assembly with raw `ecall` in BusyBox proper. Replace the stubs and you are done.

\- BusyBox does not depend on hardware privilege isolation for correctness.

\- uClibc-ng NOMMU already assumes a flat address space (BFLT). FDPIC isn't used. There is no MMU-derived behavior the C library will trip on.

\- The existing `nommu\_k210\_defconfig` already turns off `FUTEX`, `EPOLL`, `SIGNALFD`, `TIMERFD`, `EVENTFD`, `AIO`, `IO\_URING`, `ADVISE\_SYSCALLS`, `MEMBARRIER`, and `MMU`, so the kernel-side surface area is already minimized.



The model that fits is "kernel and userspace are colinked but logically distinct" — i.e., your address space contains the kernel image at one range and BFLT user binaries loaded into another, and the `\_\_pv\_syscall\_gateway` is a known address shared between them. This is structurally identical to UKL "base model."



\## 5. Minimum kernel/userspace patches for a `/init`-runs-BusyBox prototype



Roughly the following, ordered by where the changes go:



\*\*Kernel (a) — replace the trap entry.\*\*

\- `arch/riscv/kernel/entry.S`: replace `handle\_exception` with a stub that is \*never invoked\* (since you have no traps). Provide a new C entry `pv\_syscall\_entry(unsigned long a0, ..., unsigned long a7, unsigned long ret\_addr)` that builds a `pt\_regs` and tail-calls `syscall\_handler` from `arch/riscv/kernel/traps.c` (or inlines its logic). Export it at a fixed address (linker script symbol) or place it at a known offset and have userspace `jal` to it.

\- Remove or guard out `sret`/`mret` in `ret\_from\_exception` and in `\_\_switch\_to` ramp-up paths; replace with `ret`.

\- Remove the `csrrw/csrr` dance on entry/exit; you don't have CSRs. Kernel `tp` is just a memory location that the gateway loads.



\*\*Kernel (b) — first-task entry path.\*\*

\- `arch/riscv/kernel/process.c`: `start\_thread()` sets up the initial `pt\_regs` so that `ret\_from\_exception` will return to user. Change to: directly `jalr` to the user entry, with the userspace stack established in `sp`.

\- `arch/riscv/kernel/head.S`: `\_start` is unchanged in concept but no MMU relocation is needed (it already isn't for NOMMU). Skip `csrw stvec` and any other CSR setup.



\*\*Kernel (c) — interrupts/timer/preemption.\*\*

\- The hard one. You need \*some\* asynchronous mechanism to call into the kernel for the timer tick, or you give up preemption. Two clean options:



&#x20; - \*\*Cooperative-only\*\*: kernel only runs when called via the gateway. Userspace polls or no preemption. BusyBox runs fine this way for non-blocking workloads, but `sleep(1)`, `select`, anything timer-driven, won't work the same. Reschedule only at syscall boundaries.



&#x20; - \*\*Emulator-injected callback\*\*: the emulator, when it would have delivered a timer interrupt, instead transparently inserts a `jal ra, \_\_pv\_irq\_entry` (saving the prior `pc` as `ra` in a designated scratch location) at a safe instruction boundary. This is exactly what a JIT or interpreter can do trivially. The kernel side is a new function `pv\_irq\_entry(saved\_pc, saved\_regs\_ptr)` that builds a `pt\_regs`, runs the timer IRQ handler, and `ret`s to the saved PC. This is the most UML-like piece of your design — UML uses host signals to do precisely this.



\- Either way: `drivers/clocksource/timer-riscv.c` still works if you keep `mtime`/`mtimecmp` MMIO (you said the emulator can still provide MMIO + timer + interrupts). You just remove the architectural delivery of cause-bit-set interrupts; the kernel polls or the emulator injects.



\*\*Kernel (d) — signal delivery.\*\*

\- `arch/riscv/kernel/signal.c::setup\_rt\_frame()`: the NOMMU branch already copies two instructions onto the user stack: `li a7, \_\_NR\_rt\_sigreturn; ecall`. \*\*Replace the `ecall` word with a call to your gateway\*\* — concretely, replace the static `\_\_user\_rt\_sigreturn\[2]` constant in `signal.c` (defined elsewhere as a `u32\[2]`) with the encoding of your trampoline. The simplest substitution is a `jal` to a kernel-known fixed address. You still need `flush\_icache\_range` (NOMMU already does it).

\- `sys\_rt\_sigreturn` in `signal.c` is unchanged in spirit but, since you have no `sret`, it must return through your normal "exit to user" path, which is now just a `ret` to the saved user PC.



\*\*Kernel (e) — context switch.\*\*

\- `\_\_switch\_to` in `entry.S` is fine as-is conceptually; it never used CSRs. Just keep `ret`, which it already uses.



\*\*uClibc-ng.\*\*

\- Patch `libc/sysdeps/linux/riscv64/sysdep.h` PSEUDO macro: replace `scall` with `call \_\_pv\_syscall\_gateway` (or `jal ra, <fixed>`). You'll need to preserve the existing ABI: `a7` = syscall number, `a0..a5` = args, return in `a0`, errors in `a0` being `-1..-4095`. A `call` writes `ra`, which is fine — it's caller-saved.

\- Patch `internal\_syscallN` macros similarly: replace `"scall\\n\\t"` with `"call \_\_pv\_syscall\_gateway\\n\\t"` and update the clobber list (`ra` is now clobbered as a caller-saved register, which it already implicitly is for `call`).

\- Patch `clone.S`, `vfork.S`, `nptl-sysdep.S` — same one-line substitution.

\- `\_\_riscv\_flush\_icache` is implemented as a system call in uClibc-ng (per the changelog entry from 2020-08-17); same substitution there.



\*\*Buildroot.\*\*

\- Toolchain: keep `riscv32-buildroot-linux-uclibc` with `-march=rv32i -mabi=ilp32`. You'll lose A/M/F/D; ensure libgcc soft-mul/div is enabled and that `-mno-atomic` (or whatever Buildroot calls the inline-A disable) is applied; uClibc-ng must be built without atomic-extension usage. (uClibc-ng inherits a few `\_\_atomic\_\*` builtin uses; under RV32I-only you must rely on libgcc-emulated atomics, which work on uniprocessor.)

\- BFLT: continue using `BR2\_BINFMT\_FLAT` + `elf2flt` per the Niklas Cassel patches that landed in 2022 (`fs/binfmt\_flat.c` GOTPLT fix). RISC-V `elf2flt` support lives at `github.com/floatious/elf2flt/tree/riscv`.

\- Kernel config: start from `arch/riscv/configs/nommu\_virt\_defconfig` (not `nommu\_k210\_defconfig`, because it depends on K210 SoC drivers); switch to RV32 by setting `ARCH\_RV32I=y`.



\## 6. What native emulator support is still required



You said the emulator already provides MMIO devices, a timer, interrupts, and RAM. Of those, here's what's \*minimally\* needed:



\- \*\*RAM at the configured load address\*\* (typically 0x80000000 for `nommu\_virt`).

\- \*\*A timer MMIO\*\* that produces something the kernel can poll or that the emulator translates into a call to `\_\_pv\_irq\_entry`. If you keep mtime/mtimecmp at their CLINT addresses, the existing `timer-riscv.c` driver works as-is, and the only thing you change is \*how\* the resulting "interrupt" arrives — not the device model.

\- \*\*A console MMIO\*\* — anything `8250`/`ns16550a` works with the in-tree driver; the K210 path uses SiFive UART.

\- \*\*A "syscall gateway" mechanism.\*\* You have three reasonable options:

&#x20; - (a) Replace `ecall` with a `call <kernel\_symbol\_addr>`. The emulator does nothing special — it's just a regular jump-and-link. The kernel must export `\_\_pv\_syscall\_gateway` at a stable address, and uClibc-ng must be built knowing that address (link-time symbol or fixed by linker script).

&#x20; - (b) Keep `ecall` as the instruction but \*\*redefine it\*\* in the emulator: instead of raising an exception, decode `ecall` as `jal ra, <gateway>`. This is the lightest-touch option for the userspace side (uClibc-ng requires \*zero\* changes) but it does mean the emulator now contains a tiny piece of OS-aware behavior.

&#x20; - (c) MMIO trigger: a load/store to a magic address causes the emulator to invoke a callback. Heavier and slower; not worth it.



&#x20; I recommend (a) — pure architectural — because it keeps the emulator dumb. You said you "prefer explicit helper calls we control, not simply deleting instructions"; (a) is precisely that.



\- \*\*CSR storage.\*\* RV32I (Zicsr extension stripped) has no CSR instructions, so the kernel must never execute `csrr`/`csrw`/`csrrw`. You'll need to ensure the build never emits them. The places that do today: `entry.S` (`csrrw tp, CSR\_SCRATCH, tp`, `csrc/csrs sstatus`, etc.), `head.S` (`csrw stvec`), `traps.c` (initial `csr\_write(CSR\_TVEC, ...)`), the SBI/M-mode timer code, and inline `csr\_read`/`csr\_write` macros in `asm/csr.h`. All of these need replacing with plain memory accesses in your patched arch port. \*No\* fake trap-cause/status structs are needed at the architectural level — the kernel can just have its own globals (`pv\_kernel\_tp`, `pv\_kernel\_sp`, etc.) that the gateway loads from.



\- \*\*Fake trap cause / status structs.\*\* Not required at all if you go with the function-call gateway model. The kernel's `pt\_regs` is a perfectly normal C struct on the kernel stack, populated by the gateway in C. The only "cause" you need to know is whether you entered via syscall or via injected IRQ — which is just \*which\* gateway function was called.



\- \*\*Helper MMIO/gateway addresses.\*\* Not strictly needed if the gateway is a function symbol. If you want to keep options open, designate a small fixed range (e.g. `0xFF000000–0xFF00FFFF`) as "paravirt area" and put gateway entry points there. This is cleaner than depending on the loader to expose kernel symbols to userspace.



\## 7. Hard blockers and risks



In order of severity:



1\. \*\*The `ecall` instruction appears in static, copy-to-stack code: the NOMMU rt\_sigreturn trampoline.\*\* This is the single most awkward case because the instruction word `0x00000073` (encoding of `ecall`) is a literal `u32` constant in `arch/riscv/kernel/signal.c`. You can patch it to whatever instruction encoding your gateway needs, but the patch must be consistent with whatever the emulator treats as a gateway call. With approach (a) above (replace with `jal ra, <gateway>`), the gateway address is encoded as a 20-bit signed PC-relative offset in the `jal` instruction — i.e., the immediate must be computed at sigframe construction time relative to the frame address. So you'd actually want to use a 2-instruction sequence: `la t0, gateway; jr t0` — but `la` is a pseudo that expands to `auipc + addi`, which is two instructions, total 12 bytes. Easy enough; just bump `sigreturn\_code` size.



2\. \*\*`ecall` in unexpected places.\*\* Aside from uClibc-ng, the rt\_sigreturn trampoline, and any vDSO-equivalents, you must audit:

&#x20;  - Any user binary that uses the raw `syscall()` libc wrapper or that has inline `ecall` (rare in BusyBox, but exists in some statically linked helpers).

&#x20;  - `\_\_riscv\_flush\_icache` calls from JITs (unlikely in your target).

&#x20;  - Anything dropped into memory by the dynamic loader — but you're BFLT-static, so this doesn't apply.

&#x20;  - \*\*Crucially:\*\* direct architectural user `ecall` remains supported. Any inline `ecall` missed by the gateway rewrite should still trap as cause 8 through the normal Linux-compatible path; the gateway is an optimization/experimentation ABI, not a compatibility requirement.



3\. \*\*CSR instructions in compiled-in kernel code.\*\* `Zicsr` is part of the base RV32I privileged ISA today and the kernel uses CSRs extensively. You will get assembler errors and linker errors and runtime "illegal instruction" failures until you methodically replace every CSR site. The list is long enough (`csr\_read`, `csr\_write`, `csr\_set`, `csr\_clear`, the inline asm in `entry.S` / `head.S` / `traps.c` / `irq-riscv-intc.c` / `time.c` / `kernel/sbi.c`) that it deserves its own audit pass. Strategy: define `CONFIG\_RISCV\_PARAVIRT` and add `#ifdef`s, replacing CSR accesses with memory accesses to a per-cpu `pv\_csr\_state` struct that the kernel keeps internally. Most CSR reads in the kernel are conceptually local kernel state (`sstatus.SIE`, `sscratch`) — they just happened to live in a CSR; making them memory is fine.



4\. \*\*Atomics on RV32I without A extension.\*\* uClibc-ng and the kernel both expect `lr.w/sc.w` or `amoadd.w` etc. You have neither. libgcc has `\_\_atomic\_\*` emulation that uses `\_\_sync\_synchronize` and disabled interrupts; on UP this works, but you must either compile everything with `-march=rv32i` \*and\* a libgcc that has the helpers built (Buildroot typically does, but verify), and make sure the kernel's `arch\_atomic\*` is redirected away from inline asm to the libgcc helpers. There's a `CONFIG\_GENERIC\_ATOMIC64` path; you may need a `CONFIG\_GENERIC\_ATOMIC32` equivalent (which doesn't exist in mainline RISC-V — would need a patch).



5\. \*\*`fence` / `fence.i` / cache flush.\*\* Not a blocker. The single-hart
core retires the whole `fence` opcode as a NOP, so `fence` and `fence.i` are
both harmless and need no kernel rewrite.



6\. \*\*The classifier of risks UML hit and you'll hit too\*\*: synchronization between kernel and "user" when they share an address space. Any place the kernel does `copy\_to\_user`/`copy\_from\_user` is currently a `lb`/`sb` with `SR\_SUM` set in `sstatus`. You don't have `sstatus`. Make `copy\_to\_user` a plain `memcpy` (NOMMU already does this — `arch/riscv/include/asm/uaccess.h` under `!CONFIG\_MMU` already collapses to direct access). Verify there are no leftover `csrs sstatus, SR\_SUM` instructions wrapping uaccess.



7\. \*\*Multitasking / SMP.\*\* You almost certainly want UP. NOMMU + K210 defconfig is already UP. Keep it that way; SMP would force you to think about IPIs, which means another paravirt entry point.



8\. \*\*GDB / debugging.\*\* Symbol resolution across kernel/BFLT-user is awkward in a shared address space. Plan for `vmlinux + binfmt\_flat` symbol files.



9\. \*\*Long-term maintenance.\*\* Every kernel release will touch `entry.S` and `signal.c`. Forking these is fine for a prototype; expect rebase pain at every LTS bump. UKL's experience (550–1250 LOC across many subsystems) is a fair estimate of your steady-state diff size.



\## 8. Proposed minimal prototype design



Define a `CONFIG\_RISCV\_PARAVIRT` Kconfig that depends on `!MMU` and provides:



\*\*One paravirt header\*\*, `arch/riscv/include/asm/paravirt.h`, containing:

\- Function symbols: `\_\_pv\_syscall\_gateway`, `\_\_pv\_irq\_entry`, `\_\_pv\_sigreturn\_gateway`, `\_\_pv\_resume\_user`.

\- Stub macros for `csr\_read`/`csr\_write` that redirect to a `struct pv\_cpu\_state` global (one per hart, but you have one hart).

\- An ABI document: gateway entry register convention is identical to syscall ABI; callee preserves callee-saved per the standard RISC-V ABI; gateway returns via `ret` into the user `ra`.



\*\*One paravirt C file\*\*, `arch/riscv/kernel/paravirt.c`, containing:

\- `\_\_pv\_syscall\_gateway(...)`: assembler entry that swaps `sp` to the kernel stack (load from `pv\_cpu\_state.kernel\_sp`), saves user GPRs into a `pt\_regs` on it, calls `syscall\_handler(pt\_regs\*)`, then restores user GPRs and `ret`s. Roughly 50 lines of asm.

\- `\_\_pv\_irq\_entry(...)`: same structure, but called from the emulator at safe points instead of from the user. Saves user state, calls `do\_irq` with synthesized cause, returns.

\- `\_\_pv\_resume\_user(struct pt\_regs \*)`: replaces the tail of `ret\_from\_exception` — checks `\_TIF\_WORK\_MASK`, handles pending signals and resched, then loads user GPRs and `ret`s. \*\*No `sret`.\*\*



\*\*Patches to existing files (numbers approximate):\*\*

\- `arch/riscv/kernel/entry.S`: 200-line edit, mostly deletions. `\_\_switch\_to` stays.

\- `arch/riscv/kernel/head.S`: remove the `csrw stvec` and any relocation/MMU code (already conditional in NOMMU); ensure end-of-boot tail-calls `start\_kernel` without privilege transition.

\- `arch/riscv/kernel/signal.c`: 20-line change. Replace `\_\_user\_rt\_sigreturn` with a 3-instruction sequence (`auipc + addi + jr` to `\_\_pv\_sigreturn\_gateway`), update `frame->sigreturn\_code` size, keep the `flush\_icache\_range`.

\- `arch/riscv/kernel/process.c`: `start\_thread` writes initial register state; tail must end with `\_\_pv\_resume\_user` rather than the existing return-to-user.

\- `arch/riscv/kernel/traps.c`: remove `csr\_write(CSR\_TVEC, ...)`. Strip out `do\_trap\_ecall\_u`/`do\_trap\_ecall\_s`/`do\_trap\_ecall\_m` if unused, or repurpose.

\- `arch/riscv/kernel/sbi.c`: skip entirely (we're not in S-mode and we're not calling M-mode SBI from "S-mode").

\- `arch/riscv/kernel/time.c`: replace SBI-set-timer with a direct write to a paravirt timer-set MMIO (which can just be CLINT mtimecmp).

\- `arch/riscv/include/asm/csr.h`: redirect every `csr\_\*` macro to memory ops on `pv\_cpu\_state`.

\- `arch/riscv/Kconfig`: add `CONFIG\_RISCV\_PARAVIRT`, force `!CONFIG\_RISCV\_SBI`, force `!CONFIG\_RISCV\_M\_MODE` (or alternatively set it — it doesn't matter since you have no privilege at all), force `!CONFIG\_MMU`.

\- `arch/riscv/kernel/asm-offsets.c`: add offsets for `pv\_cpu\_state`.



\*\*uClibc-ng patches:\*\*

\- `libc/sysdeps/linux/riscv64/sysdep.h`: replace `scall` with `call \_\_pv\_syscall\_gateway` in `PSEUDO` (2-line change) and in `internal\_syscallN` (6 macros, one-line each).

\- `libc/sysdeps/linux/riscv64/clone.S`, `vfork.S`, `nptl-sysdep.S` if present: same substitution.

\- `libc/sysdeps/linux/riscv64/sysdep.h`: also patch the `\_\_riscv\_flush\_icache` syscall stub.



\*\*Buildroot patches:\*\* a `linux.config.fragment` that selects the new config and a `uclibc.config.fragment` that turns on the paravirt mode.



\*\*Emulator side:\*\*

\- Define a fixed kernel-image load address (e.g. `0x80000000`).

\- Provide at boot time the address of `\_\_pv\_syscall\_gateway` to userspace either via:

&#x20; - A fixed memory location (write the address into a known page at `0x80000000 + offset` that uClibc-ng reads at startup), OR

&#x20; - Link uClibc-ng's startup glue with the kernel symbol (cleaner if your toolchain supports it).

\- Implement an instruction-boundary callback for periodic timer "interrupts" that calls `\_\_pv\_irq\_entry` — the emulator pushes the current PC as `ra`, jumps to the entry, and returns when the kernel `ret`s.



\## 9. Risk list



1\. \*\*CSR audit completeness\*\* — you'll find CSR sites you didn't expect. Mitigation: build with `-march=rv32i` (no Zicsr) and let the assembler reject every CSR instruction. Drive errors to zero.

2\. \*\*`ecall` audit completeness\*\* — same idea: treat any executed `ecall` as fatal in the emulator during bring-up.

3\. \*\*Atomics on RV32I-only\*\* — kernel will not link cleanly without atomics. May require backporting/forward-porting a `CONFIG\_GENERIC\_ATOMIC32` shim or compiling with `-march=rv32i\_zaamo`-style hacks. \*\*Highest unknown.\*\*

4\. \*\*Signal frame trampoline encoding\*\* — must produce exactly the right instruction bytes; bug-prone. Mitigation: keep a test that builds the sequence, dumps it via `objdump`, and asserts on the encoding.

5\. \*\*Preemption model\*\* — cooperative is much simpler; only attempt injected IRQs if you actually need preemptive scheduling. BusyBox shell does not.

6\. \*\*uClibc-ng version churn\*\* — sysdep.h is occasionally restructured. Pin a uClibc-ng version (1.0.x is the current stable line, see `uclibc-ng.org`).

7\. \*\*Kernel forward porting\*\* — `entry.S` changes shape every few releases (e.g., shadow-call-stack additions in 2024). Pin a kernel version. The K210-era 5.10–5.15 are good prototyping targets because they predate most CFI/shadow-stack churn.

8\. \*\*Tooling: `elf2flt` for RISC-V\*\* — still not mainlined. Use `github.com/floatious/elf2flt/tree/riscv` per the Niklas Cassel buildroot tree (`github.com/floatious/buildroot/tree/k210-v14`).

9\. \*\*Floating point / soft-fp\*\* — RV32I has no F/D. uClibc-ng and BusyBox must be built with soft-float (`-mabi=ilp32`, not `ilp32f`). Already standard for the K210 nommu config.



\## 10. Recommended first experiment



Before touching `entry.S`, \*\*prove the ABI path end-to-end on the existing NOMMU kernel\*\* using the existing `ecall` instruction:



1\. Build the standard `nommu\_virt\_defconfig` (or `nommu\_k210\_defconfig`) kernel for RV32 from the K210 buildroot tree. Confirm it boots BusyBox in QEMU.

2\. Patch uClibc-ng's `sysdep.h` to use `jal ra, %pcrel(\_\_pv\_syscall\_gateway)` instead of `scall`. In the kernel, define a kernel symbol `\_\_pv\_syscall\_gateway` whose body is exactly a small wrapper that calls `syscall\_handler` with a synthesized `pt\_regs`. Build, run \*in QEMU with full S/U support\*, and confirm BusyBox still works. \*\*This validates the entire ABI substitution while still running on stock hardware semantics\*\*, so you can debug the gateway in isolation.

3\. Once step 2 works, port the rt\_sigreturn trampoline to the same gateway (test by sending signals — `kill -HUP $$`, etc.).

4\. Once steps 2–3 are solid on real QEMU, port the kernel away from `sret`/`csr\*` and run it on your RV32I-only emulator. By then your kernel-side gateway logic is debugged and only the privilege-removal changes are new.



This staged approach minimizes the matrix of unknowns: at each step, only one thing is different from a known-good baseline.



\---



\## Selected references



Kernel source (primary):

\- `arch/riscv/kernel/entry.S` — `handle\_exception`, `ret\_from\_exception`, `\_\_switch\_to`. (`github.com/torvalds/linux`)

\- `arch/riscv/kernel/signal.c` — `setup\_rt\_frame`, `\_\_user\_rt\_sigreturn\[2]`.

\- `arch/riscv/kernel/traps.c` — `do\_trap\_ecall\_u`, `syscall\_handler`.

\- `arch/riscv/configs/nommu\_virt\_defconfig`, `nommu\_k210\_defconfig`.

\- `fs/binfmt\_flat.c` — BFLT loader; recent RV-relevant fix in 2022.



RISC-V NOMMU upstreaming history:

\- Plumbers 2019 slides, Damien Le Moal: `lpc.events/event/4/contributions/386/attachments/298/502/RISC-V-NOMMU-Linux-Plumbers-2019.pdf`

\- Hellwig nommu series: `lore.kernel.org/linux-mm/e5827553-0924-28ee-3c8a-d29b4c01defd@arm.com/T/`

\- NOMMU sigreturn icache fix: `lore.kernel.org/r/20230406101130.82304-1-mathis.salmen@matsal.de`



Prior art:

\- UKL (Unikernel Linux), Raza et al., arXiv 2206.00789 — the closest conceptual analog, plus its LKML RFC at `lkml.iu.edu/hypermail/linux/kernel/2210.0/01939.html` and LWN coverage at `lwn.net/Articles/910303/`.

\- Kernel Mode Linux (Maeda 2003), `web.yl.is.s.u-tokyo.ac.jp/\~tosh/kml/` and Linux Journal article `linuxjournal.com/article/6516`.

\- UML SKAS0 internals, LWN: `lwn.net/Articles/142494/`. UML HOWTO: `kernel.org/doc/html/v5.9/virt/uml/user\_mode\_linux.html`.

\- LKL: `github.com/lkl/linux`, LWN `lwn.net/Articles/662953/`, and the LKL→UML unification effort `lwn.net/Articles/804177/`.



uClibc-ng:

\- Source tree: `cgit.uclibc-ng.org/cgi/cgit/uclibc-ng.git/tree/libc/sysdeps/linux/riscv64`. `sysdep.h` has the `PSEUDO`/`scall` macro.

\- Project home: `uclibc-ng.org`.



RISC-V entry/exit pedagogy:

\- SiFive "All Aboard, Part 7": `sifive.com/blog/all-aboard-part-7-entering-and-exiting-the-linux-kernel-on-risc-v`.

\- xv6-riscv-book ch. 4 (`github.com/mit-pdos/xv6-riscv-book/blob/xv6-riscv/trap.tex`).



Toolchain:

\- elf2flt for RISC-V: `github.com/floatious/elf2flt/tree/riscv`.

\- Buildroot K210 NOMMU: `github.com/floatious/buildroot/tree/k210-v14`.



Other RISC-V emulators worth a look as syscall-interception precedent:

\- libriscv (`github.com/libriscv/libriscv`) — a userspace-emulator that intercepts Linux syscalls in C++; conceptually similar to what your gateway is doing on the kernel side, just the inverse direction.

