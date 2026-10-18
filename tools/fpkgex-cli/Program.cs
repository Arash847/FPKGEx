// fPKGEx CLI - identify, list and extract PS5 fPKG packages.
//
//   fpkgex info    <package.pkg>
//   fpkgex list    <package.pkg> [--filter <substr>]
//   fpkgex extract <package.pkg> <dest-dir> [--passcode <32 hex>] [--raw]
//
// Extraction runs entirely through the LibProsperoPkg engine, so the output
// matches the reference extraction byte for byte.
using System.Diagnostics;
using System.Reflection;
using System.Text;

namespace FpkgEx;

internal static class Program
{
    const string DefaultPasscode = "00000000000000000000000000000000";

    static Type _archive;
    static string _engineDir = "";

    static int Main(string[] rawArgs)
    {
        Console.OutputEncoding = Encoding.UTF8;
        var args = rawArgs.ToList();

        if (args.Count == 0 || args[0] is "-h" or "--help" or "help")
        {
            Usage();
            return args.Count == 0 ? 2 : 0;
        }

        string engine = null;
        int ei = args.FindIndex(a => a == "--engine");
        if (ei >= 0 && ei + 1 < args.Count) { engine = args[ei + 1]; args.RemoveRange(ei, 2); }
        _engineDir = engine ?? Environment.GetEnvironmentVariable("FPKGEX_ENGINE_DIR")
                             ?? @"D:\- PS5\FPKG GUI";

        string cmd = args[0];
        args.RemoveAt(0);

        try
        {
            if (!TryLoadEngine(out _archive))
            {
                Console.Error.WriteLine(
                    $"Could not load LibProsperoPkg.dll from:\n  {_engineDir}\n\n" +
                    "Pass --engine <folder> or set FPKGEX_ENGINE_DIR to the folder that holds it.");
                return 3;
            }

            switch (cmd)
            {
                case "info": return Info(args);
                case "list": return List(args);
                case "extract": return Extract(args);
                default:
                    Console.Error.WriteLine($"unknown command '{cmd}'");
                    Usage();
                    return 2;
            }
        }
        catch (TargetInvocationException tie)
        {
            Console.Error.WriteLine("error: " + (tie.InnerException ?? tie).Message);
            return 1;
        }
        catch (Exception ex)
        {
            Console.Error.WriteLine("error: " + ex.Message);
            return 1;
        }
    }

    static bool TryLoadEngine(out Type archive)
    {
        archive = null;
        string dll = Path.Combine(_engineDir, "LibProsperoPkg.dll");
        if (!File.Exists(dll)) return false;
        try
        {
            Assembly.LoadFrom(dll);
            archive = Assembly.LoadFrom(dll).GetType("LibProsperoPkg.PKG.ProsperoPackageArchive");
            return archive != null;
        }
        catch { return false; }
    }

    // ---------------------------------------------------------------- commands

    static int Info(List<string> args)
    {
        if (args.Count < 1) { Console.Error.WriteLine("info: missing package path"); return 2; }
        string pkg = args[0];
        if (!File.Exists(pkg)) { Console.Error.WriteLine($"not found: {pkg}"); return 2; }

        var map = Invoke("Inspect", pkg);
        var t = map.GetType();
        Console.WriteLine(Path.GetFileName(pkg));
        Console.WriteLine(new string('-', 60));
        foreach (var p in t.GetProperties())
        {
            object v;
            try { v = p.GetValue(map); } catch { continue; }
            string s = v is System.Collections.IEnumerable en && v is not string
                ? $"[{en.Cast<object>().Count()} items]"
                : Convert.ToString(v);
            if (!string.IsNullOrEmpty(s)) Console.WriteLine($"  {p.Name,-24} {s}");
        }
        return 0;
    }

    static int List(List<string> args)
    {
        if (args.Count < 1) { Console.Error.WriteLine("list: missing package path"); return 2; }
        string pkg = args[0];
        if (!File.Exists(pkg)) { Console.Error.WriteLine($"not found: {pkg}"); return 2; }

        int fi = args.FindIndex(a => a == "--filter");
        string filter = fi >= 0 && fi + 1 < args.Count ? args[fi + 1] : null;
        string pass = Passcode(args);

        var files = (System.Collections.IEnumerable)Invoke("ListExtractionFiles", pkg, pass, null);
        int shown = 0, total = 0;
        long bytes = 0;
        var sw = Stopwatch.StartNew();
        foreach (var f in files)
        {
            var (rel, size) = Describe(f);
            total++;
            bytes += size;
            if (filter != null && rel.IndexOf(filter, StringComparison.OrdinalIgnoreCase) < 0)
                continue;
            Console.WriteLine($"{size,12}  {rel}");
            shown++;
        }
        Console.WriteLine();
        Console.WriteLine($"{total} files, {Fmt(bytes)} total" +
                          (filter != null ? $" ({shown} shown)" : "") +
                          $" in {sw.Elapsed.TotalSeconds:0.00}s");
        return 0;
    }

    static int Extract(List<string> args)
    {
        var pos = args.Where(a => !a.StartsWith("--")).ToList();
        if (pos.Count < 2) { Console.Error.WriteLine("extract: usage: fpkgex extract <pkg> <dest>"); return 2; }
        string pkg = pos[0], dest = pos[1];
        if (!File.Exists(pkg)) { Console.Error.WriteLine($"not found: {pkg}"); return 2; }
        string pass = Passcode(args);
        bool raw = args.Contains("--raw");

        Directory.CreateDirectory(dest);

        long total = 0;
        try
        {
            var probe = (System.Collections.IEnumerable)Invoke("ListExtractionFiles", pkg, pass, null);
            foreach (var f in probe) total += Describe(f).size;
        }
        catch { /* listing is best effort */ }

        Console.WriteLine($"extracting {Path.GetFileName(pkg)} -> {dest}");
        Console.WriteLine($"expected {Fmt(total)} across the package\n");

        var sw = Stopwatch.StartNew();
        int lastPct = -1;
        long done = 0;
        long written = 0;
        Action<int> onProgress = pct =>
        {
            if (pct / 5 > lastPct / 5)
            {
                lastPct = pct;
                Console.Write($"  {pct,3}%  {Bar(pct)}  {Fmt(written)} / {Fmt(total)}\n");
            }
        };

        string current = null;
        var swFile = Stopwatch.StartNew();
        Action<string> onFile = rel =>
        {
            written++;
            if (rel == null) return;
            // Report the active file between percent ticks.
            if (swFile.ElapsedMilliseconds > 400)
            {
                swFile.Restart();
                current = rel;
            }
        };

        var extracted = (System.Collections.IEnumerable)Pick("ExtractInnerFiles", 8)
            .Invoke(null, new object[]
            {
                pkg, dest, pass, !raw, CancellationToken.None,
                onProgress, onFile, (Predicate<string>)null
            });

        int count = 0;
        foreach (var item in extracted) count++;
        _ = current;

        // The reference layout also carries the sce_sys payloads that live in
        // the package's CNT metadata block rather than in the inner image.
        // The engine writes them flat plus its own bookkeeping files, so lift
        // the payloads into sce_sys/ and tuck the bookkeeping out of the way.
        if (!args.Contains("--no-cnt"))
        {
            Console.WriteLine("adding sce_sys metadata from the CNT block...");
            try
            {
                var cnt = (System.Collections.IEnumerable)Pick("ExtractCntEntries", 4)
                    .Invoke(null, new object[] { pkg, dest, pass, false });

                // The engine returns the paths it wrote, so relocate exactly
                // those rather than guessing from the directory listing.
                var cntPaths = new List<string>();
                int c = 0;
                foreach (var item in cnt)
                {
                    c++;
                    string p = Convert.ToString(item);
                    if (!string.IsNullOrEmpty(p)) cntPaths.Add(p);
                }
                Console.WriteLine($"  {c} metadata files");
                count += c;
                ArrangeCntFiles(dest, cntPaths);

                // The engine does not emit every CNT payload (ext_info.dat,
                // nptitle.dat and the npbind records). Read the CNT block
                // directly and add whatever is still missing.
                var present = Directory
                    .EnumerateFiles(Path.Combine(dest, "sce_sys"), "*", SearchOption.AllDirectories)
                    .Select(p => "sce_sys/" + Path.GetRelativePath(Path.Combine(dest, "sce_sys"), p).Replace('\\', '/'))
                    .ToList();
                int extra = CntReader.Supplement(pkg, dest, present);
                if (extra > 0)
                {
                    Console.WriteLine($"  added {extra} further CNT payloads from the metadata block");
                    count += extra;
                }
            }
            catch (Exception ex)
            {
                Console.Error.WriteLine("  warning: CNT extraction failed: " + ex.Message);
            }
        }

        Console.WriteLine($"\ndone: {count} files in {sw.Elapsed.TotalSeconds:0.0}s -> {dest}");
        return 0;
    }

    // ---------------------------------------------------------------- helpers

    /// <summary>
    /// Moves the CNT payloads the engine wrote into sce_sys/ so the output tree
    /// matches the package layout, and relocates the engine's scratch files
    /// (.digests, entry-*.bin, ...) into a _cnt folder.
    ///
    /// The engine writes some payloads flat (icon0.png) and some as whole
    /// subtrees (trophy2/, uds/), so both shapes are handled.
    /// </summary>
    static void ArrangeCntFiles(string dest, List<string> paths)
    {
        string sceSys = Path.Combine(dest, "sce_sys");
        string scratch = Path.Combine(dest, "_cnt");
        int moved = 0, tucked = 0;

        foreach (string p in paths)
        {
            // The engine reports paths relative to the destination root, e.g.
            // "icon0.png" or "trophy2/trophy00.ucp". Keep that shape and just
            // re-root it under sce_sys/.
            string rel = p.Replace('\\', '/').TrimStart('/');
            if (rel.Length == 0) continue;

            string top = rel.Split('/')[0];
            bool isScratch = top.StartsWith('.') || top.StartsWith("entry-", StringComparison.Ordinal);

            string srcDir = Path.Combine(dest, rel.Replace('/', Path.DirectorySeparatorChar));
            string src = Directory.Exists(srcDir) ? srcDir : srcDir;   // file or dir
            if (!File.Exists(src) && !Directory.Exists(src)) continue;

            string baseDir = isScratch ? scratch : sceSys;
            string into = Path.Combine(baseDir, rel.Replace('/', Path.DirectorySeparatorChar));
            string intoParent = Path.GetDirectoryName(into);
            if (!string.IsNullOrEmpty(intoParent)) Directory.CreateDirectory(intoParent);

            if (Directory.Exists(into)) Directory.Delete(into, true);
            else if (File.Exists(into)) File.Delete(into);

            Directory.CreateDirectory(Path.GetDirectoryName(src) ?? dest);
            File.Move(src, into);
            if (isScratch) tucked++; else moved++;
        }

        PruneEmptyDirs(dest, sceSys, scratch);

        if (Directory.Exists(scratch) && !Directory.EnumerateFileSystemEntries(scratch).Any())
            Directory.Delete(scratch);

        if (moved > 0 || tucked > 0)
            Console.WriteLine($"  placed {moved} sce_sys entries, moved {tucked} engine files to _cnt/");
    }

    /// <summary>Removes directories left empty after relocating payloads.</summary>
    static void PruneEmptyDirs(string root, params string[] keep)
    {
        var keepSet = new HashSet<string>(
            keep.Select(k => Path.GetFullPath(k).TrimEnd(Path.DirectorySeparatorChar)),
            StringComparer.OrdinalIgnoreCase);

        foreach (var dir in Directory.GetDirectories(root).OrderByDescending(d => d.Length))
        {
            string full = Path.GetFullPath(dir).TrimEnd(Path.DirectorySeparatorChar);
            if (keepSet.Contains(full)) continue;
            if (!Directory.EnumerateFileSystemEntries(dir).Any()) Directory.Delete(dir);
        }
    }

    static void Tuck(string src, string dir, string name)
    {
        string target = Path.Combine(dir, name);
        if (File.Exists(target)) File.Delete(target);
        File.Move(src, target);
    }

    static string Passcode(List<string> args)
    {
        int i = args.FindIndex(a => a == "--passcode");
        string pass = i >= 0 && i + 1 < args.Count ? args[i + 1] : DefaultPasscode;
        if (pass.Length != 32)
            throw new ArgumentException("--passcode must be exactly 32 characters (16 bytes, hex).");
        return pass;
    }

    static (string rel, long size) Describe(object f)
    {
        var t = f.GetType();
        string rel = Convert.ToString(t.GetProperty("RelativePath")?.GetValue(f)) ?? "";
        object s = t.GetProperty("Size")?.GetValue(f);
        return (rel, s is null ? 0 : Convert.ToInt64(s));
    }

    static object Invoke(string name, params object[] argv) => Pick(name, argv.Length).Invoke(null, argv);

    /// <summary>
    /// Resolves an engine method by name and arity. Where several overloads
    /// share an arity the one that takes an Action&lt;int&gt; progress callback
    /// wins, because those are the cancellation-aware forms.
    /// </summary>
    static MethodInfo Pick(string name, int arity)
    {
        var candidates = _archive.GetMethods(BindingFlags.Public | BindingFlags.Static)
            .Where(m => m.Name == name && m.GetParameters().Length == arity)
            .ToList();

        if (candidates.Count == 0)
        {
            var have = string.Join(", ", _archive.GetMethods(BindingFlags.Public | BindingFlags.Static)
                .Where(m => m.Name == name)
                .Select(m => m.GetParameters().Length.ToString()));
            throw new MissingMethodException(
                $"The engine has no {name} overload taking {arity} arguments (available arities: {have}).");
        }

        return candidates.FirstOrDefault(m =>
                   m.GetParameters().Any(p => p.ParameterType == typeof(Action<int>)))
               ?? candidates[0];
    }

    static string Bar(int pct)
    {
        int n = pct / 5;
        return new string('#', n) + new string('.', 20 - n);
    }

    static string Fmt(long n)
    {
        string[] u = { "B", "KB", "MB", "GB", "TB" };
        double v = n; int i = 0;
        while (v >= 1024 && i < u.Length - 1) { v /= 1024; i++; }
        return (i == 0 ? v.ToString("0") : v.ToString(v < 10 ? "0.00" : "0.0")) + " " + u[i];
    }

    static void Usage()
    {
        Console.WriteLine("""
            fPKGEx - PS5 fPKG identify / list / extract

            USAGE
              fpkgex info    <package.pkg> [--engine <dir>]
              fpkgex list    <package.pkg> [--filter <text>] [--passcode <32 hex>]
              fpkgex extract <package.pkg> <dest-dir> [--passcode <32 hex>] [--raw]

            NOTES
              --passcode is 32 hex characters; fPKGs use 16 zero bytes by default.
              --raw keeps blocks in their stored form instead of decompressing.

            The extraction engine (LibProsperoPkg.dll) is loaded from the folder
            given by --engine, FPKGEX_ENGINE_DIR, or the default install path.
            """);
    }
}
