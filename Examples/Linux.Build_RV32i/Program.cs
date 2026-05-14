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
    // virtio-input from the guest side; the host can ship one virtio-mmio slot
    // per device (kbd, mouse) and userspace reads /dev/input/eventN via evdev.
    "CONFIG_INPUT", "CONFIG_INPUT_EVDEV", "CONFIG_INPUT_KEYBOARD",
    "CONFIG_INPUT_MOUSEDEV", "CONFIG_INPUT_MISC",
    "CONFIG_VIRTIO_INPUT",

    // ── Sound + MIDI ────────────────────────────────────────────────────────
    // ALSA + virtio-snd backs /dev/snd/* including raw MIDI on /dev/snd/midiCxDx.
    "CONFIG_SOUND", "CONFIG_SND", "CONFIG_SND_PCM", "CONFIG_SND_TIMER",
    "CONFIG_SND_RAWMIDI", "CONFIG_SND_SEQUENCER",
    "CONFIG_SND_VIRTIO",
};
Console.WriteLine($"Patching cnlohr-kernel.config (+{kernelEnable.Length} symbols)...");
foreach (var sym in kernelEnable)
{
    RunWsl($"sed -i 's|^# {sym} is not set$|{sym}=y|' {wslDir}/board/rvemu/cnlohr-kernel.config");
    RunWslSilent($"grep -q '^{sym}=' {wslDir}/board/rvemu/cnlohr-kernel.config || " +
                 $"echo '{sym}=y' >> {wslDir}/board/rvemu/cnlohr-kernel.config");
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
		 *   0x30000000 audio PCM
		 *   0x30100000 audio control
		 *   <tbd>      MIDI                                              */
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

// NOT injecting a simple-framebuffer DT node — exhaustively bisected and
// found that ANY well-formed simple-framebuffer node (with correct cells
// declaration so its reg resolves to a real size) pulls the FB region
// into memblock.memory, which drops ARCH_PFN_OFFSET to the FB's PFN
// (0x20000) and makes init_unavailable_range walk ~400k pages of the
// gap to RAM (0x80000) one-by-one via __memset(struct page). At 50 MIPS
// this is effectively forever. Repro: just `compatible="simple-framebuffer";
// reg=<0 0x20000000 0 0x40000>;` inside /chosen with #address-cells=2,
// #size-cells=2. Without proper cells the reg parses as size=0 and the
// kernel ignores it (but simplefb's probe would also reject it, so that's
// not a fix).
//
// Workable paths (NOT yet implemented):
//   1. Move FramebufferDevice INSIDE RAM (carve last 256KB of /memory),
//      then standard simple-framebuffer works because the kernel reserves
//      it from RAM rather than treating it as a separate bank.
//   2. Write a tiny custom in-tree driver "rvemu,framebuffer" that
//      ioremap's 0x20000000 without going through any memory-aware
//      framework. Kernel ignores the reg for memblock purposes.
//   3. Skip /dev/fb0 entirely. Userspace mmap's /dev/mem at 0x20000000
//      directly (same path Doom uses bare-metal). Already works with
//      CONFIG_DEVMEM=y (default).

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
       "# BR2_RISCV_ISA_RVA is not set\n" +
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
       // awk is built but broken in our nommu uclibc build (""Access to
       // negative field"" on `$1`). Leaving the symbols out — rvpkg uses
       // pure POSIX shell parsing instead.
       "BB_EOF");

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

RunWsl($"cd {wslDir} && yes '' | make olddefconfig");

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
