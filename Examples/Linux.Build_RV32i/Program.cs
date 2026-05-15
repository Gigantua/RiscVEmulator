// Examples.Linux.Build_RV32i — drives WSL to build a network-enabled mini-rv32ima Linux
// image via buildroot. Outputs to ~/.cache/riscvemu/linux/Image-net and
// ~/.cache/riscvemu/linux/rvemu-net.dtb. The Examples.Linux project picks
// these up automatically if present.
//
// Usage:
//   dotnet run --project Examples\Linux.Build_RV32i                     # default: full build
//   dotnet run --project Examples\Linux.Build_RV32i -- --skip-apt       # skip apt-get install
//   dotnet run --project Examples\Linux.Build_RV32i -- --clean          # nuke + reclone buildroot
//   dotnet run --project Examples\Linux.Build_RV32i -- --apt pkg1,pkg2  # extra apt packages

using System.Diagnostics;
using System.Text;

const string BuildrootBranch = "2024.05.x";
const string BuildrootRepo   = "https://gitlab.com/buildroot.org/buildroot.git";
const string DefaultWslDir   = "$HOME/rvemu-buildroot";

// ── Args ─────────────────────────────────────────────────────────────────

bool    skipApt       = false;
bool    clean         = false;
string  wslDir        = DefaultWslDir;
string  distro        = "";
string? sudoPassword  = null;     // --sudo-password skips the interactive prompt
int     jobs          = 32;
var     extraApt      = new List<string>();

for (int i = 0; i < args.Length; i++)
{
    switch (args[i])
    {
        case "--skip-apt":       skipApt      = true; break;
        case "--clean":          clean        = true; break;
        case "--buildroot-dir":  wslDir       = args[++i]; break;
        case "--distro":         distro       = args[++i]; break;
        case "--apt":            extraApt.AddRange(args[++i].Split(',', StringSplitOptions.RemoveEmptyEntries)); break;
        case "--sudo-password":  sudoPassword = args[++i]; break;
        case "-j":
        case "--jobs":           jobs         = int.Parse(args[++i]); break;
        case "-h": case "--help": PrintUsage(); return 0;
        default:
            Console.Error.WriteLine($"Unknown option: {args[i]}");
            PrintUsage();
            return 1;
    }
}

// ── 1. Verify WSL is installed ───────────────────────────────────────────

if (!HasWsl())
{
    Console.Error.WriteLine("WSL is not installed. Install Ubuntu first:");
    Console.Error.WriteLine("    wsl --install -d Ubuntu");
    return 1;
}
Console.WriteLine($"WSL detected (distro: {(string.IsNullOrEmpty(distro) ? "default" : distro)})");

// ── 2. apt install build dependencies ───────────────────────────────────

var aptPkgs = new List<string>
{
    "git", "build-essential", "libncurses-dev", "libssl-dev", "bc", "bison",
    "flex", "unzip", "rsync", "wget", "cpio", "file", "python3",
    "device-tree-compiler", "ca-certificates",
    // For unpacking msys2 .pkg.tar.zst archives (libslirp Windows DLLs).
    "zstd",
};
aptPkgs.AddRange(extraApt);

if (!skipApt)
{
    Console.WriteLine($"Installing {aptPkgs.Count} apt packages...");

    // Resolve sudo credential in this order: --sudo-password arg, passwordless
    // sudo, then interactive prompt. The chosen password (if any) is then piped
    // via stdin to every `sudo -S` call below.
    if (sudoPassword == null && RunWslSilent("sudo -n true") != 0)
    {
        Console.WriteLine("WSL sudo requires a password.");
        sudoPassword = ReadPassword("Enter WSL user password: ");
    }
    if (sudoPassword != null && RunWslSudo("true", sudoPassword) != 0)
    {
        Console.Error.WriteLine("Password rejected by sudo. Aborting.");
        return 1;
    }

    int rc = RunWslSudo("apt-get update", sudoPassword);
    if (rc != 0) return rc;
    rc = RunWslSudo($"apt-get install -y --no-install-recommends {string.Join(' ', aptPkgs)}", sudoPassword);
    if (rc != 0) return rc;
}
else Console.WriteLine("Skipping apt install (--skip-apt).");

// ── 3. Reuse existing buildroot; download cnlohr's kernel config + DTS ──
// cnlohr's "secret sauce" is just two data files (no buildroot patches, no
// kernel patches). We curl them into the buildroot we already have, then
// apply our own networking + PLIC overlays on top.

if (clean)
{
    Console.WriteLine($"Removing {wslDir}...");
    RunWsl($"rm -rf {wslDir}");
}

bool hasBuildroot = RunWslSilent($"test -f {wslDir}/Makefile") == 0;
if (!hasBuildroot)
{
    Console.WriteLine($"Cloning buildroot {BuildrootBranch} to {wslDir} (shallow)...");
    int rc = RunWsl($"git clone --depth 1 --branch {BuildrootBranch} {BuildrootRepo} {wslDir}");
    if (rc != 0) return rc;
}
else Console.WriteLine($"Reusing existing buildroot at {wslDir}");

// Vendor cnlohr's two data files into board/rvemu/.
const string CnlohrRawBase = "https://raw.githubusercontent.com/cnlohr/mini-rv32ima/master/configs";
Console.WriteLine("Fetching cnlohr's kernel config + DTS from mini-rv32ima...");
RunWsl($"mkdir -p {wslDir}/board/rvemu");
RunWsl($"wget -q -O {wslDir}/board/rvemu/cnlohr-kernel.config {CnlohrRawBase}/custom_kernel_config");
RunWsl($"wget -q -O {wslDir}/board/rvemu/cnlohr-minimal.dts   {CnlohrRawBase}/minimal.dts");

// ── 4. Patch cnlohr's kernel config to enable networking + PLIC ─────────

string[] kernelEnable =
{
    "CONFIG_NET", "CONFIG_PACKET", "CONFIG_UNIX", "CONFIG_INET",
    "CONFIG_IP_MULTICAST", "CONFIG_IP_PNP", "CONFIG_IP_PNP_DHCP",
    "CONFIG_INET_RAW", "CONFIG_NETDEVICES", "CONFIG_NET_CORE",
    "CONFIG_VIRTIO", "CONFIG_VIRTIO_MENU", "CONFIG_VIRTIO_MMIO", "CONFIG_VIRTIO_NET",
    "CONFIG_HW_RANDOM_VIRTIO",
    // PLIC routes virtio-net IRQs into the CPU's external-interrupt line.
    "CONFIG_RISCV_INTC", "CONFIG_SIFIVE_PLIC",
    // Real 8250 UART driver — cnlohr's config uses HVC console, but our emulator
    // implements ns16550a. Without this the kernel stays on earlycon only and
    // goes silent once bootconsole is unregistered.
    "CONFIG_SERIAL_8250", "CONFIG_SERIAL_8250_CONSOLE",
    "CONFIG_SERIAL_OF_PLATFORM", "CONFIG_SERIAL_EARLYCON",

    // ── Framebuffer / display ───────────────────────────────────────────────
    // simple-framebuffer driver binds to a DT node pointing at our
    // FramebufferDevice's MMIO slice and exposes it as /dev/fb0. We
    // deliberately do NOT enable CONFIG_FRAMEBUFFER_CONSOLE — fbcon would
    // take over the kernel console and silence ttyS0 mid-boot.
    "CONFIG_FB", "CONFIG_FB_SIMPLE", "CONFIG_FB_NOTIFY",

    // ── Input subsystem (keyboard + mouse) ──────────────────────────────────
    // CONFIG_INPUT_UINPUT lets the rvemu-input userspace daemon (overlay)
    // synthesize input events read from our MMIO peripherals; CONFIG_INPUT_EVDEV
    // exposes them as /dev/input/eventN.
    "CONFIG_INPUT", "CONFIG_INPUT_EVDEV", "CONFIG_INPUT_KEYBOARD",
    "CONFIG_INPUT_MOUSEDEV", "CONFIG_INPUT_MISC",
    "CONFIG_INPUT_UINPUT",

    // /dev/mem so rvemu-input can mmap the MMIO regions for kbd/mouse and
    // userspace can mmap the framebuffer at 0x85FC0000 directly. STRICT_DEVMEM
    // would block userspace from touching anything inside RAM (including our
    // reserved FB) — explicit `# is not set` line keeps it off (`olddefconfig`
    // re-enables it otherwise).
    "CONFIG_DEVMEM",

    // SysV IPC: shmget/shmat needed for nano-X's BUFFER_MMAP path so
    // Microwindows clients (doomgeneric in particular) can write pixels
    // directly to a shared backing buffer instead of pushing 256 KB
    // through the unix socket per frame.
    "CONFIG_SYSVIPC", "CONFIG_POSIX_MQUEUE",

    // ── Sound + MIDI ────────────────────────────────────────────────────────
    // ALSA core + raw MIDI. CONFIG_SND_ALOOP is the in-tree loopback driver
    // — it registers a real ALSA card "Loopback" whose pcmC0D0p playback
    // substream forwards into pcmC0D1c capture. The rvemu-audiod daemon
    // reads from the capture side and forwards PCM frames to the host
    // AudioBufferDevice at 0x30000000 via /dev/mem. This is the audio
    // equivalent of rvemu-input bridging MMIO kbd/mouse to /dev/uinput.
    "CONFIG_SOUND", "CONFIG_SND", "CONFIG_SND_PCM", "CONFIG_SND_TIMER",
    "CONFIG_SND_RAWMIDI", "CONFIG_SND_SEQUENCER",
    "CONFIG_SND_VIRTIO",
    "CONFIG_SND_DRIVERS", "CONFIG_SND_ALOOP",
    // CONFIG_SND_VIRMIDI: virtual rawmidi card. Built-in (=y) auto-creates
    // one card at boot with 4 rawmidi devices (midiC1D0..D3). Each device
    // loopbacks writes→reads via the alsa-sequencer subsystem, so anything
    // an app sends to /dev/snd/midiC1D0 (with O_WRONLY) is received by the
    // rvemu-midid daemon reading the same device O_RDONLY. The daemon
    // forwards each short message to the host MidiDevice MMIO at 0x10005000.
    // CONFIG_SND_SEQ_VIRMIDI is auto-selected by SEQUENCER+VIRMIDI.
    "CONFIG_SND_VIRMIDI",
};
Console.WriteLine($"Patching cnlohr-kernel.config (+{kernelEnable.Length} symbols)...");
foreach (var sym in kernelEnable)
{
    RunWsl($"sed -i 's|^# {sym} is not set$|{sym}=y|' {wslDir}/board/rvemu/cnlohr-kernel.config");
    RunWslSilent($"grep -q '^{sym}=' {wslDir}/board/rvemu/cnlohr-kernel.config || " +
                 $"echo '{sym}=y' >> {wslDir}/board/rvemu/cnlohr-kernel.config");
}

// Explicitly DISABLE CONFIG_STRICT_DEVMEM — our compositor mmap's /dev/mem
// at 0x85FC0000 (inside RAM, marked /reserved-memory), and STRICT_DEVMEM
// blocks all RAM-range /dev/mem access. Symptom when left on: kernel WARN
// at kernel/workqueue.c when the FB process accesses the mapping, then
// devtmpfs corruption ("can't open /dev/console").
string[] kernelDisable = { "CONFIG_STRICT_DEVMEM", "CONFIG_IO_STRICT_DEVMEM" };
foreach (var sym in kernelDisable)
{
    RunWsl($"sed -i 's|^{sym}=.*|# {sym} is not set|' {wslDir}/board/rvemu/cnlohr-kernel.config");
    RunWslSilent($"grep -q '{sym}' {wslDir}/board/rvemu/cnlohr-kernel.config || " +
                 $"echo '# {sym} is not set' >> {wslDir}/board/rvemu/cnlohr-kernel.config");
}

// ── 5. Patch cnlohr's DTS with PLIC + virtio-mmio nodes ─────────────────
// Uses cnlohr's 64-bit address/size cell convention: <hi lo hi lo>.

// Device tree for the host-emulated peripherals.
//
// IRQ map on the PLIC (phandle 0x05):
//   1  virtio-net          (always registered when --net, see Examples/Linux)
//   2  virtio-input kbd    (dormant until C# VirtioInputDevice exists)
//   3  virtio-input mouse  (dormant until C# VirtioInputDevice exists)
//   4  virtio-snd          (dormant until C# VirtioSndDevice exists)
//
// MMIO map:
//   0x10008000  virtio-net
//   0x10009000  virtio-input kbd
//   0x1000A000  virtio-input mouse
//   0x1000B000  virtio-snd
//   0x20000000  framebuffer (320×200 RGBA = 256000 bytes, plain RAM page)
//
// Nodes with no backing C# peripheral are harmless: virtio's first MMIO read
// is a magic value (0x74726976). Our MmioDispatcher returns 0 for unmapped
// addresses, so the kernel sees no virtio device and skips the slot.
//
// simple-framebuffer is different — the kernel maps the region into /dev/fb0
// regardless. FramebufferDevice IS always wired up in Examples/Linux so this
// memory is real and read/writable.

const string dtsExtraNodes = """

		plic: interrupt-controller@c000000 {
			compatible          = "sifive,plic-1.0.0", "riscv,plic0";
			#interrupt-cells    = <0x01>;
			interrupt-controller;
			reg                 = <0x00 0x0c000000 0x00 0x4000000>;
			interrupts-extended = <0x02 0x0b 0x02 0x09>;
			riscv,ndev          = <0x1f>;
			phandle             = <0x05>;
		};

		virtio_mmio@10008000 {
			compatible       = "virtio,mmio";
			reg              = <0x00 0x10008000 0x00 0x1000>;
			interrupt-parent = <0x05>;
			interrupts       = <0x01>;
		};

		/* simple-framebuffer is NOT placed here — sibling of /memory@80000000
		 * would let the OF early-init treat its reg as a memory bank, pulling
		 * ARCH_PFN_OFFSET down to 0x20000 and walking 393k pages of "hole"
		 * via init_unavailable_range() (each one memset'd → effectively
		 * forever at 50 MIPS). Instead it goes under /chosen — that subtree
		 * is special-cased by of_get_compatible_child(of_chosen,...) and
		 * doesn't participate in memblock. See the /chosen injection below. */

		/* Reserved DT slots for kbd/mouse/audio/midi — not emitted until
		 * the matching in-tree Linux drivers are wired up (their compatible
		 * strings will be "rvemu,keyboard", "rvemu,mouse", etc., binding
		 * to the existing C# MMIO peripherals at:
		 *   0x10001000 keyboard
		 *   0x10002000 mouse
		 *   0x10005000 MIDI         (bridged via snd-virmidi + rvemu-midid)
		 *   0x30000000 audio PCM
		 *   0x30100000 audio control                                      */
""";

Console.WriteLine("Injecting PLIC + virtio-mmio into the DTS...");
string b64Nodes = Convert.ToBase64String(System.Text.Encoding.UTF8.GetBytes(dtsExtraNodes));
RunWsl($"python3 -c \"" +
       $"import base64;" +
       $"p='{wslDir}/board/rvemu/cnlohr-minimal.dts';" +
       $"s=open(p).read();" +
       $"frag=base64.b64decode('{b64Nodes}').decode();" +
       $"i=s.rfind('}};', 0, s.rfind('}};'));" +
       $"_=open(p,'w').write(s[:i]+frag+s[i:]) if 'virtio_mmio' not in s else None\"");

// Fix cnlohr's malformed CLINT compatible: '"sifive,clint0,riscv,clint0"'
// is a single string with commas inside; Linux 6.6 wants two strings.
// Switch the UART compatible from "hvc" (cnlohr's HVC console) to "ns16550a"
// (what our emulator implements). Replace cnlohr's hardcoded RAM size with
// the magic guard 0x00C0FF03 so Examples.Linux can patch it at runtime
// based on --ram.
RunWsl($"sed -i 's|\"sifive,clint0,riscv,clint0\"|\"sifive,clint0\", \"riscv,clint0\"|;" +
       $"      s|compatible = \"hvc\";|compatible = \"ns16550a\";|;" +
       $"      s|0x3ffc000|0x00C0FF03|' " +
       $"{wslDir}/board/rvemu/cnlohr-minimal.dts");

// Inject /chosen/rng-seed so Linux's CRNG inits at boot (time ~0s) instead
// of after several minutes of guest time. Linux 5.10+ consumes this property
// during early init and immediately marks the CRNG seeded. The kernel
// scrubs the value from memory after reading it, so a static-per-build seed
// is acceptable for an emulator demo — every Prepare run generates a fresh
// 64-byte seed from the host's /dev/urandom.
Console.WriteLine("Injecting /chosen/rng-seed (CRNG no-wait)...");
RunWsl($"python3 -c \"" +
       $"import os, re;" +
       $"p='{wslDir}/board/rvemu/cnlohr-minimal.dts';" +
       $"s=open(p).read();" +
       $"seed=' '.join('%02x'%b for b in os.urandom(64));" +
       $"frag='rng-seed = [' + seed + '];\\n\\t\\t';" +
       $"s2=re.sub(r'(\\s*)bootargs = ', r'\\1' + frag + 'bootargs = ', s, count=1) if 'rng-seed' not in s else s;" +
       $"open(p,'w').write(s2)\"");

// Not injecting any simple-framebuffer DT node.
//
// We tried workaround #1 (FB inside RAM, /reserved-memory + /chosen/framebuffer)
// in May 2026 and it STILL hangs the kernel before any printk. Same PCs in
// __memset / init_unavailable_range as the out-of-RAM case. The trigger is
// surprisingly fragile: a "minimal" /chosen/framebuffer (compatible+reg
// only) boots, but adding ANY one of width/height/stride/format kicks the
// kernel back into the slow walk — even with FB inside the RAM range and
// /reserved-memory carving it out. The kernel apparently still ends up
// treating the FB region as a separate memblock entry through some path
// we haven't fully pinpointed.
//
// Active strategy: workaround #3 — guest userspace mmaps /dev/mem at
// 0x85FC0000 directly (Doom's bare-metal path). CONFIG_DEVMEM=y is on.
// FramebufferDevice is still registered (Examples.Linux), so the address
// is real RAM. A future Microwindows port that uses /dev/mem instead of
// /dev/fb0 would render here directly.

// ── 6. Configure buildroot to use cnlohr's kernel config ────────────────

Console.WriteLine("Running defconfig + wiring kernel config...");
int dcfg = RunWsl($"cd {wslDir} && make qemu_riscv32_nommu_virt_defconfig");
if (dcfg != 0) return dcfg;

// Force buildroot to use cnlohr's custom kernel .config (and the version
// it targets, Linux 6.8). Also embed rootfs as initramfs (no virtio-blk
// in our emulator).
RunWsl($"sed -i '/^BR2_LINUX_KERNEL_CUSTOM_VERSION/d;" +
          "/^BR2_LINUX_KERNEL_VERSION/d;" +
          "/^BR2_LINUX_KERNEL_DEFCONFIG/d;" +
          "/^BR2_LINUX_KERNEL_USE_DEFCONFIG/d;" +
          "/^# *BR2_LINUX_KERNEL_USE_CUSTOM_CONFIG/d;" +
          "/^BR2_LINUX_KERNEL_USE_CUSTOM_CONFIG/d;" +
          "/^BR2_LINUX_KERNEL_CUSTOM_CONFIG_FILE/d;" +
          "/^BR2_LINUX_KERNEL_CONFIG_FRAGMENT_FILES/d;" +
          "/^# *BR2_TARGET_ROOTFS_INITRAMFS/d;" +
          "/^BR2_TARGET_ROOTFS_INITRAMFS=/d;" +
          "/^BR2_TARGET_ROOTFS_EXT2=/d;" +
          "/^BR2_TARGET_ROOTFS_TAR=/d;" +
          // Toolchain ISA: match cnlohr (RV32IM, ILP32 ABI, uclibc).
          "/^BR2_riscv_g=/d;" +
          "/^BR2_riscv_custom=/d;" +
          "/^BR2_RISCV_ISA_/d;" +
          "/^BR2_RISCV_ABI_/d' {wslDir}/.config");
// Use whatever Linux version buildroot 2024.05.x defaults to (6.6.18) —
// it ships pre-validated hashes. cnlohr's kernel config was generated against
// 6.8-rc1, but olddefconfig silently drops symbols missing in 6.6.18.
RunWsl($"cat >> {wslDir}/.config << 'BR_CFG_EOF'\n" +
       "BR2_LINUX_KERNEL_USE_CUSTOM_CONFIG=y\n" +
       "BR2_LINUX_KERNEL_CUSTOM_CONFIG_FILE=\"board/rvemu/cnlohr-kernel.config\"\n" +
       "BR2_TARGET_ROOTFS_INITRAMFS=y\n" +
       "# BR2_TARGET_ROOTFS_EXT2 is not set\n" +
       "# BR2_TARGET_ROOTFS_TAR is not set\n" +
       "BR2_riscv_custom=y\n" +
       "BR2_RISCV_ISA_RVI=y\n" +
       "BR2_RISCV_ISA_RVM=y\n" +
       // A-extension (LR.W/SC.W/AMO*). Our emulator implements it fully
       // (see CLAUDE.md ISA support table). Required by uClibc-ng's
       // libpthread/linuxthreads/sysdeps/riscv32/pt-machine.h once WCHAR
       // + LOCALE are enabled, because those pull in pthread atomic
       // primitives that use `lr.w` / `sc.w` directly.
       "BR2_RISCV_ISA_RVA=y\n" +
       "# BR2_RISCV_ISA_RVF is not set\n" +
       "# BR2_RISCV_ISA_RVD is not set\n" +
       "# BR2_RISCV_ISA_RVC is not set\n" +
       "BR2_RISCV_ABI_ILP32=y\n" +
       "# BR2_RISCV_ABI_ILP32D is not set\n" +
       "BR2_ROOTFS_OVERLAY=\"board/rvemu/rootfs-overlay\"\n" +
       // Real opkg needs MMU + wchar. Our nommu uclibc target can't run it.
       // Instead the overlay ships an `rvpkg` shell script that uses busybox
       // ar+tar+wget to install .ipks from Examples.Linux.Packageserver.
       "BR2_PACKAGE_BUSYBOX_CONFIG_FRAGMENT_FILES=\"board/rvemu/busybox.fragment\"\n" +
       // ── Toolchain capabilities ──────────────────────────────────────
       // uClibc-ng wchar + locale. Required by gnulib-using packages
       // (nano, sed, grep, gawk, ...) which reference wctype/iswctype/
       // wctomb. Without these the package server's cross-build fails
       // at link time with "undefined reference to `wctype'" etc.
       // Adds ~250 KB to libc.a, but transitively unlocks most of
       // buildroot's package set for the Packageserver feed.
       "BR2_TOOLCHAIN_BUILDROOT_WCHAR=y\n" +
       "BR2_TOOLCHAIN_BUILDROOT_LOCALE=y\n" +
       "BR2_ENABLE_LOCALE=y\n" +
       // Keep only C/POSIX + a couple of common UTF-8 locales — full set
       // bloats the rootfs by several MB and we don't need it.
       "BR2_ENABLE_LOCALE_WHITELIST=\"C en_US en_US.UTF-8\"\n" +
       // Bake doom-puredoom (+ its WAD dep) directly into the base image so
       // the Doom button shows up on the taskbar on first boot, no rvpkg
       // install needed. Costs ~5 MB of rootfs (binary + shareware WAD).
       // PureDOOM has built-in PCM mixing + MIDI emission — same path as
       // Examples.Doom bare-metal demo, so SFX + music work out of the box.
       "BR2_PACKAGE_DOOM_PUREDOOM=y\n" +
       "BR2_PACKAGE_DOOM_WAD=y\n" +
       "BR_CFG_EOF");

// ── 6b. Rootfs overlay: drop in an auto-DHCP init script ────────────────
// Buildroot copies the overlay tree on top of the staged rootfs right
// before packing the initramfs. We add /etc/init.d/S41dhcp which tries
// to bring up eth0 via DHCP non-blockingly on boot. The script swallows
// all errors and exits 0, so any failure (no eth0, no slirp_bridge, etc.)
// can never block boot — the kernel still reaches the login prompt.
Console.WriteLine("Writing rootfs overlay (auto-DHCP init script)...");
RunWsl($"mkdir -p {wslDir}/board/rvemu/rootfs-overlay/etc/init.d");
RunWsl($"cat > {wslDir}/board/rvemu/rootfs-overlay/etc/init.d/S41dhcp << 'DHCP_EOF'\n" +
       "#!/bin/sh\n" +
       "# Best-effort DHCP on eth0. Never blocks boot — any failure exits 0.\n" +
       "case \"$1\" in\n" +
       "    start)\n" +
       "        [ -d /sys/class/net/eth0 ] || exit 0\n" +
       "        echo -n 'Bringing up eth0 (DHCP via libslirp)... '\n" +
       "        ip link set eth0 up 2>/dev/null\n" +
       "        # -q: quit after lease, -n: exit if no lease (don't hang).\n" +
       "        # -t 5 -T 2: 5 retries with 2s gap = 10s max wait.\n" +
       "        if udhcpc -i eth0 -q -n -t 5 -T 2 >/dev/null 2>&1; then\n" +
       "            echo 'OK'\n" +
       "        else\n" +
       "            echo 'no lease (continuing without network)'\n" +
       "        fi\n" +
       "        ;;\n" +
       "    stop)\n" +
       "        killall udhcpc 2>/dev/null\n" +
       "        ;;\n" +
       "esac\n" +
       "exit 0\n" +
       "DHCP_EOF");
RunWsl($"chmod 0755 {wslDir}/board/rvemu/rootfs-overlay/etc/init.d/S41dhcp");

// Autologin on the serial console — drop the getty/login dance, give us
// a root shell directly. It's a single-user emulator, not a mainframe.
// Replace the GENERIC_SERIAL getty line with a direct -/bin/sh respawn.
Console.WriteLine("Writing inittab override (autologin root on console)...");
RunWsl($"mkdir -p {wslDir}/board/rvemu/rootfs-overlay/etc");
RunWsl($"cat > {wslDir}/board/rvemu/rootfs-overlay/etc/inittab << 'INIT_EOF'\n" +
       "::sysinit:/bin/mount -t proc proc /proc\n" +
       "::sysinit:/bin/mount -o remount,rw /\n" +
       "::sysinit:/bin/mkdir -p /dev/pts /dev/shm\n" +
       "::sysinit:/bin/mount -a\n" +
       "::sysinit:/bin/mkdir -p /run/lock/subsys\n" +
       "null::sysinit:/bin/ln -sf /proc/self/fd /dev/fd\n" +
       "null::sysinit:/bin/ln -sf /proc/self/fd/0 /dev/stdin\n" +
       "null::sysinit:/bin/ln -sf /proc/self/fd/1 /dev/stdout\n" +
       "null::sysinit:/bin/ln -sf /proc/self/fd/2 /dev/stderr\n" +
       "::sysinit:/bin/hostname -F /etc/hostname\n" +
       "::sysinit:/etc/init.d/rcS\n" +
       // Autologin: just exec a root shell on the console. No getty,
       // no login prompt, no password. -/bin/sh makes it a login shell
       // so /etc/profile is sourced.
       "console::respawn:-/bin/sh\n" +
       "::shutdown:/etc/init.d/rcK\n" +
       "::shutdown:/bin/umount -a -r\n" +
       "INIT_EOF");

// busybox config fragment — enable the `ar` and `tar` applets so the rvpkg
// install script can crack open the .ipk archives served by the host.
Console.WriteLine("Writing busybox fragment (enable ar+tar applets)...");
RunWsl($"cat > {wslDir}/board/rvemu/busybox.fragment << 'BB_EOF'\n" +
       "CONFIG_AR=y\n" +
       "CONFIG_FEATURE_AR_LONG_FILENAMES=y\n" +
       "CONFIG_FEATURE_AR_CREATE=y\n" +
       "CONFIG_TAR=y\n" +
       "CONFIG_FEATURE_TAR_CREATE=y\n" +
       "CONFIG_FEATURE_TAR_AUTODETECT=y\n" +
       "CONFIG_FEATURE_TAR_FROM=y\n" +
       "CONFIG_FEATURE_TAR_GNU_EXTENSIONS=y\n" +
       "CONFIG_FEATURE_TAR_LONG_OPTIONS=y\n" +
       "CONFIG_FEATURE_SEAMLESS_GZ=y\n" +
       // tar -xzf in busybox shells out to `gunzip` even with SEAMLESS_GZ on,
       // so we need the gunzip applet too.
       "CONFIG_GUNZIP=y\n" +
       "CONFIG_GZIP=y\n" +
       "CONFIG_FEATURE_GZIP_DECOMPRESS=y\n" +
       // Process/system inspection — useful in the on-guest terminal.
       "CONFIG_TOP=y\n" +
       "CONFIG_FEATURE_TOP_INTERACTIVE=y\n" +
       "CONFIG_FEATURE_TOP_CPU_USAGE_PERCENTAGE=y\n" +
       "CONFIG_FEATURE_TOP_CPU_GLOBAL_PERCENTS=y\n" +
       "CONFIG_PS=y\n" +
       "CONFIG_FEATURE_PS_TIME=y\n" +
       "CONFIG_FEATURE_PS_LONG=y\n" +
       "CONFIG_FREE=y\n" +
       "CONFIG_UPTIME=y\n" +
       "CONFIG_VMSTAT=y\n" +
       "CONFIG_PIDOF=y\n" +
       "CONFIG_KILLALL=y\n" +
       // awk is built but broken in our nommu uclibc build (""Access to
       // negative field"" on `$1`). Leaving the symbols out — rvpkg uses
       // pure POSIX shell parsing instead.
       "BB_EOF");

// ── Custom buildroot package: doom-puredoom ───────────────────────────────
// Replaces the old doomgeneric package. PureDOOM is a single-header port
// with built-in PCM mixing + MIDI emission — same code path as the bare-
// metal Examples.Doom demo, so sound effects + music work via the same
// MMIO peripherals (audio @ 0x30000000, MIDI @ 0x10005004) that
// Examples.Linux already wires up.
//
// Source layout in the rvemu tree (no upstream git):
//   Examples/Linux.Build_RV32i/doom-puredoom/Config.in
//   Examples/Linux.Build_RV32i/doom-puredoom/doom-puredoom.mk
//   Examples/Linux.Build_RV32i/doom-puredoom/src/doom_linux.c
//   Examples/Linux.Build_RV32i/doom-puredoom/src/Makefile
// PureDOOM.h itself (~48k LOC) is shared with Examples.Doom and staged
// into src/ below — no duplication in the repo.
{
    string hostDoomDir = Path.GetFullPath(Path.Combine(
        AppContext.BaseDirectory, "..", "..", "..", "..", "doom-puredoom"))
        .Replace("\\", "/");
    string hostPureHdr = Path.GetFullPath(Path.Combine(
        AppContext.BaseDirectory, "..", "..", "..", "..", "..", "Doom", "Programs", "PureDOOM.h"))
        .Replace("\\", "/");
    string wslDoomDir = $"$(wslpath -u '{hostDoomDir}')";
    string wslPureHdr = $"$(wslpath -u '{hostPureHdr}')";

    Console.WriteLine("Installing doom-puredoom buildroot package...");
    RunWsl($"rm -rf {wslDir}/package/doom-puredoom && " +
           $"mkdir -p {wslDir}/package/doom-puredoom/src");
    RunWsl($"cp {wslDoomDir}/Config.in          {wslDir}/package/doom-puredoom/");
    RunWsl($"cp {wslDoomDir}/doom-puredoom.mk   {wslDir}/package/doom-puredoom/");
    RunWsl($"cp {wslDoomDir}/src/doom_linux.c   {wslDir}/package/doom-puredoom/src/");
    RunWsl($"cp {wslDoomDir}/src/Makefile       {wslDir}/package/doom-puredoom/src/");
    // PureDOOM.h is shared with Examples.Doom; stage it into the package src/.
    RunWsl($"cp {wslPureHdr}                    {wslDir}/package/doom-puredoom/src/");

    // Splice `source "package/doom-puredoom/Config.in"` into package/Config.in
    // (idempotent). Also clean up any prior `doomgeneric` Config.in pointer
    // left over from an older prepare so kconfig doesn't fail on a missing
    // include when the package dir was removed.
    RunWsl($"sed -i '/source \"package\\/doomgeneric\\/Config.in\"/d' {wslDir}/package/Config.in");
    RunWsl($"rm -rf {wslDir}/package/doomgeneric");
    RunWsl($"grep -q 'package/doom-puredoom/Config.in' {wslDir}/package/Config.in || " +
           $"sed -i '/menu \"Games\"/a\\\tsource \"package/doom-puredoom/Config.in\"' " +
           $"{wslDir}/package/Config.in");

    // Patch upstream doom-wad's Config.in so it accepts being selected by
    // our package. Upstream only allows chocolate-doom or prboom — without
    // this our `select BR2_PACKAGE_DOOM_WAD` triggers an "unmet direct
    // dependencies" warning and the WAD doesn't actually get into the
    // rootfs. Patch is idempotent: if either DOOMGENERIC or DOOM_PUREDOOM
    // is already in the list we leave it; otherwise append our flag.
    RunWsl($"grep -q 'BR2_PACKAGE_DOOM_PUREDOOM' {wslDir}/package/doom-wad/Config.in || " +
           $"sed -i 's/depends on BR2_PACKAGE_CHOCOLATE_DOOM || BR2_PACKAGE_PRBOOM\\( || BR2_PACKAGE_DOOMGENERIC\\)\\?$/" +
           $"depends on BR2_PACKAGE_CHOCOLATE_DOOM || BR2_PACKAGE_PRBOOM || BR2_PACKAGE_DOOMGENERIC || BR2_PACKAGE_DOOM_PUREDOOM/' " +
           $"{wslDir}/package/doom-wad/Config.in");

    // Stage Microwindows nano-X client lib + headers into buildroot's
    // sysroot so future nano-X apps can link. doom-puredoom itself doesn't
    // use nano-X (talks to /dev/mem directly, same as the bare-metal port)
    // but other packages still depend on the staging being present.
    RunWsl("bash /mnt/c/work/RiscV/RiscVEmulator/Examples/Linux.Build_RV32i/scripts/stage-nanox.sh");
}

// rvpkg — tiny shell-script package installer. Fetches the host feed,
// extracts the .ipk (ar + tar.gz inside) into /. No opkg dependencies.
// Transfer via base64 because `bash -c` + heredoc still expands `$VAR`
// despite the single-quoted delimiter — base64 is shell-meta-free.
Console.WriteLine("Writing /usr/bin/rvpkg (shell package installer)...");
RunWsl($"mkdir -p {wslDir}/board/rvemu/rootfs-overlay/usr/bin");
const string rvpkgScript = @"#!/bin/sh
# Minimal package installer for rvemu. Talks to Examples.Linux.Packageserver.
# Pure POSIX shell — busybox awk in our nommu+uclibc build is broken
# (""Access to negative field"" on a trivial $1 read), so we parse Packages
# with `while read` loops and ${var#prefix} parameter expansion.
FEED=""${RVPKG_FEED:-http://10.0.2.2:8080}""
LISTS=""/var/lib/rvpkg""
mkdir -p ""$LISTS""

update() {
    wget -qO ""$LISTS/Packages.new"" ""$FEED/Packages"" || { echo ""feed unreachable: $FEED""; return 1; }
    mv ""$LISTS/Packages.new"" ""$LISTS/Packages""
    echo ""$(grep -c '^Package:' $LISTS/Packages) packages available.""
}

list() {
    [ -f ""$LISTS/Packages"" ] || { echo ""run 'rvpkg update' first""; return 1; }
    pkg=""""; ver=""""
    while IFS= read -r line; do
        case ""$line"" in
            ""Package: ""*)     pkg=${line#Package: } ;;
            ""Version: ""*)     ver=${line#Version: } ;;
            ""Description: ""*) printf '%-20s %-10s %s\n' ""$pkg"" ""$ver"" ""${line#Description: }"" ;;
        esac
    done < ""$LISTS/Packages""
}

find_filename() {
    pkg=""$1""; cur=""""; fname=""""
    while IFS= read -r line; do
        case ""$line"" in
            ""Package: ""*)  cur=${line#Package: } ;;
            ""Filename: ""*) [ ""$cur"" = ""$pkg"" ] && { fname=${line#Filename: }; break; } ;;
        esac
    done < ""$LISTS/Packages""
    echo ""$fname""
}

install_pkg() {
    pkg=""$1""
    [ -z ""$pkg"" ] && { echo ""usage: rvpkg install <name>""; return 1; }
    [ -f ""$LISTS/Packages"" ] || update || return 1
    fname=$(find_filename ""$pkg"")
    [ -z ""$fname"" ] && { echo ""$pkg: not in feed""; return 1; }
    echo ""Fetching $fname...""
    cd /tmp && rm -rf rvpkg-ext && mkdir rvpkg-ext
    wget -qO ""$fname"" ""$FEED/$fname"" || { echo ""download failed""; return 1; }
    cd rvpkg-ext
    ar x ""/tmp/$fname"" || { echo ""ar failed (busybox without CONFIG_AR?)""; return 1; }
    [ -f data.tar.gz ] || { echo ""$fname: missing data.tar.gz""; return 1; }
    tar -xzf data.tar.gz -C /
    # Touch the launchers dir so rvemu-taskbar's mtime poll sees a change
    # and re-scans for new .desktop entries the package may have shipped.
    [ -d /etc/rvemu-launchers.d ] && touch /etc/rvemu-launchers.d
    echo ""$pkg installed.""
    cd / && rm -rf /tmp/rvpkg-ext /tmp/$fname
}

case ""$1"" in
    update)  update ;;
    list)    list ;;
    install) shift; install_pkg ""$1"" ;;
    *)       echo ""usage: rvpkg {update | list | install <name>}"" ;;
esac
";
string rvpkgB64 = Convert.ToBase64String(System.Text.Encoding.UTF8.GetBytes(rvpkgScript));
RunWsl($"echo {rvpkgB64} | base64 -d > {wslDir}/board/rvemu/rootfs-overlay/usr/bin/rvpkg");
RunWsl($"chmod 0755 {wslDir}/board/rvemu/rootfs-overlay/usr/bin/rvpkg");

// rvemu-input — userspace daemon that bridges our MMIO keyboard/mouse to
// the kernel input subsystem via /dev/uinput. After it's running,
// /dev/input/event0 (kbd) and event1 (mouse) appear and any standard
// fbdev/evdev-aware app sees real Linux input devices.
//
// Source lives at guest-userspace/rvemu-input.c. We cross-compile it here
// with buildroot's toolchain (which produces a statically-linked nommu
// uclibc RV32I binary) and drop the result into the overlay.
Console.WriteLine("Cross-compiling rvemu-input...");
// AppContext.BaseDirectory varies by build flavour:
//   Debug:        bin/Debug/net10.0/            → 3 levels up = project dir
//   Release:      bin/Release/net10.0/          → 3 levels up = project dir
//   Release x64:  bin/x64/Release/net10.0/      → 4 levels up = project dir
// Walk upward until we find a sibling guest-userspace/ folder so we work
// in all configurations without hard-coding the depth.
string hostSrcDir;
{
    var dir = new DirectoryInfo(AppContext.BaseDirectory);
    while (dir != null && !Directory.Exists(Path.Combine(dir.FullName, "guest-userspace")))
        dir = dir.Parent;
    if (dir == null)
        throw new DirectoryNotFoundException(
            "Could not locate guest-userspace/ above " + AppContext.BaseDirectory);
    hostSrcDir = Path.Combine(dir.FullName, "guest-userspace").Replace("\\", "/");
}
Console.WriteLine($"  source dir: {hostSrcDir}");
RunWsl($"mkdir -p {wslDir}/board/rvemu/userspace");
// Let bash convert the Windows path to /mnt/c/... at run time.
RunWsl($"cp \"$(wslpath -u '{hostSrcDir}')/rvemu-input.c\" {wslDir}/board/rvemu/userspace/");
// Buildroot's toolchain wrapper lives at output/host/bin/<triple>-gcc. The
// `-static` is required because our nommu rootfs has no dynamic linker
// listening at the standard path; everything in /usr/bin is statically linked.
// BFLT link recipe — mirrors what buildroot uses for busybox itself.
// Critical flags:
//   -fPIC               position-independent code (BFLT v4 'ram gotpic')
//   -Wl,-elf2flt=-r     emit BFLT + tell elf2flt -r (ram-relocatable)
//   -static             no shared libs (nommu has no dynamic linker)
// Without -elf2flt=-r the output is BFLT 'gotpic' (no 'ram' flag) which
// the kernel loads as a process but never reaches main() — busybox
// works because it links with =-r. See memory/project_nommu_bflt_quirks.md.
// Copy fbtest + desktop + audiotest + audiod + play source too. The shell
// loop below compiles each .c.
RunWsl($"cp \"$(wslpath -u '{hostSrcDir}')/rvemu-fbtest.c\"    {wslDir}/board/rvemu/userspace/");
RunWsl($"cp \"$(wslpath -u '{hostSrcDir}')/rvemu-desktop.c\"   {wslDir}/board/rvemu/userspace/");
RunWsl($"cp \"$(wslpath -u '{hostSrcDir}')/rvemu-audiotest.c\" {wslDir}/board/rvemu/userspace/");
RunWsl($"cp \"$(wslpath -u '{hostSrcDir}')/rvemu-audiod.c\"    {wslDir}/board/rvemu/userspace/");
RunWsl($"cp \"$(wslpath -u '{hostSrcDir}')/rvemu-play.c\"      {wslDir}/board/rvemu/userspace/");
RunWsl($"cp \"$(wslpath -u '{hostSrcDir}')/rvemu-midid.c\"     {wslDir}/board/rvemu/userspace/");
RunWsl($"cp \"$(wslpath -u '{hostSrcDir}')/rvemu-keytap.c\"    {wslDir}/board/rvemu/userspace/");
const string buildScript = @"#!/bin/sh
set -e
cd ""$1""
OVERLAY=""$2""
CC=""$HOME/rvemu-buildroot/output/host/bin/riscv32-buildroot-linux-uclibc-gcc""
FLAGS=""-static -fPIC -Wl,-elf2flt=-r -O2 -Wall -Wextra""
for src in rvemu-input.c rvemu-fbtest.c rvemu-desktop.c rvemu-audiotest.c rvemu-audiod.c rvemu-play.c rvemu-midid.c rvemu-keytap.c; do
    [ -f ""$src"" ] || continue
    out=""${src%.c}""
    echo ""  compiling $src...""
    $CC $FLAGS ""$src"" -o ""$out""
    cp ""$out"" ""$OVERLAY/usr/bin/$out""
    chmod 0755 ""$OVERLAY/usr/bin/$out""
    file ""$OVERLAY/usr/bin/$out""
done
";
string bsB64 = Convert.ToBase64String(System.Text.Encoding.UTF8.GetBytes(buildScript));
RunWsl($"echo {bsB64} | base64 -d > /tmp/rvemu-build-userspace.sh && chmod +x /tmp/rvemu-build-userspace.sh");
string ccCmd =
    $"/tmp/rvemu-build-userspace.sh " +
    $"{wslDir}/board/rvemu/userspace " +
    $"{wslDir}/board/rvemu/rootfs-overlay";
int ccRc = RunWsl(ccCmd);
if (ccRc == 0)
{
    Console.WriteLine("  rvemu-input compiled.");
    // S42input — start the daemon after networking, never block boot.
    // Base64-piped because bash -c heredocs still expand $1 / $! / $(...).
    const string s42 = @"#!/bin/sh
case ""$1"" in
    start)
        [ -e /dev/uinput ] && [ -x /usr/bin/rvemu-input ] || exit 0
        echo -n 'Starting rvemu-input (MMIO→uinput bridge)... '
        /usr/bin/rvemu-input >/dev/null 2>&1 &
        echo $! > /var/run/rvemu-input.pid
        echo 'OK'
        ;;
    stop)
        [ -f /var/run/rvemu-input.pid ] && kill $(cat /var/run/rvemu-input.pid) 2>/dev/null
        ;;
esac
exit 0
";
    string s42B64 = Convert.ToBase64String(System.Text.Encoding.UTF8.GetBytes(s42));
    RunWsl($"echo {s42B64} | base64 -d > {wslDir}/board/rvemu/rootfs-overlay/etc/init.d/S42input");
    RunWsl($"chmod 0755 {wslDir}/board/rvemu/rootfs-overlay/etc/init.d/S42input");

    // S43desktop — auto-launch the rvemu-desktop compositor right after
    // input is up. Runs detached (the host's --gui SDL window picks up
    // the framebuffer renders). Boot still reaches a normal login on
    // the serial console in parallel.
    const string s43 = @"#!/bin/sh
case ""$1"" in
    start)
        [ -x /usr/bin/rvemu-desktop ] && [ -c /dev/input/event0 ] || exit 0
        echo -n 'Starting rvemu-desktop... '
        /usr/bin/rvemu-desktop >/dev/null 2>&1 &
        echo $! > /var/run/rvemu-desktop.pid
        echo 'OK'
        ;;
    stop)
        [ -f /var/run/rvemu-desktop.pid ] && kill $(cat /var/run/rvemu-desktop.pid) 2>/dev/null
        ;;
esac
exit 0
";
    string s43B64 = Convert.ToBase64String(System.Text.Encoding.UTF8.GetBytes(s43));
    RunWsl($"echo {s43B64} | base64 -d > {wslDir}/board/rvemu/rootfs-overlay/etc/init.d/S43desktop");
    RunWsl($"chmod 0755 {wslDir}/board/rvemu/rootfs-overlay/etc/init.d/S43desktop");

    // S44audio — start the ALSA loopback → MMIO bridge after input is up.
    // Together with CONFIG_SND_ALOOP and the asound.conf below, this is what
    // makes `aplay -l` show a real card and any ALSA app route through to
    // the host SDL audio device.
    const string s44 = @"#!/bin/sh
case ""$1"" in
    start)
        [ -x /usr/bin/rvemu-audiod ] && [ -c /dev/snd/pcmC0D1c ] || exit 0
        echo -n 'Starting rvemu-audiod (snd-aloop -> MMIO bridge)... '
        /usr/bin/rvemu-audiod >/dev/null 2>&1 &
        echo $! > /var/run/rvemu-audiod.pid
        echo 'OK'
        ;;
    stop)
        [ -f /var/run/rvemu-audiod.pid ] && kill $(cat /var/run/rvemu-audiod.pid) 2>/dev/null
        ;;
esac
exit 0
";
    string s44B64 = Convert.ToBase64String(System.Text.Encoding.UTF8.GetBytes(s44));
    RunWsl($"echo {s44B64} | base64 -d > {wslDir}/board/rvemu/rootfs-overlay/etc/init.d/S44audio");
    RunWsl($"chmod 0755 {wslDir}/board/rvemu/rootfs-overlay/etc/init.d/S44audio");

    // S46midi — start the snd-virmidi → MMIO bridge after audio is up.
    // snd-virmidi (CONFIG_SND_VIRMIDI=y) creates /dev/snd/midiC1D0..D3 at
    // boot; rvemu-midid opens C1D0 read-only and forwards rawmidi bytes to
    // the host MidiDevice MMIO at 0x10005000, which calls winmm.midiOutShortMsg
    // on the Windows side (default GM synth — no extra software needed).
    const string s46 = @"#!/bin/sh
# Globbing here: snd-virmidi may land on any card index depending on
# what else loaded first. As long as ANY rawmidi device exists, start
# the daemon — rvemu-midid probes /dev/snd/midiC0D0..C7D0 itself.
case ""$1"" in
    start)
        [ -x /usr/bin/rvemu-midid ] || exit 0
        ls /dev/snd/midiC*D0 >/dev/null 2>&1 || exit 0
        echo -n 'Starting rvemu-midid (snd-virmidi -> MMIO bridge)... '
        /usr/bin/rvemu-midid >/dev/null 2>&1 &
        echo $! > /var/run/rvemu-midid.pid
        echo 'OK'
        ;;
    stop)
        [ -f /var/run/rvemu-midid.pid ] && kill $(cat /var/run/rvemu-midid.pid) 2>/dev/null
        ;;
esac
exit 0
";
    string s46B64 = Convert.ToBase64String(System.Text.Encoding.UTF8.GetBytes(s46));
    RunWsl($"echo {s46B64} | base64 -d > {wslDir}/board/rvemu/rootfs-overlay/etc/init.d/S46midi");
    RunWsl($"chmod 0755 {wslDir}/board/rvemu/rootfs-overlay/etc/init.d/S46midi");

    // /etc/asound.conf — route ALSA's `default` device through plug:
    //   * plug resamples / reformats whatever the app uses
    //   * down to fixed 44100 / S16_LE / stereo so it matches what
    //     rvemu-audiod opens on the capture side of the loopback.
    // Without this, an app that requests an unusual rate (8 kHz, 48 kHz)
    // would negotiate a different format on D0p than the daemon is reading
    // on D1c, and snd-aloop would refuse the bind.
    const string asoundConf = @"# Generated by Examples.Linux.Build_RV32i.
# Maps ALSA's `default` to the snd-aloop card so all audio flows through
# rvemu-audiod -> MMIO -> host SDL2.
pcm.!default {
    type plug
    slave {
        pcm ""hw:Loopback,0,0""
        rate 44100
        format S16_LE
        channels 2
    }
}
ctl.!default {
    type hw
    card Loopback
}
";
    string asoundB64 = Convert.ToBase64String(System.Text.Encoding.UTF8.GetBytes(asoundConf));
    RunWsl($"mkdir -p {wslDir}/board/rvemu/rootfs-overlay/etc");
    RunWsl($"echo {asoundB64} | base64 -d > {wslDir}/board/rvemu/rootfs-overlay/etc/asound.conf");
    RunWsl($"chmod 0644 {wslDir}/board/rvemu/rootfs-overlay/etc/asound.conf");

    // ── Microwindows (nano-X) ────────────────────────────────────────────────
    // If a pre-built Microwindows tree exists at ~/rvemu-mw/microwindows, ship
    // its BFLT binaries into the rootfs and replace our makeshift rvemu-desktop
    // with a real retained-mode window manager + a couple of demo clients.
    //
    // Build steps (one-time, manual):
    //   git clone https://github.com/ghaerr/microwindows.git ~/rvemu-mw/microwindows
    //   cp .../microwindows/config        ~/rvemu-mw/microwindows/src/config
    //   cp .../microwindows/scr_rvemu.c   ~/rvemu-mw/microwindows/src/drivers/
    //   ... (Arch.rules, Objects.rules tweaks — see scripts/)
    //   cd ~/rvemu-mw/microwindows/src && make
    //
    // The Microwindows binaries are BFLT executables linked against the same
    // buildroot uclibc toolchain we use here. They mmap /dev/mem at 0x85FC0000
    // (see scr_rvemu.c) so they need CONFIG_DEVMEM in the kernel (already on).
    int mwRc = RunWsl(
        "test -x $HOME/rvemu-mw/microwindows/src/bin/nano-X && " +
        "test -x $HOME/rvemu-mw/microwindows/src/bin/nanowm");
    if (mwRc == 0)
    {
        Console.WriteLine("  Microwindows detected — installing nano-X + demos.");
        string[] mwBins = { "nano-X", "nanowm", "rvemu-taskbar", "rvemu-term",
                            "nxstart",
                            "nxclock", "nxeyes", "nxchess", "nxcalc",
                            "nxtetris",
                            "demo-hello", "demo-arc", "demo-blit" };
        // nxchess loads its piece sprites from /usr/share/nxchess/*.gif at
        // runtime (HAVE_GIF_SUPPORT=Y in our MW config enables the decoder).
        RunWsl($"mkdir -p {wslDir}/board/rvemu/rootfs-overlay/usr/share/nxchess && " +
               $"cp $HOME/rvemu-mw/microwindows/src/demos/tuxchess/images/*.gif " +
               $"   {wslDir}/board/rvemu/rootfs-overlay/usr/share/nxchess/");

        // Taskbar launcher entries — rvemu-taskbar scans this dir at startup
        // and re-scans whenever the dir's mtime changes (poll every second).
        // `rvpkg install` drops new .desktop files here and the taskbar
        // picks them up automatically without restart.
        var launchers = new (string name, string prog)[] {
            ("Terminal", "/usr/bin/rvemu-term"),
            ("Clock",    "/usr/bin/nxclock"),
            ("Eyes",     "/usr/bin/nxeyes"),
            ("Chess",    "/usr/bin/nxchess"),
            ("Calc",     "/usr/bin/nxcalc"),
            ("Tetris",   "/usr/bin/nxtetris"),
            ("Hello",    "/usr/bin/demo-hello"),
            // doomgeneric is now baked into the base image via
            // BR2_PACKAGE_DOOMGENERIC=y. Its own .ipk also ships a
            // doom.desktop but our rm-and-rewrite of the launchers dir
            // would wipe it, so we re-add it explicitly here.
            ("Doom",     "/usr/bin/doom"),
        };
        // Wipe stale .desktop entries first so removing an item from the list
        // above also removes the button. Have to scrub BOTH the rootfs-overlay
        // (source) AND output/target (where buildroot stages the rootfs) —
        // buildroot only copies overlay → target, it never removes files from
        // target that were placed by a previous build.
        RunWsl($"rm -rf {wslDir}/board/rvemu/rootfs-overlay/etc/rvemu-launchers.d " +
               $"      {wslDir}/output/target/etc/rvemu-launchers.d");
        RunWsl($"mkdir -p {wslDir}/board/rvemu/rootfs-overlay/etc/rvemu-launchers.d");
        foreach (var (name, prog) in launchers)
        {
            string body = $"Name={name}\nExec={prog}\n";
            string b64  = Convert.ToBase64String(System.Text.Encoding.UTF8.GetBytes(body));
            string file = $"{wslDir}/board/rvemu/rootfs-overlay/etc/rvemu-launchers.d/{name.ToLower()}.desktop";
            RunWsl($"echo {b64} | base64 -d > {file}");
        }
        foreach (var bin in mwBins)
        {
            RunWsl($"cp $HOME/rvemu-mw/microwindows/src/bin/{bin} " +
                   $"{wslDir}/board/rvemu/rootfs-overlay/usr/bin/{bin} 2>/dev/null && " +
                   $"chmod 0755 {wslDir}/board/rvemu/rootfs-overlay/usr/bin/{bin}");
        }

        // S45microwindows — stops our makeshift rvemu-desktop and brings up the
        // real stack: nano-X server, nano-X window manager, plus nxclock and
        // nxeyes as visible demos. Each runs detached. nano-X listens on
        // /tmp/.nano-X for client connections.
        const string s45 = @"#!/bin/sh
case ""$1"" in
    start)
        [ -x /usr/bin/nano-X ] && [ -c /dev/input/event0 ] || exit 0
        # If rvemu-desktop is up, stop it — it owns the same framebuffer.
        [ -x /etc/init.d/S43desktop ] && /etc/init.d/S43desktop stop 2>/dev/null
        echo -n 'Starting nano-X server... '
        /usr/bin/nano-X >/dev/null 2>&1 &
        echo $! > /var/run/nano-X.pid
        # Wait for the socket to come up before launching clients.
        i=0
        while [ ! -S /tmp/.nano-X ] && [ $i -lt 20 ]; do
            sleep 0.1
            i=$((i+1))
        done
        echo 'OK'
        echo -n 'Starting nanowm... '
        /usr/bin/nanowm >/dev/null 2>&1 &
        echo $! > /var/run/nanowm.pid
        echo 'OK'
        echo -n 'Starting desktop apps (rvemu-taskbar, nxclock, nxeyes)... '
        # rvemu-taskbar — our own bottom-of-screen launcher with proper
        # graphical buttons. The clock + eyes show as initial visible
        # apps; everything else is one click away on the taskbar.
        /usr/bin/rvemu-taskbar >/dev/null 2>&1 &
        echo $! > /var/run/rvemu-taskbar.pid
        /usr/bin/nxclock       >/dev/null 2>&1 &
        /usr/bin/nxeyes        >/dev/null 2>&1 &
        echo 'OK'
        ;;
    stop)
        # Kill the taskbar first so its parent loop doesn't try to reconnect
        # to the dying server. Then clients. Then the server itself.
        for f in rvemu-taskbar nxeyes nxclock nanowm nano-X; do
            [ -f /var/run/$f.pid ] && kill $(cat /var/run/$f.pid) 2>/dev/null
            rm -f /var/run/$f.pid
        done
        # Belt-and-suspenders — vfork'd children may not have written pidfiles.
        killall nxeyes nxclock nanowm rvemu-taskbar nano-X 2>/dev/null
        ;;
esac
exit 0
";
        string s45B64 = Convert.ToBase64String(System.Text.Encoding.UTF8.GetBytes(s45));
        RunWsl($"echo {s45B64} | base64 -d > {wslDir}/board/rvemu/rootfs-overlay/etc/init.d/S45microwindows");
        RunWsl($"chmod 0755 {wslDir}/board/rvemu/rootfs-overlay/etc/init.d/S45microwindows");
        // Disable S43desktop on boot — Microwindows takes the framebuffer.
        // Buildroot's rootfs build copies rootfs-overlay/ over target/ but
        // never deletes target/ files that were placed by a previous run,
        // so a stale S43desktop in target/etc/init.d/ will still be packed
        // into the cpio. Remove it from BOTH locations.
        RunWsl($"rm -f {wslDir}/board/rvemu/rootfs-overlay/etc/init.d/S43desktop " +
               $"      {wslDir}/output/target/etc/init.d/S43desktop");
    }
    else
    {
        Console.WriteLine("  Microwindows not built (skipping). To enable, build the");
        Console.WriteLine("  microwindows tree at $HOME/rvemu-mw/microwindows and re-run.");
    }
}
else
{
    Console.WriteLine("  rvemu-input cross-compile failed (toolchain not built yet — " +
                      "first `make` will build it; rerun Prepare after).");
}

RunWsl($"cd {wslDir} && yes '' | make olddefconfig");

// ── 6c. Force toolchain rebuild when wchar/locale flags toggle on ───────
// Buildroot caches the toolchain heavily — the uclibc + gcc-final stamp
// files don't get invalidated by a .config edit alone. If an existing
// toolchain was built without UCLIBC_HAS_WCHAR=y, we need to dirclean
// those packages so the next `make` picks up the new flags. The check
// is self-cancelling: after a rebuild the uclibc-ng .config contains
// UCLIBC_HAS_WCHAR=y and this branch is skipped.
Console.WriteLine("Checking toolchain for wchar/locale support...");
// Marker-file approach: the first time we run after adding the wchar/locale
// flags, force a uclibc + gcc-final dirclean so buildroot actually rebuilds
// the toolchain with UCLIBC_HAS_WCHAR=y. Subsequent runs see the marker and
// skip the dirclean.
//
// Why not detect by grepping the uclibc-*/.config for UCLIBC_HAS_WCHAR=y?
// Tried it — `bash -c '...$(...)...'` invoked through .NET ProcessStartInfo
// + ArgumentList apparently does NOT preserve command-substitution output
// (the var ends up empty even though the pipeline prints the right thing
// when run standalone). Marker files use only simple `[ -e PATH ]` tests
// which work reliably through that invocation path.
RunWsl(
    $"cd {wslDir} && if [ ! -e board/rvemu/.wchar-applied ]; then " +
    "  echo '  first run with wchar/locale - forcing toolchain rebuild'; " +
    "  make uclibc-dirclean host-gcc-final-dirclean toolchain-buildroot-dirclean && " +
    "  mkdir -p board/rvemu && touch board/rvemu/.wchar-applied; " +
    "else " +
    "  echo '  marker present - toolchain already rebuilt with wchar/locale'; " +
    "fi");

// ── 7. Build ─────────────────────────────────────────────────────────────

Console.WriteLine();
Console.WriteLine("================================================================");
Console.WriteLine(" Building (cnlohr's kernel config + mainline buildroot).");
Console.WriteLine(" First-time: 20-40 minutes (kernel re-fetched at version 6.8).");
Console.WriteLine("================================================================");
Console.WriteLine();
RunWsl($"cd {wslDir} && make linux-reconfigure");
int bld = RunWsl($"cd {wslDir} && make -j{jobs}");
if (bld != 0) return bld;

// ── 8. Copy outputs ──────────────────────────────────────────────────────

string cacheDir = Path.Combine(
    Environment.GetFolderPath(Environment.SpecialFolder.UserProfile),
    ".cache", "riscvemu", "linux");
Directory.CreateDirectory(cacheDir);

string imageOut = Path.Combine(cacheDir, "Image-net");
string dtbOut   = Path.Combine(cacheDir, "rvemu-net.dtb");
string wslCache = WindowsToWslPath(cacheDir);

Console.WriteLine($"Compiling DTS → {dtbOut}");
RunWsl($"{wslDir}/output/host/bin/dtc -I dts -O dtb " +
       $"-o '{wslCache}/rvemu-net.dtb' '{wslDir}/board/rvemu/cnlohr-minimal.dts' -S 2048");

Console.WriteLine($"Copying kernel → {imageOut}");
RunWsl($"cp '{wslDir}/output/images/Image' '{wslCache}/Image-net'");

// ── 8b. Download libslirp Windows DLLs + deps ──────────────────────────
// Vendor them into Core/Networking/native/ so Core.csproj's Content reference
// picks them up and they get auto-copied to every consumer's bin dir.

string repoRoot  = Path.GetFullPath(Path.Combine(AppContext.BaseDirectory, "..", "..", "..", "..", ".."));
string nativeDir = Path.Combine(repoRoot, "Core", "Networking", "native");
Directory.CreateDirectory(nativeDir);
string wslNative = WindowsToWslPath(nativeDir);

bool haveSlirp = System.IO.File.Exists(Path.Combine(nativeDir, "slirp.dll"));
if (!haveSlirp)
{
    Console.WriteLine("Downloading libslirp Windows DLLs + deps from msys2...");
    const string Mirror = "https://mirror.msys2.org/mingw/mingw64/";
    // Package name prefixes (without version/release suffix). We resolve the
    // current filename from the mirror's directory listing each run, so old
    // versions getting retired doesn't break us.
    string[] pkgPrefixes =
    {
        "libslirp", "glib2", "gettext-runtime", "libiconv", "pcre2", "gcc-libs",
    };
    RunWsl("rm -rf /tmp/slirp-pkgs && mkdir -p /tmp/slirp-pkgs && cd /tmp/slirp-pkgs && " +
           $"wget -q -O index.html '{Mirror}'");
    foreach (var prefix in pkgPrefixes)
    {
        // grep the index for the latest filename matching this prefix.
        string findCmd = $"grep -oP 'mingw-w64-x86_64-{prefix}-[0-9][^\"]*\\.pkg\\.tar\\.zst' /tmp/slirp-pkgs/index.html | sort -u | tail -1";
        string filename = "";
        try
        {
            var psi = new ProcessStartInfo("wsl") { UseShellExecute = false, RedirectStandardOutput = true };
            if (!string.IsNullOrEmpty(distro)) { psi.ArgumentList.Add("-d"); psi.ArgumentList.Add(distro); }
            psi.ArgumentList.Add("--"); psi.ArgumentList.Add("bash"); psi.ArgumentList.Add("-c");
            psi.ArgumentList.Add("export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin; " + findCmd);
            using var p = Process.Start(psi)!;
            filename = p.StandardOutput.ReadToEnd().Trim();
            p.WaitForExit();
        }
        catch { }
        if (string.IsNullOrEmpty(filename))
        {
            Console.Error.WriteLine($"Could not resolve current msys2 package for prefix '{prefix}'.");
            return 1;
        }
        Console.WriteLine($"  · {filename}");
        int rc = RunWsl($"cd /tmp/slirp-pkgs && wget -q '{Mirror}{filename}' && zstd -dc '{filename}' | tar -x");
        if (rc != 0)
        {
            Console.Error.WriteLine($"Failed to fetch/extract {filename}");
            return rc;
        }
    }
    // Copy every DLL the packages contain.
    RunWsl($"cp /tmp/slirp-pkgs/mingw64/bin/*.dll '{wslNative}/'");
    // .NET's DllImport looks up "slirp" — rename libslirp-0.dll to slirp.dll.
    RunWsl($"cd '{wslNative}' && [ -f libslirp-0.dll ] && cp -f libslirp-0.dll slirp.dll");
    int dllCount = Directory.GetFiles(nativeDir, "*.dll").Length;
    Console.WriteLine($"Installed {dllCount} DLLs in {nativeDir}");
}
else Console.WriteLine($"slirp.dll already present at {nativeDir} — skipping download.");

// ── 9. Done ──────────────────────────────────────────────────────────────

Console.WriteLine();
Console.WriteLine("================================================================");
Console.WriteLine(" Build complete.");
Console.WriteLine($"   Kernel: {imageOut}");
Console.WriteLine($"   DTB:    {dtbOut}");
Console.WriteLine();
Console.WriteLine(" Examples.Linux will now auto-detect these and run with --net.");
Console.WriteLine();
Console.WriteLine("   dotnet run --no-build --project Examples\\Linux -p:Platform=x64");
Console.WriteLine("================================================================");
return 0;

// ── Helpers ──────────────────────────────────────────────────────────────

static void PrintUsage()
{
    Console.WriteLine("Usage: Examples.Linux.Build_RV32i [options]");
    Console.WriteLine();
    Console.WriteLine("  --skip-apt              Skip apt-get install (assume packages already present)");
    Console.WriteLine("  --clean                 Remove existing buildroot dir and re-clone");
    Console.WriteLine("  --buildroot-dir <path>  WSL path (default: $HOME/rvemu-buildroot)");
    Console.WriteLine("  --distro <name>         Specific WSL distro (default: WSL default)");
    Console.WriteLine("  --apt pkg1,pkg2,...     Extra apt packages to install alongside the defaults");
    Console.WriteLine("  --sudo-password <pw>    WSL sudo password (skips prompt; default: prompt if needed)");
    Console.WriteLine("  -j, --jobs <N>          Parallel make jobs (default: 32)");
    Console.WriteLine();
    Console.WriteLine("Output goes to %USERPROFILE%\\.cache\\riscvemu\\linux\\Image-net and rvemu-net.dtb.");
}

bool HasWsl()
{
    try
    {
        var psi = new ProcessStartInfo("wsl", "--status")
        {
            UseShellExecute = false,
            RedirectStandardOutput = true,
            RedirectStandardError  = true,
        };
        using var p = Process.Start(psi)!;
        p.WaitForExit(5000);
        return p.HasExited && p.ExitCode == 0;
    }
    catch { return false; }
}

int RunWsl(string command) => RunWslInner(command, captureSilently: false, stdin: null);
int RunWslSilent(string command) => RunWslInner(command, captureSilently: true, stdin: null);

int RunWslSudo(string command, string? password)
{
    if (password == null)
        return RunWslInner($"sudo -n {command}", captureSilently: false, stdin: null);
    // `-S` reads password from stdin; `-p ''` suppresses the "[sudo] password for X:" prompt.
    return RunWslInner($"sudo -S -p '' {command}", captureSilently: false, stdin: password + "\n");
}

int RunWslInner(string command, bool captureSilently, string? stdin)
{
    var psi = new ProcessStartInfo("wsl")
    {
        UseShellExecute        = false,
        RedirectStandardOutput = true,
        RedirectStandardError  = true,
        RedirectStandardInput  = stdin != null,
    };
    if (!string.IsNullOrEmpty(distro)) { psi.ArgumentList.Add("-d"); psi.ArgumentList.Add(distro); }
    psi.ArgumentList.Add("--");
    psi.ArgumentList.Add("bash");
    psi.ArgumentList.Add("-c");
    // Force a clean POSIX PATH for every WSL command. Without this, WSL inherits
    // Windows PATH (which contains spaces — "Program Files" etc.) and buildroot's
    // dependency check rejects the build with "Your PATH contains spaces...".
    psi.ArgumentList.Add("export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin; " + command);

    using var p = Process.Start(psi)!;
    if (stdin != null)
    {
        p.StandardInput.Write(stdin);
        p.StandardInput.Flush();
        p.StandardInput.Close();
    }
    if (!captureSilently)
    {
        p.OutputDataReceived += (_, e) => { if (e.Data != null) Console.WriteLine(e.Data); };
        p.ErrorDataReceived  += (_, e) => { if (e.Data != null) Console.Error.WriteLine(e.Data); };
        p.BeginOutputReadLine();
        p.BeginErrorReadLine();
    }
    p.WaitForExit();
    if (captureSilently) { _ = p.StandardOutput.ReadToEnd(); _ = p.StandardError.ReadToEnd(); }
    return p.ExitCode;
}

static string ReadPassword(string prompt)
{
    Console.Write(prompt);
    var sb = new System.Text.StringBuilder();
    while (true)
    {
        var k = Console.ReadKey(intercept: true);
        if (k.Key == ConsoleKey.Enter) break;
        if (k.Key == ConsoleKey.Backspace)
        {
            if (sb.Length > 0) sb.Length--;
        }
        else if (!char.IsControl(k.KeyChar))
        {
            sb.Append(k.KeyChar);
        }
    }
    Console.WriteLine();
    return sb.ToString();
}

void WriteFileViaWsl(string wslPath, string content)
{
    // Use base64 to avoid quoting headaches with newlines + quotes in the content.
    string b64 = Convert.ToBase64String(Encoding.UTF8.GetBytes(content));
    int rc = RunWsl($"echo '{b64}' | base64 -d > '{wslPath}'");
    if (rc != 0) throw new InvalidOperationException($"Failed to write {wslPath}");
}

static string WindowsToWslPath(string winPath)
{
    // Pure C# conversion: C:\Users\X\.cache → /mnt/c/Users/X/.cache.
    // Avoids the wsl→bash arg-passing minefield where backslashes can get
    // eaten by intermediate shells.
    string full = Path.GetFullPath(winPath);
    if (full.Length < 3 || full[1] != ':' || (full[2] != '\\' && full[2] != '/'))
        throw new InvalidOperationException($"Cannot map non-drive path to WSL: {winPath}");
    char drive = char.ToLowerInvariant(full[0]);
    string rest = full.Substring(2).Replace('\\', '/');
    return $"/mnt/{drive}{rest}";
}
