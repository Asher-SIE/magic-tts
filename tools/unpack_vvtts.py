#!/usr/bin/env python3
"""VVTTS (com.vvtts.lan) 引擎解包器 — 阶段 1 逆向成果 (2026-10-17).

VVTTS 的 APK 里没有引擎本体文件: IBM ViaVoice TTS 中文版的 6 个 Windows
i386 DLL 被打包进 libq4.so (静态 i386 解包器/PE 加载器) 尾部, 运行时解到
files/d1 再由 libz9.so (= Box64 arm64 v0.4.5) 模拟执行。本脚本在宿主侧
完成同样解包, 不需要运行任何 Android/i386 代码。

容器格式 (逆向自 libq4.so unpack_container @ 0x805c750):
  [打包数据 + 条目表 (整体逐字节 XOR 混淆)][尾 8B: u32 目录大小 LE + u32 魔数 0xC26B3DA7]
  混淆 keystream: plain[i] = enc[i] ^ (((i >> 8) ^ (i * 31)) & 0xFF) ^ 0x5B
                  (i 从目录数据区首字节起算; 头 8 字节头除外)
  目录数据区: [9B 头: 魔数 9E 3B C7 51 01 + 条目数 N + 2B 零]
             + N × 条目 [type:1][nlen:1][name:nlen][size:4 LE][crc32:4 LE]
             (size ≤ 0x17FFFFF, 256 对齐故低字节恒 0; type=0 → main dll 标记)
  条目表之后: 单条 raw deflate 流, 解压 = 全部文件按条目顺序无缝拼接,
             逐段 crc32 校验。

用法: python3 unpack_vvtts.py <libq4.so 路径> <输出目录>
"""

import struct
import sys
import zlib
import os

DIR_MAGIC0 = bytes([0x9E, 0x3B, 0xC7, 0x51, 0x01])
TAIL_MAGIC = 0xC26B3DA7
MAX_FILE = 0x17FFFFF


def deobfuscate(buf: bytes) -> bytes:
    out = bytearray(len(buf))
    for i, b in enumerate(buf):
        out[i] = b ^ (((i >> 8) ^ (i * 31)) & 0xFF) ^ 0x5B
    return bytes(out)


def unpack(libq4_path: str, out_dir: str) -> None:
    data = open(libq4_path, 'rb').read()
    n = len(data)
    lo, hi = struct.unpack('<HH', data[n - 8:n - 4])
    magic, = struct.unpack('<I', data[n - 4:n])
    assert magic == TAIL_MAGIC, f'尾魔数不符: 0x{magic:08x}'
    t = (hi << 16) | lo
    dstart = n - 8 - t
    assert dstart >= 0, '目录大小越界'

    header = data[dstart:dstart + 8]           # 原始 (未混淆): 5B 魔数 + N + 2B 零
    dec = deobfuscate(data[dstart + 8:n - 8])  # 去混淆区: 条目表 + 打包数据
    assert header[:5] == DIR_MAGIC0, f'目录头魔数不符: {header[:5].hex()}'
    count = header[5]

    pos, entries = 0, []
    while pos + 10 <= len(dec) and dec[pos] in (0, 1) and len(entries) < count:
        typ, nlen = dec[pos], dec[pos + 1]
        if not (1 <= nlen <= 63):
            break
        p = pos + 2 + nlen
        name = dec[pos + 2:p].decode()
        size, = struct.unpack('<I', dec[p:p + 4])
        crc, = struct.unpack('<I', dec[p + 4:p + 8])
        if size == 0 or size > MAX_FILE:
            break
        entries.append((typ, name, size, crc))
        pos = p + 8

    total = sum(e[2] for e in entries)
    print(f'{len(entries)} 个条目, 共 {total} 字节解压数据')
    for t_, name, size, crc in entries:
        print(f'  type={t_} {name:14s} size={size:8d} crc=0x{crc:08x}')

    d = zlib.decompressobj(-15)
    out = d.decompress(dec[pos:]) + d.flush()
    assert len(out) == total, f'解压 {len(out)} != 期望 {total}'

    os.makedirs(out_dir, exist_ok=True)
    off = 0
    for t_, name, size, crc in entries:
        seg = out[off:off + size]
        ok = (zlib.crc32(seg) & 0xFFFFFFFF) == crc
        print(f'  {name:14s} {"OK" if ok else "CRC FAIL"}')
        assert ok, name
        open(os.path.join(out_dir, name), 'wb').write(seg)
        off += size


if __name__ == '__main__':
    unpack(sys.argv[1], sys.argv[2])
