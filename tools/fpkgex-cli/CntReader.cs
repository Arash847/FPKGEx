// CntReader.cs - direct CNT metadata reader for PS5 packages.
//
// The extraction engine hands back most sce_sys payloads but not all of them.
// This reader walks the package's CNT block itself (the same FIH -> CNT ->
// entry table -> string table path the payload uses) so the missing payloads
// can be added, and so every sce_sys entry can be verified.
using System.Buffers.Binary;

namespace FpkgEx;

internal static class CntReader
{
    const string CntMagic = "\x7fCNT";

    public readonly record struct Entry(string Path, long DataOffset, long Size, uint Type);

    public sealed class Result
    {
        public string ContentId = "";
        public string TitleId = "";
        public string Version = "";
        public List<Entry> Entries { get; } = new();
    }

    public static Result Read(string packagePath)
    {
        var res = new Result();
        using var fs = new FileStream(packagePath, FileMode.Open, FileAccess.Read, FileShare.Read);

        long cnt = LocateCnt(fs, out long pfsOffset, out long pfsSize);
        if (cnt < 0) return res;

        byte[] hdr = ReadAt(fs, cnt, 0x80);
        if (hdr.Length < 0x80) return res;

        res.ContentId = CStr(hdr, 0x40, 48);

        uint entryCount = BinaryPrimitives.ReadUInt32BigEndian(hdr.AsSpan(0x10));
        uint tableOffset = BinaryPrimitives.ReadUInt32BigEndian(hdr.AsSpan(0x18));
        if (entryCount == 0 || entryCount > 4096) return res;

        byte[] table = ReadAt(fs, cnt + tableOffset, entryCount * 32);
        if (table.Length < entryCount * 32) return res;

        // Locate the string table (type 0x0200).
        byte[] strtab = null;
        for (uint i = 0; i < entryCount; i++)
        {
            int e = (int)(i * 32);
            if (BinaryPrimitives.ReadUInt32BigEndian(table.AsSpan(e)) != 0x0200u) continue;
            uint so = BinaryPrimitives.ReadUInt32BigEndian(table.AsSpan(e + 16));
            uint sz = BinaryPrimitives.ReadUInt32BigEndian(table.AsSpan(e + 20));
            if (sz is > 0 and <= 65536) strtab = ReadAt(fs, cnt + so, sz);
            break;
        }

        // Unnamed CNT records that still materialise as files. Confirmed against
        // the package: type 0x0402 is sce_sys/nptitle.dat (both 160 bytes).
        // Other unnamed types (0x0001/0x0010/0x0020/0x0080/0x0100/0x0400/
        // 0x0401/0x040A) are structural and are deliberately not materialised.
        var typeNames = new Dictionary<uint, string> { [0x0402] = "nptitle.dat" };

        for (uint i = 0; i < entryCount; i++)
        {
            int e = (int)(i * 32);
            uint type = BinaryPrimitives.ReadUInt32BigEndian(table.AsSpan(e));
            uint fnOff = BinaryPrimitives.ReadUInt32BigEndian(table.AsSpan(e + 4));
            uint dataOff = BinaryPrimitives.ReadUInt32BigEndian(table.AsSpan(e + 16));
            uint dataSz = BinaryPrimitives.ReadUInt32BigEndian(table.AsSpan(e + 20));

            if (dataSz == 0 || type == 0x0200) continue;

            string name = null;
            if (strtab != null && fnOff < strtab.Length)
            {
                int end = Array.IndexOf(strtab, (byte)0, (int)fnOff);
                if (end < 0) end = strtab.Length;
                if (end > fnOff) name = System.Text.Encoding.ASCII.GetString(strtab, (int)fnOff, end - (int)fnOff);
            }
            if (name == null && typeNames.TryGetValue(type, out string tn)) name = tn;
            if (name == null) continue;

            res.Entries.Add(new Entry($"sce_sys/{name}", cnt + dataOff, dataSz, type));
        }

        _ = pfsOffset;
        _ = pfsSize;
        return res;
    }

    /// <summary>Writes any CNT payload that is not already present under dest.</summary>
    public static int Supplement(string packagePath, string dest, List<string> existing)
    {
        Result r;
        try { r = Read(packagePath); }
        catch { return 0; }

        int added = 0;
        foreach (var e in r.Entries)
        {
            if (existing.Contains(e.Path)) continue;

            string full = Path.Combine(dest, e.Path.Replace('/', Path.DirectorySeparatorChar));
            if (File.Exists(full)) { existing.Add(e.Path); continue; }

            try
            {
                Directory.CreateDirectory(Path.GetDirectoryName(full)!);
                using var src = new FileStream(packagePath, FileMode.Open, FileAccess.Read, FileShare.Read);
                src.Seek(e.DataOffset, SeekOrigin.Begin);
                using var outp = new FileStream(full, FileMode.Create, FileAccess.Write);
                CopyExactly(src, outp, e.Size);
                existing.Add(e.Path);
                added++;
            }
            catch { /* skip unreadable payload */ }
        }
        return added;
    }

    static void CopyExactly(Stream src, Stream dst, long count)
    {
        byte[] buf = new byte[1 << 20];
        long left = count;
        while (left > 0)
        {
            int want = (int)Math.Min(buf.Length, left);
            int got = src.Read(buf, 0, want);
            if (got <= 0) break;
            dst.Write(buf, 0, got);
            left -= got;
        }
    }

    static long LocateCnt(FileStream fs, out long pfsOffset, out long pfsSize)
    {
        pfsOffset = 0; pfsSize = 0;
        byte[] head = ReadAt(fs, 0, 0x100);
        if (head.Length < 0x10) return -1;

        if (head.AsSpan(0, 4).SequenceEqual("\x7fCNT"u8)) return 0;

        if (head.AsSpan(0, 4).SequenceEqual("\x7fFIH"u8))
        {
            pfsOffset = (long)BinaryPrimitives.ReadUInt64LittleEndian(head.AsSpan(0x10));
            pfsSize = (long)BinaryPrimitives.ReadUInt64LittleEndian(head.AsSpan(0x18));

            long cand = (long)BinaryPrimitives.ReadUInt64LittleEndian(head.AsSpan(0x58));
            if (HasMagic(fs, cand)) return cand;
        }

        for (int off = 0x10; off + 8 <= head.Length; off += 8)
        {
            long cand = (long)BinaryPrimitives.ReadUInt64LittleEndian(head.AsSpan(off));
            if (cand >= 0x10000 && (cand & 0xFFF) == 0 && HasMagic(fs, cand)) return cand;
        }
        return -1;
    }

    static bool HasMagic(FileStream fs, long off)
    {
        if (off <= 0) return false;
        byte[] m = ReadAt(fs, off, 4);
        return m.Length == 4 && m[0] == 0x7F && m[1] == (byte)'C' && m[2] == (byte)'N' && m[3] == (byte)'T';
    }

    static byte[] ReadAt(FileStream fs, long off, long len)
    {
        if (off < 0 || len <= 0 || len > 64 * 1024 * 1024) return Array.Empty<byte>();
        try
        {
            fs.Seek(off, SeekOrigin.Begin);
            byte[] buf = new byte[len];
            long want = Math.Min(len, fs.Length - off);
            int got = 0;
            while (got < want)
            {
                int n = fs.Read(buf, got, (int)(want - got));
                if (n <= 0) break;
                got += n;
            }
            if (got < want) return Array.Empty<byte>();
            return buf;
        }
        catch { return Array.Empty<byte>(); }
    }

    static string CStr(byte[] buf, int off, int max)
    {
        int end = Array.IndexOf(buf, (byte)0, off, Math.Min(max, buf.Length - off));
        if (end < 0) end = off + max;
        return System.Text.Encoding.ASCII.GetString(buf, off, end - off);
    }
}
