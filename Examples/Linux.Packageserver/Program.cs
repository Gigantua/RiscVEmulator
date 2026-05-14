// Examples.Linux.Packageserver — interactive curator + opkg feed server for the
// RV32 guest.
//
//   dotnet run --project Examples\Linux.Packageserver
//     > search nano                # search buildroot's available packages
//     > add nano                   # mark a package for inclusion
//     > list                       # show curated set
//     > build                      # cross-build everything via WSL/buildroot
//     > serve                      # HTTP feed on http://0.0.0.0:8080/
//     > run                        # build then serve (eager)
//     > quit
//
// State lives in `packages.json` next to the executable. The feed (.ipk files
// + Packages.gz) lands in `feed-cache/`.
//
// Guest side: rvpkg + busybox ar/tar applets are baked in by
// Examples.Linux.Build_RV32i; it points the guest at this server's address.
// at http://10.0.2.2:8080 (libslirp's host-loopback NAT).

using System.Diagnostics;
using System.Formats.Tar;
using System.IO.Compression;
using System.Net;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using System.Text.RegularExpressions;

// ── Paths ────────────────────────────────────────────────────────────────────

string ProjectDir   = AppContext.BaseDirectory;
string RepoRoot     = Path.GetFullPath(Path.Combine(ProjectDir, "..", "..", "..", "..", ".."));
string PackagesJson = Path.Combine(RepoRoot, "Examples", "Linux.Packageserver", "packages.json");
string FeedCacheDir = Path.Combine(RepoRoot, "Examples", "Linux.Packageserver", "feed-cache");
string WslBuildroot = "$HOME/rvemu-buildroot";

const string FeedArch = "rv32inommu_uclibc";
const int    FeedPort = 8080;

// ── Args / mode ──────────────────────────────────────────────────────────────

if (args.Length > 0)
{
    return await ExecuteAsync(args);
}

// No args → interactive REPL.
Console.WriteLine("rvemu package server. Commands: search <q>, add <name>, remove <name>, list, build, serve, run, quit");
while (true)
{
    Console.Write("> ");
    var line = Console.ReadLine();
    if (line is null) break;
    line = line.Trim();
    if (line == "" || line == "quit" || line == "exit") break;
    try
    {
        var tokens = line.Split(' ', StringSplitOptions.RemoveEmptyEntries);
        int rc = await ExecuteAsync(tokens);
        if (rc != 0) Console.Error.WriteLine($"(exit {rc})");
    }
    catch (Exception ex)
    {
        Console.Error.WriteLine($"ERROR: {ex.Message}");
    }
}
return 0;

// ── Dispatcher ───────────────────────────────────────────────────────────────

async Task<int> ExecuteAsync(string[] argv)
{
    switch (argv[0])
    {
        case "search":
            DoSearch(argv.Length < 2 ? "" : string.Join(' ', argv.Skip(1)));
            return 0;

        case "add":
            if (argv.Length < 2) { Console.Error.WriteLine("usage: add <name> [<name>...]"); return 1; }
            DoAdd(argv.Skip(1));
            return 0;

        case "remove":
            if (argv.Length < 2) { Console.Error.WriteLine("usage: remove <name> [<name>...]"); return 1; }
            DoRemove(argv.Skip(1));
            return 0;

        case "list":
            DoList();
            return 0;

        case "build":
            return await DoBuildAsync();

        case "serve":
            return await DoServeAsync();

        case "run":
            { int rc = await DoBuildAsync(); return rc != 0 ? rc : await DoServeAsync(); }

        case "help":
        case "-h":
        case "--help":
            Console.WriteLine("commands: search <q>, add <name>, remove <name>, list, build, serve, run");
            return 0;

        default:
            Console.Error.WriteLine($"unknown command: {argv[0]}");
            return 1;
    }
}

// ── Catalog (packages.json) ─────────────────────────────────────────────────

Catalog LoadCatalog()
{
    if (!File.Exists(PackagesJson)) return new Catalog();
    return JsonSerializer.Deserialize<Catalog>(File.ReadAllText(PackagesJson)) ?? new Catalog();
}

void SaveCatalog(Catalog c)
{
    Directory.CreateDirectory(Path.GetDirectoryName(PackagesJson)!);
    File.WriteAllText(PackagesJson, JsonSerializer.Serialize(c, new JsonSerializerOptions { WriteIndented = true }));
}

// ── Buildroot package discovery ──────────────────────────────────────────────
// Walk `package/*/Config.in` in WSL and grep `config BR2_PACKAGE_*` symbols
// with the line after `bool "..."`.

List<BrPackage> ListBuildrootPackages()
{
    // Write the helper script via heredoc (quoted delimiter — no escaping)
    // so we don't have to fight wsl.exe's quote handling for embedded `"`.
    string helperBody = @"
import os, re, sys
root = sys.argv[1]
for d in sorted(os.listdir(root)):
    cf = os.path.join(root, d, 'Config.in')
    if not os.path.isfile(cf): continue
    try: text = open(cf, encoding='utf-8', errors='ignore').read()
    except OSError: continue
    msym = re.search(r'^config (BR2_PACKAGE_[A-Z0-9_]+)\s*$', text, re.M)
    if not msym: continue
    mbool = re.search(r'^\s*bool\s*""([^""]*)""', text, re.M)
    desc = mbool.group(1) if mbool else ''
    print(f'{d}\t{msym.group(1)}\t{desc}')
";
    // C# verbatim "" → " (single). Python sees "([^"]*)" which is right.
    string helperPath = "/tmp/rvemu-listpkgs.py";
    var write = RunWsl($"cat > {helperPath} << 'PYEOF'\n{helperBody}\nPYEOF");
    if (write.rc != 0) throw new Exception($"Failed to stage list-packages helper: {write.stderr}");

    var (rc, stdout, stderr) = RunWsl($"python3 {helperPath} {WslBuildroot}/package");
    if (rc != 0) throw new Exception($"Failed to list buildroot packages (rc={rc}):\n{stderr}");
    var list = new List<BrPackage>();
    foreach (var line in stdout.Split('\n', StringSplitOptions.RemoveEmptyEntries))
    {
        var parts = line.Split('\t', 3);
        if (parts.Length < 2) continue;
        list.Add(new BrPackage(parts[0], parts[1], parts.Length > 2 ? parts[2] : ""));
    }
    return list;
}

// Cached, since walking ~3000 Config.in files takes ~1s.
List<BrPackage> AllBrPackages()
{
    PackageCache.All ??= ListBuildrootPackages();
    return PackageCache.All;
}

void DoSearch(string query)
{
    var all = AllBrPackages();
    bool empty = string.IsNullOrWhiteSpace(query);
    var hits = empty
        ? all
        : all.Where(p =>
            p.Name.Contains(query, StringComparison.OrdinalIgnoreCase) ||
            p.Description.Contains(query, StringComparison.OrdinalIgnoreCase));
    int n = 0, total = hits.Count();
    foreach (var p in hits)
    {
        Console.WriteLine($"  {p.Name,-32} {p.Description}");
        if (++n >= 50)
        {
            Console.WriteLine($"  ... and {total - 50} more (type 'search <substring>' to filter, or '/' for fuzzy)");
            break;
        }
    }
    if (n == 0) Console.WriteLine("  no matches");
    if (empty) Console.WriteLine($"  ({all.Count} total in buildroot)");
}

void DoAdd(IEnumerable<string> names)
{
    var all = AllBrPackages();
    var byName = all.ToDictionary(p => p.Name, StringComparer.OrdinalIgnoreCase);
    var cat = LoadCatalog();
    foreach (var n in names)
    {
        if (!byName.TryGetValue(n, out var pkg))
        {
            Console.Error.WriteLine($"  ? unknown: {n} (try 'search {n}')");
            continue;
        }
        if (cat.Packages.Any(p => p.Name.Equals(pkg.Name, StringComparison.OrdinalIgnoreCase)))
        {
            Console.WriteLine($"  - already in catalog: {pkg.Name}");
            continue;
        }
        cat.Packages.Add(new CatalogEntry { Name = pkg.Name, BrSymbol = pkg.BrSymbol });
        Console.WriteLine($"  + {pkg.Name} ({pkg.BrSymbol})");
    }
    SaveCatalog(cat);
}

void DoRemove(IEnumerable<string> names)
{
    var cat = LoadCatalog();
    var nameSet = new HashSet<string>(names, StringComparer.OrdinalIgnoreCase);
    int before = cat.Packages.Count;
    cat.Packages.RemoveAll(p => nameSet.Contains(p.Name));
    SaveCatalog(cat);
    Console.WriteLine($"removed {before - cat.Packages.Count}");
}

void DoList()
{
    var cat = LoadCatalog();
    if (cat.Packages.Count == 0) { Console.WriteLine("  (empty — try 'search nano' then 'add nano')"); return; }
    foreach (var p in cat.Packages)
        Console.WriteLine($"  {p.Name,-32} {p.BrSymbol}  {(p.BuiltVersion ?? "(unbuilt)")}");
}

// ── Build (cross-compile each catalog entry, pack .ipk) ─────────────────────

async Task<int> DoBuildAsync()
{
    var cat = LoadCatalog();
    if (cat.Packages.Count == 0) { Console.WriteLine("nothing to build (catalog empty)"); return 0; }
    Directory.CreateDirectory(FeedCacheDir);

    foreach (var entry in cat.Packages)
    {
        Console.WriteLine($"=== {entry.Name} ({entry.BrSymbol}) ===");
        var (ok, ipkPath, version) = await BuildAndPackAsync(entry);
        if (!ok) { Console.Error.WriteLine($"  ! build failed"); continue; }
        entry.BuiltVersion = version;
        entry.IpkFile      = Path.GetFileName(ipkPath);
        Console.WriteLine($"  → {entry.IpkFile}");
    }
    SaveCatalog(cat);

    // Regenerate Packages.gz from whatever .ipk's are in feed-cache.
    Console.WriteLine("Indexing feed...");
    var ipks = Directory.GetFiles(FeedCacheDir, "*.ipk");
    var packages = new StringBuilder();
    foreach (var ipk in ipks)
    {
        var stanza = ExtractControlStanza(ipk);
        long size  = new FileInfo(ipk).Length;
        string sha256 = Convert.ToHexString(SHA256.HashData(File.ReadAllBytes(ipk))).ToLowerInvariant();
        packages.Append(stanza.TrimEnd('\n', '\r'));
        packages.Append('\n');
        packages.Append($"Filename: {Path.GetFileName(ipk)}\n");
        packages.Append($"Size: {size}\n");
        packages.Append($"SHA256sum: {sha256}\n\n");
    }
    File.WriteAllText(Path.Combine(FeedCacheDir, "Packages"), packages.ToString());
    using (var fs = File.Create(Path.Combine(FeedCacheDir, "Packages.gz")))
    using (var gz = new GZipStream(fs, CompressionLevel.Optimal))
        gz.Write(Encoding.UTF8.GetBytes(packages.ToString()));
    Console.WriteLine($"Feed ready: {ipks.Length} packages in {FeedCacheDir}");
    return 0;
}

async Task<(bool ok, string ipkPath, string version)> BuildAndPackAsync(CatalogEntry entry)
{
    // 1. Ensure the symbol is enabled in buildroot's .config.
    var ensure = $@"
        cd {WslBuildroot}
        grep -q '^{entry.BrSymbol}=y$' .config || {{
            sed -i '/^# {entry.BrSymbol} is not set$/d; /^{entry.BrSymbol}=/d' .config
            echo '{entry.BrSymbol}=y' >> .config
            yes '' | make olddefconfig >/dev/null 2>&1
        }}
    ";
    if (RunWsl(ensure).rc != 0) return (false, "", "");

    // 2. Drop a timestamp marker. Anything in target/ whose mtime is newer
    //    than this after the rebuild belongs to this package (or its deps,
    //    rebuilt transitively — which is fine for bundled .ipks).
    //    Plain diff-of-find can't work: buildroot's stamps make `make <pkg>`
    //    a no-op once the .stamp_built file exists, so files already in
    //    target/ from an earlier session don't reappear in the diff.
    string marker = "/tmp/rvemu-pkg-marker";
    RunWsl($"rm -f {marker} && sleep 1 && touch {marker}");

    // 3. Force rebuild + reinstall so the install step touches every file.
    string pkgLower = entry.Name.ToLowerInvariant();
    var (rc, _, _) = RunWsl($"cd {WslBuildroot} && make {pkgLower}-rebuild", echo: true);
    if (rc != 0) return (false, "", "");

    // 4. Find everything in target/ newer than the marker.
    var (_, newFilesStdout, _) = RunWsl(
        $"find {WslBuildroot}/output/target -newer {marker} \\( -type f -o -type l \\)");
    var newFiles = newFilesStdout.Split('\n', StringSplitOptions.RemoveEmptyEntries).ToList();
    if (newFiles.Count == 0)
    {
        Console.Error.WriteLine($"  (no new files — package may have no targetinstalled artifacts)");
        return (false, "", "");
    }
    Console.WriteLine($"  captured {newFiles.Count} files");

    // 5. Discover version from buildroot's per-package staging metadata.
    var (_, verStdout, _) = RunWsl($"make -s -C {WslBuildroot} {pkgLower}-show-version 2>/dev/null || true");
    string version = verStdout.Trim();
    if (string.IsNullOrWhiteSpace(version)) version = "0.0";
    // Sanitize: opkg dislikes weird chars.
    version = Regex.Replace(version, @"[^0-9A-Za-z.\-+~]", "");

    // 6. Pack the .ipk entirely in WSL — Python writes control.tar.gz +
    //    data.tar.gz, then `ar` glues the three members into the outer
    //    archive. Avoids fighting wslpath/quoting when bridging files between
    //    WSL and Windows filesystems; only the finished .ipk crosses over.
    string ipkName = $"{entry.Name}_{version}_{FeedArch}.ipk";
    string ipkPath = Path.Combine(FeedCacheDir, ipkName);
    string ipkWsl  = $"/tmp/{ipkName}";

    var control =
        $"Package: {entry.Name}\n" +
        $"Version: {version}\n" +
        $"Architecture: {FeedArch}\n" +
        $"Maintainer: rvemu-feed\n" +
        $"Description: {entry.Name} (cross-built via buildroot)\n";

    string packScript = @"
import os, sys, tarfile, subprocess
filelist, target_root, control_text, out_ipk = sys.argv[1:]
# control.tar.gz
with open('/tmp/control', 'w') as f: f.write(control_text)
with tarfile.open('/tmp/control.tar.gz', 'w:gz') as t:
    t.add('/tmp/control', arcname='./control')
# data.tar.gz from the listed paths, mapped relative to target_root
with tarfile.open('/tmp/data.tar.gz', 'w:gz') as t:
    seen_dirs = set()
    for line in open(filelist):
        p = line.strip()
        if not p or not p.startswith(target_root): continue
        rel = p[len(target_root):].lstrip('/')
        if not rel: continue
        # Add parent dirs (./usr, ./usr/bin) once each.
        parts = rel.split('/')
        for i in range(1, len(parts)):
            d = '/'.join(parts[:i])
            if d in seen_dirs: continue
            seen_dirs.add(d)
            ti = tarfile.TarInfo('./' + d); ti.type = tarfile.DIRTYPE; ti.mode = 0o755
            t.addfile(ti)
        t.add(p, arcname='./' + rel)
# debian-binary
with open('/tmp/debian-binary', 'w') as f: f.write('2.0\n')
# ar -r is destructive: remove any prior ipk first
try: os.remove(out_ipk)
except FileNotFoundError: pass
subprocess.run(['ar', 'rc', out_ipk,
                '/tmp/debian-binary', '/tmp/control.tar.gz', '/tmp/data.tar.gz'],
               check=True)
print(out_ipk)
";
    // Write helper + filelist + control text via heredoc/cat (no quoting hell).
    RunWsl($"cat > /tmp/rvemu-packipk.py << 'PYEOF'\n{packScript}\nPYEOF");
    string newFilesList = "/tmp/rvemu-pkg-new.lst";
    RunWsl($"cat > {newFilesList} << 'NF_EOF'\n{string.Join('\n', newFiles)}\nNF_EOF");
    string controlPath = "/tmp/rvemu-control-input.txt";
    RunWsl($"cat > {controlPath} << 'CTL_EOF'\n{control}\nCTL_EOF");
    var (prc, _, perr) = RunWsl(
        $"python3 /tmp/rvemu-packipk.py {newFilesList} " +
        $"$(eval echo {WslBuildroot})/output/target " +
        $"\"$(cat {controlPath})\" {ipkWsl}");
    if (prc != 0)
    {
        Console.Error.WriteLine($"  ! pack failed:\n{perr}");
        return (false, "", "");
    }
    // Copy the finished single-file .ipk to the host via /mnt/c.
    var (_, wslDst, _) = RunWsl($"wslpath -u \"{ipkPath.Replace("\\", "/")}\"");
    RunWsl($"mkdir -p '$(dirname \"{wslDst.Trim()}\")' && cp {ipkWsl} \"{wslDst.Trim()}\"");
    return (true, ipkPath, version);
}

string ExtractControlStanza(string ipkPath)
{
    // .ipk is an `ar` archive with debian-binary, control.tar.gz, data.tar.gz
    using var fs = File.OpenRead(ipkPath);
    var br = new BinaryReader(fs);
    var magic = br.ReadBytes(8);
    if (Encoding.ASCII.GetString(magic) != "!<arch>\n") return "";
    while (fs.Position < fs.Length)
    {
        var hdr = br.ReadBytes(60);
        if (hdr.Length < 60) break;
        string name = Encoding.ASCII.GetString(hdr, 0, 16).TrimEnd(' ', '/');
        string sizeStr = Encoding.ASCII.GetString(hdr, 48, 10).TrimEnd(' ');
        long size = long.Parse(sizeStr);
        long dataStart = fs.Position;
        if (name == "control.tar.gz")
        {
            using var gz = new GZipStream(fs, CompressionMode.Decompress, leaveOpen: true);
            using var tarReader = new TarReader(gz);
            while (tarReader.GetNextEntry() is { } entry)
            {
                if (entry.Name.EndsWith("control") || entry.Name == "./control" || entry.Name == "control")
                {
                    using var ms = new MemoryStream();
                    entry.DataStream?.CopyTo(ms);
                    return Encoding.UTF8.GetString(ms.ToArray());
                }
            }
            return "";
        }
        fs.Position = dataStart + size + (size % 2);
    }
    return "";
}

// ── Serve ────────────────────────────────────────────────────────────────────

async Task<int> DoServeAsync()
{
    Directory.CreateDirectory(FeedCacheDir);
    // Bind to 127.0.0.1 only — no admin needed, and that's exactly where
    // libslirp's host-loopback NAT lands guest connections to 10.0.2.2.
    var listener = new HttpListener();
    listener.Prefixes.Add($"http://127.0.0.1:{FeedPort}/");
    listener.Prefixes.Add($"http://localhost:{FeedPort}/");
    listener.Start();
    Console.WriteLine($"Serving {FeedCacheDir} on http://localhost:{FeedPort}/ (guest URL: http://10.0.2.2:{FeedPort}/)");
    Console.WriteLine("Ctrl+C to stop.");
    using var cts = new CancellationTokenSource();
    Console.CancelKeyPress += (_, e) => { e.Cancel = true; cts.Cancel(); };

    var acceptTask = Task.Run(async () =>
    {
        while (!cts.IsCancellationRequested)
        {
            HttpListenerContext ctx;
            try { ctx = await listener.GetContextAsync(); }
            catch { break; }
            _ = Task.Run(() => ServeRequest(ctx));
        }
    });
    try { await Task.Delay(-1, cts.Token); } catch (TaskCanceledException) { }
    listener.Stop();
    return 0;
}

void ServeRequest(HttpListenerContext ctx)
{
    string urlPath = ctx.Request.Url?.AbsolutePath ?? "/";
    string rel = urlPath.TrimStart('/');
    if (rel == "") rel = "Packages.gz";

    // Prevent path traversal.
    rel = rel.Replace('\\', '/');
    if (rel.Contains("..")) { ctx.Response.StatusCode = 400; ctx.Response.Close(); return; }

    string path = Path.Combine(FeedCacheDir, rel);
    Console.WriteLine($"  GET {urlPath}  →  {(File.Exists(path) ? "200" : "404")}");
    if (!File.Exists(path)) { ctx.Response.StatusCode = 404; ctx.Response.Close(); return; }

    ctx.Response.ContentType = rel.EndsWith(".gz") ? "application/gzip"
                              : rel.EndsWith(".ipk") ? "application/octet-stream"
                              : "text/plain";
    ctx.Response.ContentLength64 = new FileInfo(path).Length;
    using (var fs = File.OpenRead(path))
        fs.CopyTo(ctx.Response.OutputStream);
    ctx.Response.OutputStream.Close();
}

// ── WSL invoker ──────────────────────────────────────────────────────────────

(int rc, string stdout, string stderr) RunWsl(string command, bool echo = false)
{
    var psi = new ProcessStartInfo("wsl")
    {
        UseShellExecute        = false,
        RedirectStandardOutput = true,
        RedirectStandardError  = true,
    };
    psi.ArgumentList.Add("--");
    psi.ArgumentList.Add("bash");
    psi.ArgumentList.Add("-c");
    psi.ArgumentList.Add("export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin; " + command);
    if (echo) Console.WriteLine($"  $ {command.Replace("\n", " ").Substring(0, Math.Min(160, command.Replace("\n"," ").Length))}");
    using var p = Process.Start(psi)!;
    if (echo)
    {
        // Stream output through.
        p.OutputDataReceived += (_, e) => { if (e.Data != null) Console.WriteLine($"    {e.Data}"); };
        p.ErrorDataReceived  += (_, e) => { if (e.Data != null) Console.Error.WriteLine($"    {e.Data}"); };
        p.BeginOutputReadLine();
        p.BeginErrorReadLine();
        p.WaitForExit();
        return (p.ExitCode, "", "");
    }
    string so = p.StandardOutput.ReadToEnd();
    string se = p.StandardError .ReadToEnd();
    p.WaitForExit();
    return (p.ExitCode, so, se);
}

// ── Types ────────────────────────────────────────────────────────────────────

static class PackageCache { public static List<BrPackage>? All; }

record BrPackage(string Name, string BrSymbol, string Description);

class Catalog
{
    public List<CatalogEntry> Packages { get; set; } = new();
}

class CatalogEntry
{
    public string Name { get; set; } = "";
    public string BrSymbol { get; set; } = "";
    public string? BuiltVersion { get; set; }
    public string? IpkFile { get; set; }
}

// ── IpkBuilder ───────────────────────────────────────────────────────────────
// .ipk = ar archive {debian-binary, control.tar.gz, data.tar.gz}.

static class IpkBuilder
{
    public static void Pack(string ipkPath, string dataRoot, string controlText)
    {
        using var ms = new MemoryStream();

        // ar magic.
        ms.Write(Encoding.ASCII.GetBytes("!<arch>\n"));

        // Member 1: debian-binary.
        WriteArMember(ms, "debian-binary", Encoding.ASCII.GetBytes("2.0\n"));

        // Member 2: control.tar.gz.
        WriteArMember(ms, "control.tar.gz", PackControl(controlText));

        // Member 3: data.tar.gz.
        WriteArMember(ms, "data.tar.gz", PackData(dataRoot));

        File.WriteAllBytes(ipkPath, ms.ToArray());
    }

    static void WriteArMember(Stream s, string name, byte[] data)
    {
        // ar member header: name(16) mtime(12) uid(6) gid(6) mode(8) size(10) magic(2)
        var hdr = new byte[60];
        for (int i = 0; i < hdr.Length; i++) hdr[i] = (byte)' ';
        Span<byte> sp = hdr;
        Encoding.ASCII.GetBytes(name.PadRight(16)[..16]).CopyTo(sp[..16]);
        Encoding.ASCII.GetBytes("0".PadRight(12)).CopyTo(sp[16..28]);   // mtime
        Encoding.ASCII.GetBytes("0".PadRight(6)).CopyTo(sp[28..34]);    // uid
        Encoding.ASCII.GetBytes("0".PadRight(6)).CopyTo(sp[34..40]);    // gid
        Encoding.ASCII.GetBytes("100644".PadRight(8)).CopyTo(sp[40..48]); // mode
        Encoding.ASCII.GetBytes(data.Length.ToString().PadRight(10)).CopyTo(sp[48..58]);
        sp[58] = (byte)'`';
        sp[59] = (byte)'\n';
        s.Write(hdr);
        s.Write(data);
        if (data.Length % 2 != 0) s.WriteByte((byte)'\n');
    }

    static byte[] PackControl(string controlText)
    {
        using var outMs = new MemoryStream();
        using (var gz = new GZipStream(outMs, CompressionLevel.Optimal, leaveOpen: true))
        using (var tar = new TarWriter(gz, leaveOpen: true))
        {
            var bytes = Encoding.UTF8.GetBytes(controlText);
            var entry = new PaxTarEntry(TarEntryType.RegularFile, "./control") { Mode = (UnixFileMode)0b110_100_100 };
            entry.DataStream = new MemoryStream(bytes);
            tar.WriteEntry(entry);
        }
        return outMs.ToArray();
    }

    static byte[] PackData(string root)
    {
        using var outMs = new MemoryStream();
        using (var gz = new GZipStream(outMs, CompressionLevel.Optimal, leaveOpen: true))
        using (var tar = new TarWriter(gz, leaveOpen: true))
        {
            // Walk root, add each file with path relative to root, prefixed "./".
            foreach (var path in Directory.EnumerateFileSystemEntries(root, "*", SearchOption.AllDirectories))
            {
                string rel = Path.GetRelativePath(root, path).Replace('\\', '/');
                string entryName = "./" + rel;

                var attr = File.GetAttributes(path);
                if ((attr & FileAttributes.Directory) != 0)
                {
                    var de = new PaxTarEntry(TarEntryType.Directory, entryName.TrimEnd('/') + "/") { Mode = (UnixFileMode)0b111_101_101 };
                    tar.WriteEntry(de);
                }
                else
                {
                    var fe = new PaxTarEntry(TarEntryType.RegularFile, entryName) { Mode = (UnixFileMode)0b111_101_101 };
                    fe.DataStream = File.OpenRead(path);
                    tar.WriteEntry(fe);
                    fe.DataStream.Dispose();
                }
            }
        }
        return outMs.ToArray();
    }
}
