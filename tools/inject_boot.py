#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""把本补丁的启动脚本注入《千恋＊万花》的 data.xp3 —— 自包含的单文件工具。

    python inject_boot.py --data <游戏目录>\\data.xp3 --out data.patched.xp3

字节码默认读仓库里的 data/startup.tjs.bin（已入库、已实测可用），正常发布构建
不需要 tjs2c.exe。--bin 可以指定别的字节码文件；--script + --tjs2c 只是旧流程
兼容用法，仅当默认 .bin 不存在时才会用到。--bin 优先级最高。

只有纯标准库依赖（zlib / struct / hashlib），不需要装任何东西。

它只做一件事：把归档里 startup.tjs 这一个条目的内容换成我们的启动脚本。
其余内容**原样拷贝**，内层索引**逐字节照抄** —— 这一条是硬约束：
该归档的内层索引里有十几组同名 adlr 哈希，一旦按哈希重建就会静默丢文件
（实测丢过 18 个 storage，症状是剧本里引用到就抛
 "Cannot convert the variable type ((void) to Object)"）。

顺带说明为什么必须改 data.xp3：外挂补丁档（a11y.xp3 / patch0.xp3 /
patch.xp3 / patch_extra.xp3）与散装同名脚本都实测过，本作引擎一概不加载。
详见 docs/DESIGN.md 第 16 节。
"""
import argparse
import os
import struct
import subprocess
import sys
import tempfile
import zlib

BOOT_NAME = 'startup.tjs'
BOOT_MAGIC = b'TJS2100'
BOOT_XOR = 0x01          # 落盘混淆：TJS2100 <-> UKR3011

# 仓库内置的字节码：tools/ 的上一级 data/ 下（干净克隆即可用）.
DEFAULT_BIN = os.path.normpath(os.path.join(
    os.path.dirname(os.path.abspath(__file__)), '..', 'data', 'startup.tjs.bin'))


# ---------------------------------------------------------------- 归档解析

def chunks(buf, pos, end):
    out = []
    while pos + 12 <= end:
        m = bytes(buf[pos:pos + 4])
        s = struct.unpack_from('<Q', buf, pos + 4)[0]
        out.append((m, pos, s))
        pos += 12 + s
    return out


def walk_outer(outer):
    """遍历外层索引里的 File 块。头 52 字节 = 'sen:' 头(30) + 尾部的 4 个 u32/u64 + 10+4+2。"""
    res, pos = [], 52
    while pos + 12 <= len(outer) and outer[pos:pos + 4] == b'File':
        csz = struct.unpack_from('<Q', outer, pos + 4)[0]
        end = pos + 12 + csz
        rec = dict(pos=pos, end=end, subs=chunks(outer, pos + 12, end))
        for m, p, s in rec['subs']:
            if m == b'adlr':
                rec['adlr'] = struct.unpack_from('<I', outer, p + 12)[0]
            elif m == b'segm':
                rec['segm_q'] = p + 12
                rec['segs'] = [(struct.unpack_from('<I', outer, p + 12 + 28 * i)[0],
                                struct.unpack_from('<Q', outer, p + 12 + 28 * i + 4)[0],
                                struct.unpack_from('<Q', outer, p + 12 + 28 * i + 12)[0],
                                struct.unpack_from('<Q', outer, p + 12 + 28 * i + 20)[0])
                               for i in range(s // 28)]
            elif m == b'info':
                rec['info_p'] = p + 12
        res.append(rec)
        pos = end
    return res


def walk_inner(inner):
    res, pos = [], 0
    magic = inner[0:4]
    while pos < len(inner) - 18:
        if inner[pos:pos + 4] != magic:
            k = 1
            while k < 8 and pos + k + 4 <= len(inner) and inner[pos + k:pos + k + 4] != magic:
                k += 1
            if k >= 8:
                break
            pos += k
        csz = struct.unpack_from('<Q', inner, pos + 4)[0]
        nl = struct.unpack_from('<H', inner, pos + 16)[0]
        res.append(dict(adlr=struct.unpack_from('<I', inner, pos + 12)[0],
                        name=inner[pos + 18:pos + 18 + nl * 2].decode('utf-16le', 'replace')))
        pos += 12 + csz
    return res


def read_file(src, rec):
    out = b''
    for _fl, off, org, arc in rec['segs']:
        raw = src[off:off + arc]
        out += zlib.decompress(raw) if arc != org else raw
    return out


# ---------------------------------------------------------------- 编译

def compile_tjs(script_path, tjs2c):
    """把 .tjs 编译成 TJS2100 字节码。返回 raw 字节。"""
    if not tjs2c:
        raise SystemExit('需要 tjs2c.exe 来编译 .tjs —— 用 --tjs2c 指定；'
                         '通常不需要编译，直接让脚本读 data/startup.tjs.bin 即可')
    tmp = tempfile.mkdtemp(prefix='a11yboot')
    raw = os.path.join(tmp, 'boot.raw')
    r = subprocess.run([tjs2c, script_path, raw], capture_output=True)
    if not os.path.exists(raw):
        raise SystemExit('tjs2c 编译失败: %s' % (r.stdout + r.stderr).decode('utf-8', 'replace'))
    return open(raw, 'rb').read()


def read_bin(path):
    try:
        with open(path, 'rb') as f:
            return f.read()
    except OSError as e:
        raise SystemExit('读不了字节码文件 %s: %s' % (path, e))


def load_boot(a):
    """按优先级取字节码：--bin > 仓库内置 data/startup.tjs.bin > --script + --tjs2c."""
    if a.bin:
        print('字节码: %s（--bin）' % a.bin)
        return read_bin(a.bin)
    if os.path.exists(DEFAULT_BIN):
        print('字节码: %s（仓库内置）' % os.path.normpath(DEFAULT_BIN))
        return read_bin(DEFAULT_BIN)
    if a.script:
        return compile_tjs(a.script, a.tjs2c)
    raise SystemExit('找不到内置字节码 %s；请用 --bin 指定，或同时给 --script 与 --tjs2c'
                     % os.path.normpath(DEFAULT_BIN))


# ---------------------------------------------------------------- 主流程

def main():
    ap = argparse.ArgumentParser(
        description='把启动脚本字节码注入 data.xp3',
        epilog='字节码来源优先级：--bin > 仓库内置 data/startup.tjs.bin > '
               '--script + --tjs2c。默认用仓库内置 .bin，正常发布构建不需要 tjs2c。')
    ap.add_argument('--data', required=True, help='原始游戏 data.xp3')
    ap.add_argument('--out', required=True, help='输出的 data.xp3（不要覆盖原档）')
    ap.add_argument('--bin', help='已编译好的 TJS2100 字节码文件（默认 ../data/startup.tjs.bin）')
    ap.add_argument('--script', help='TJS 源码 startup.tjs（仅在默认 .bin 缺失时回退编译，需 --tjs2c）')
    ap.add_argument('--tjs2c', help='tjs2c.exe 路径（仅源码回退编译时使用）')
    a = ap.parse_args()

    boot = load_boot(a)
    # 接受两种形态：明文 TJS2100，或已按 0x01 混淆过的 UKR3011。
    if boot[:7] == b'TJS2100':
        pass
    elif boot[:7] == bytes(x ^ BOOT_XOR for x in BOOT_MAGIC):
        boot = bytes(x ^ BOOT_XOR for x in boot)
    else:
        raise SystemExit('不是 TJS2100 字节码，也不是它的 0x01 混淆形态（头部 %r）' % boot[:7])

    src = open(a.data, 'rb').read()
    io = struct.unpack_from('<Q', src, 0x20)[0]
    csize = struct.unpack_from('<Q', src, io + 1)[0]
    outer = zlib.decompress(src[io + 17:io + 17 + csize])
    coff = struct.unpack_from('<Q', outer, 12)[0]
    carc = struct.unpack_from('<I', outer, 24)[0]
    inner = zlib.decompress(src[coff:coff + carc])

    oc = walk_outer(outer)
    name_by_adlr = {}
    for e in walk_inner(inner):
        name_by_adlr.setdefault(e['adlr'], e['name'])

    rec = next((r for r in oc if name_by_adlr.get(r.get('adlr')) == BOOT_NAME), None)
    if rec is None:
        raise SystemExit('归档里找不到 %s' % BOOT_NAME)

    key = read_file(src, rec)[7]                      # 沿用该条目原本的 XOR key
    stored = bytes(x ^ key for x in boot)
    print('启动脚本: %d 字节字节码, XOR key 0x%02x, 落盘 %d 字节' % (len(boot), key, len(stored)))

    out = open(a.out, 'wb')
    out.write(b'\x00' * 40)
    out.write(src[0x28:0x58])                         # 48 字节诱饵 PNG，原样
    cur = 0x58
    new_outers = []
    for r in oc:
        fc = bytearray(outer[r['pos']:r['end']])
        if r.get('adlr') == rec['adlr']:
            q = r['segm_q'] - r['pos']
            struct.pack_into('<I', fc, q, 0)
            struct.pack_into('<Q', fc, q + 4, cur)
            struct.pack_into('<Q', fc, q + 12, len(stored))
            struct.pack_into('<Q', fc, q + 20, len(stored))
            ip = r['info_p'] - r['pos']
            struct.pack_into('<Q', fc, ip + 4, len(stored))
            struct.pack_into('<Q', fc, ip + 12, len(stored))
            out.write(stored)
            cur += len(stored)
        else:
            for i, (_fl, off, _org, arc) in enumerate(r['segs']):
                struct.pack_into('<Q', fc, r['segm_q'] - r['pos'] + 28 * i + 4, cur)
                out.write(src[off:off + arc])
                cur += arc
        new_outers.append(bytes(fc))

    # 内层索引逐字节照抄（重新压缩，但内容与顺序不变）。
    # 用级别 6 —— 与 C# 版 tools/inject_boot.cs 保持一致，两者产物可逐字节对比。
    # 级别 9 会省几百字节，但 .NET 的 DeflateStream 达不到那个压缩率，
    # 两边级别不同就没法互相校验了，不值得。
    inner_z = zlib.compress(inner, 6)
    inner_off = cur
    out.write(inner_z)
    new_outer = (b'sen:' + struct.pack('<Q', 40) + struct.pack('<Q', inner_off)
                 + struct.pack('<II', len(inner), len(inner_z)) + struct.pack('<H', 10)
                 + b'CSK\x60\x0a\xff\x07\x4e\xb1\x82' + 'Steam'.encode('utf-16le') + b'\x00\x00'
                 + b''.join(new_outers))
    outer_z = zlib.compress(new_outer, 6)
    tail = struct.pack('<BQQ', 1, len(outer_z), len(new_outer)) + outer_z
    tail_off = inner_off + len(inner_z)
    out.write(tail)

    header = bytearray(src[0:40])
    header[0x20:0x28] = struct.pack('<Q', tail_off)
    out.seek(0)
    out.write(bytes(header))
    out.close()
    print('写出 %s (%d 字节)' % (a.out, os.path.getsize(a.out)))


if __name__ == '__main__':
    main()
