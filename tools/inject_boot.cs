// inject_boot.cs —— 把无障碍补丁的启动脚本注入《千恋＊万花》的 data.xp3。
//
// 为什么需要它：本作引擎不会从别处加载这个补丁。外挂补丁档
// （a11y.xp3 / patch0.xp3 / patch.xp3 / patch_extra.xp3）与散装同名脚本
// 全部实测无效，唯一入口是 data.xp3 里的 startup.tjs 槽位。
// 本工具只改这一个条目，其余内容原样拷贝，输入文件不动。
//
// 硬约束：内层 hnfn 索引**逐字节照抄**，绝不重建。该归档有十几组同名 adlr
// 哈希，按哈希重建会静默丢文件（实测丢过 18 个 storage，症状是剧本引用到时
// 抛 "Cannot convert the variable type"）。
//
// 编译（.NET Framework 自带 csc，无需任何 SDK）：
//   %WINDIR%\Microsoft.NET\Framework\v4.0.30319\csc.exe /nologo /target:exe ^
//       /out:inject_boot.exe /r:System.Windows.Forms.dll inject_boot.cs

using System;
using System.Collections.Generic;
using System.IO;
using System.IO.Compression;
using System.Text;
using System.Windows.Forms;

static class InjectBoot
{
    // 启动脚本字节码：tjs2c 编译 data/startup.tjs 后按 0x01 混淆的落盘形态。
    // 占位符 BOOT_B64_PLACEHOLDER 由 tools/build_injector.ps1 替换为真实内容。
    const string BOOT_B64 = "BOOT_B64_PLACEHOLDER";

    const string BOOT_NAME = "startup.tjs";

    // ------------------------------------------------------------ zlib

    static uint Adler32(byte[] d)
    {
        uint a = 1, b = 0;
        for (int i = 0; i < d.Length; i++)
        {
            a = (a + d[i]) % 65521;
            b = (b + a) % 65521;
        }
        return (b << 16) | a;
    }

    /// zlib 压缩（对应 Python 的 zlib.compress(data)，即**默认级别 6**）。
    /// FLG 字节必须是 0x9C 而不是 0xDA —— 后者表示级别 9。
    /// 压缩率上 .NET 的 DeflateStream 达不到 zlib 9 级，所以两边都用级别 6 语义，
    /// 这样本工具与 tools/inject_boot.py 的产物逐字节一致（已验证）。
    static byte[] ZlibCompress(byte[] data)
    {
        var ms = new MemoryStream();
        using (var ds = new DeflateStream(ms, CompressionLevel.Optimal, true))
            ds.Write(data, 0, data.Length);
        byte[] def = ms.ToArray();

        byte[] outb = new byte[2 + def.Length + 4];
        outb[0] = 0x78; outb[1] = 0x9C;             // zlib 头，级别 6
        Buffer.BlockCopy(def, 0, outb, 2, def.Length);
        uint ad = Adler32(data);
        outb[outb.Length - 4] = (byte)(ad >> 24);
        outb[outb.Length - 3] = (byte)(ad >> 16);
        outb[outb.Length - 2] = (byte)(ad >> 8);
        outb[outb.Length - 1] = (byte)ad;
        return outb;
    }

    /// zlib 解压。len 是**整段 zlib 数据的长度**（含 2 字节头与 4 字节 adler32）。
    /// 实测得到的正确配置（三种组合里只有这一种对）：
    ///   跳过 2 字节 zlib 头，长度给完整的 len。
    /// ① 不能把长度减 6 —— .NET 的 DeflateStream 会忽略尾部 adler32，
    ///    减 6 会把数据尾巴砍掉（实测少 5 字节）；
    /// ② 不能从偏移 0 开始 —— 它不认 zlib 头，会抛 InvalidDataException。
    static byte[] ZlibDecompress(byte[] src, int off, int len)
    {
        var ms = new MemoryStream(src, off + 2, len - 2);
        var outMs = new MemoryStream();
        using (var ds = new DeflateStream(ms, CompressionMode.Decompress))
        {
            byte[] buf = new byte[1 << 16];
            int n;
            while ((n = ds.Read(buf, 0, buf.Length)) > 0) outMs.Write(buf, 0, n);
        }
        return outMs.ToArray();
    }

    // ------------------------------------------------------------ 读写小工具

    static uint U32(byte[] b, int p) { return BitConverter.ToUInt32(b, p); }
    static ulong U64(byte[] b, int p) { return BitConverter.ToUInt64(b, p); }
    static string Tag(byte[] b, int p)
    {
        return new string(new char[] { (char)b[p], (char)b[p + 1], (char)b[p + 2], (char)b[p + 3] });
    }
    static void PutU32(byte[] b, int p, uint v)
    { b[p] = (byte)v; b[p + 1] = (byte)(v >> 8); b[p + 2] = (byte)(v >> 16); b[p + 3] = (byte)(v >> 24); }
    static void PutU64(byte[] b, int p, ulong v)
    { for (int i = 0; i < 8; i++) b[p + i] = (byte)(v >> (8 * i)); }

    class Rec
    {
        public int pos, end;
        public uint adlr;
        public int segmQ = -1, infoP = -1;
        public List<ulong[]> segs = new List<ulong[]>();   // {flag, off, org, arc}
    }

    static List<Rec> WalkOuter(byte[] outer)
    {
        var res = new List<Rec>();
        int p = 52;                                        // 'sen:' 头 + 4 个字段 + 名称
        while (p + 12 <= outer.Length && Tag(outer, p) == "File")
        {
            int end = (int)(p + 12 + (long)U64(outer, p + 4));
            var r = new Rec { pos = p, end = end };
            int q = p + 12;
            while (q + 12 <= end)
            {
                string m = Tag(outer, q);
                int s = (int)U64(outer, q + 4);
                if (m == "adlr") r.adlr = U32(outer, q + 12);
                else if (m == "segm")
                {
                    r.segmQ = q + 12;
                    for (int i = 0; i < s / 28; i++)
                    {
                        int so = q + 12 + 28 * i;
                        r.segs.Add(new ulong[] { U32(outer, so), U64(outer, so + 4),
                                                 U64(outer, so + 12), U64(outer, so + 20) });
                    }
                }
                else if (m == "info") r.infoP = q + 12;
                q += 12 + s;
            }
            res.Add(r);
            p = end;
        }
        return res;
    }

    static Dictionary<uint, string> WalkInner(byte[] inner)
    {
        var map = new Dictionary<uint, string>();
        int pos = 0;
        string magic = Tag(inner, 0);
        while (pos < inner.Length - 18)
        {
            if (Tag(inner, pos) != magic)
            {
                int k = 1;
                while (k < 8 && pos + k + 4 <= inner.Length && Tag(inner, pos + k) != magic) k++;
                if (k >= 8) break;
                pos += k;
            }
            int csz = (int)U64(inner, pos + 4);
            uint adlr = U32(inner, pos + 12);
            int nl = inner[pos + 16] | (inner[pos + 17] << 8);
            string name = Encoding.Unicode.GetString(inner, pos + 18, nl * 2);
            if (!map.ContainsKey(adlr)) map[adlr] = name;
            pos += 12 + csz;
        }
        return map;
    }

    static byte[] ReadStored(byte[] src, Rec r)
    {
        var ms = new MemoryStream();
        foreach (ulong[] sg in r.segs)
        {
            int off = (int)sg[1], org = (int)sg[2], arc = (int)sg[3];
            if (arc == org) ms.Write(src, off, arc);
            else { byte[] d = ZlibDecompress(src, off, arc); ms.Write(d, 0, d.Length); }
        }
        return ms.ToArray();
    }

    // ------------------------------------------------------------ 主流程

    [STAThread]
    static int Main(string[] args)
    {
        try { Console.OutputEncoding = Encoding.UTF8; } catch { }

        string dataPath, outPath;
        bool interactive = args.Length < 2;

        if (!interactive) { dataPath = args[0]; outPath = args[1]; }
        else
        {
            Console.WriteLine("=== 千恋＊万花 无障碍补丁 —— 安装工具 ===");
            Console.WriteLine();
            Console.WriteLine("本工具把补丁的启动脚本注入你自己的 data.xp3，生成一个新文件。");
            Console.WriteLine("只改启动脚本这一个条目，其余内容原样保留，你的原档不会被改动。");
            Console.WriteLine();
            Console.WriteLine("按任意键继续，然后选择游戏目录下的 data.xp3。");
            Console.ReadKey(true);

            using (var d = new OpenFileDialog())
            {
                d.Title = "第一步：选择游戏目录下的 data.xp3";
                d.Filter = "游戏数据归档 (data.xp3)|data.xp3|所有文件 (*.*)|*.*";
                if (d.ShowDialog() != DialogResult.OK) { Console.WriteLine("已取消。"); return 1; }
                dataPath = d.FileName;
            }
            Console.WriteLine("输入：" + dataPath);
            Console.WriteLine();

            using (var d = new SaveFileDialog())
            {
                d.Title = "第二步：保存生成的新文件";
                d.Filter = "游戏数据归档 (*.xp3)|*.xp3";
                d.InitialDirectory = Path.GetDirectoryName(dataPath);
                d.FileName = "data.patched.xp3";
                if (d.ShowDialog() != DialogResult.OK) { Console.WriteLine("已取消。"); return 1; }
                outPath = d.FileName;
            }
            Console.WriteLine("输出：" + outPath);
            Console.WriteLine();
        }

        if (!File.Exists(dataPath)) { Console.WriteLine("找不到文件：" + dataPath); return 1; }
        if (string.Equals(Path.GetFullPath(dataPath), Path.GetFullPath(outPath),
                          StringComparison.OrdinalIgnoreCase))
        {
            Console.WriteLine("输出文件不能与输入相同 —— 请不要覆盖你的原档。");
            return 1;
        }
        if (BOOT_B64 == "BOOT_B64_PLACEHOLDER")
        {
            Console.WriteLine("这个 exe 是未嵌入启动脚本的半成品，无法使用。");
            return 1;
        }

        byte[] boot = Convert.FromBase64String(BOOT_B64);
        string hdr = Encoding.ASCII.GetString(boot, 0, Math.Min(7, boot.Length));
        if (hdr == "TJS2100") { /* 明文 */ }
        else if (hdr == "UKR3011") { for (int i = 0; i < boot.Length; i++) boot[i] ^= 0x01; }
        else { Console.WriteLine("启动脚本字节码无效（头部应为 TJS2100）。"); return 1; }

        Console.WriteLine("读取归档…");
        byte[] src = File.ReadAllBytes(dataPath);
        Console.WriteLine("  {0:N0} 字节", src.Length);

        int io = (int)U64(src, 0x20);
        int csize = (int)U64(src, io + 1);
        byte[] outer;
        try { outer = ZlibDecompress(src, io + 17, csize); }
        catch (Exception ex)
        {
            Console.WriteLine("这个归档读不出来（{0}）—— 可能不是本作的 data.xp3，或已损坏。", ex.GetType().Name);
            return 1;
        }
        int coff = (int)U64(outer, 12);
        int carc = (int)U32(outer, 24);
        byte[] inner;
        try { inner = ZlibDecompress(src, coff, carc); }
        catch (Exception ex)
        {
            Console.WriteLine("这个归档的索引读不出来（{0}）。", ex.GetType().Name);
            return 1;
        }

        List<Rec> oc = WalkOuter(outer);
        Dictionary<uint, string> nameByAdlr = WalkInner(inner);
        Console.WriteLine("  外层索引 {0:N0} 字节，内层索引 {1:N0} 字节，{2} 个条目",
                          outer.Length, inner.Length, oc.Count);

        Rec bootRec = null;
        foreach (Rec r in oc)
        {
            string nm;
            if (nameByAdlr.TryGetValue(r.adlr, out nm) && nm == BOOT_NAME) { bootRec = r; break; }
        }
        if (bootRec == null)
        {
            Console.WriteLine();
            Console.WriteLine("这个归档里没有 " + BOOT_NAME + " —— 可能不是本作的 data.xp3，或已被改过。");
            return 1;
        }

        byte[] orig = ReadStored(src, bootRec);
        int key = orig.Length > 7 ? orig[7] : 1;
        byte[] stored = new byte[boot.Length];
        for (int i = 0; i < boot.Length; i++) stored[i] = (byte)(boot[i] ^ key);
        Console.WriteLine("  启动脚本 {0} 字节字节码 → 落盘 {1} 字节（XOR key 0x{2:X2}）",
                          boot.Length, stored.Length, key);
        Console.WriteLine();

        Console.WriteLine("写出…");
        byte[] innerZ = ZlibCompress(inner);                // 内层索引原样重压
        byte[] newOuters;
        long innerOff, tailOff;

        using (var fs = new FileStream(outPath, FileMode.Create, FileAccess.Write))
        {
            fs.Write(new byte[40], 0, 40);
            fs.Write(src, 0x28, 0x30);                      // 48 字节诱饵 PNG
            long cur = 0x58;
            var outerParts = new List<byte[]>(oc.Count);

            foreach (Rec r in oc)
            {
                byte[] fc = new byte[r.end - r.pos];
                Buffer.BlockCopy(outer, r.pos, fc, 0, fc.Length);
                if (r.adlr == bootRec.adlr)
                {
                    int q = r.segmQ - r.pos;
                    PutU32(fc, q, 0);
                    PutU64(fc, q + 4, (ulong)cur);
                    PutU64(fc, q + 12, (ulong)stored.Length);
                    PutU64(fc, q + 20, (ulong)stored.Length);
                    int ip = r.infoP - r.pos;
                    PutU64(fc, ip + 4, (ulong)stored.Length);
                    PutU64(fc, ip + 12, (ulong)stored.Length);
                    fs.Write(stored, 0, stored.Length);
                    cur += stored.Length;
                }
                else
                {
                    for (int i = 0; i < r.segs.Count; i++)
                    {
                        int off = (int)r.segs[i][1], arc = (int)r.segs[i][3];
                        PutU64(fc, r.segmQ - r.pos + 28 * i + 4, (ulong)cur);
                        fs.Write(src, off, arc);
                        cur += arc;
                    }
                }
                outerParts.Add(fc);
            }

            innerOff = cur;
            fs.Write(innerZ, 0, innerZ.Length);
            cur += innerZ.Length;

            var head = new MemoryStream();
            byte[] sen = Encoding.ASCII.GetBytes("sen:");
            head.Write(sen, 0, 4);
            WriteU64(head, 40);
            WriteU64(head, (ulong)innerOff);
            WriteU32(head, (uint)inner.Length);
            WriteU32(head, (uint)innerZ.Length);
            WriteU16(head, 10);
            byte[] keyBlob = new byte[] { 0x43, 0x53, 0x4B, 0x60, 0x0A, 0xFF, 0x07, 0x4E, 0xB1, 0x82 };
            head.Write(keyBlob, 0, keyBlob.Length);
            byte[] steam = Encoding.Unicode.GetBytes("Steam");
            head.Write(steam, 0, steam.Length);
            head.Write(new byte[2], 0, 2);                  // 结尾 00 00
            foreach (byte[] fc in outerParts) head.Write(fc, 0, fc.Length);

            byte[] newOuter = head.ToArray();
            byte[] outerZ = ZlibCompress(newOuter);
            tailOff = cur;                              // 尾部记录从这里开始
            // 注意：外层压缩数据**只写这一份**，就在尾部记录里。
            // 曾经在外面多写了一份，导致文件里有两份压缩数据、尾部头被挤到后面，
            // 表现是引擎读到的索引位置错位、启动脚本不生效。
            var tail = new MemoryStream();
            tail.WriteByte(1);                          // flag
            // 两个大小字段是**小端** u64，与 tools/inject_boot.py 一致（该版产物已实测可用）。
            // 曾误以为是大端并据此改过一次 —— 那是读字节序读错导致的误判。
            WriteU64(tail, (ulong)outerZ.Length);       // 压缩大小
            WriteU64(tail, (ulong)newOuter.Length);     // 原始大小
            tail.Write(outerZ, 0, outerZ.Length);
            byte[] tailBytes = tail.ToArray();
            fs.Write(tailBytes, 0, tailBytes.Length);

            // 头部：前 40 字节照抄，仅把 0x20 处的尾索引偏移改掉
            byte[] header = new byte[40];
            Buffer.BlockCopy(src, 0, header, 0, 40);
            PutU64(header, 0x20, (ulong)tailOff);
            fs.Seek(0, SeekOrigin.Begin);
            fs.Write(header, 0, 40);
        }

        Console.WriteLine();
        Console.WriteLine("完成：" + outPath);
        Console.WriteLine("  {0:N0} 字节", new FileInfo(outPath).Length);
        Console.WriteLine();
        Console.WriteLine("接下来：把它改名为 data.xp3，覆盖游戏目录里的同名文件。");
        Console.WriteLine("覆盖前请先把原来的 data.xp3 改名备份（那是唯一的还原手段）。");
        if (interactive)
        {
            Console.WriteLine();
            Console.WriteLine("按任意键退出。");
            Console.ReadKey(true);
        }
        return 0;
    }

    static void WriteU16(Stream s, ushort v) { s.WriteByte((byte)v); s.WriteByte((byte)(v >> 8)); }
    static void WriteU32(Stream s, uint v) { for (int i = 0; i < 4; i++) s.WriteByte((byte)(v >> (8 * i))); }
    static void WriteU64(Stream s, ulong v) { for (int i = 0; i < 8; i++) s.WriteByte((byte)(v >> (8 * i))); }
    // 尾记录专用：大端序
    static void WriteU64BE(Stream s, ulong v) { for (int i = 7; i >= 0; i--) s.WriteByte((byte)(v >> (8 * i))); }
}
