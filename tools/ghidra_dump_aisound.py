# libaisound_jni.so 定向导出 v2 — 一次 headless 会话拿全:
#   A. JNI_OnLoad 反编译 (RegisterNatives 调用点)
#   B. JNI 方法表提取: 扫数据段中指向 .rodata 方法名字符串的指针三元组
#      {name*, sig*, fn*}, sig 以 '(' 开头校验, 得到每个 native 的真实地址
#   C. 关键 native 反编译: open/speak/speakStream/setStyle/lastPanic
#   D. 取证字符串 (AIRZ/AIRM/open: unpack failed 等) 引用定位,
#      并反编译 open 实现链 (AIRM 解压框架在此)
# 运行: analyzeHeadless <proj> aisound -import libaisound_jni.so \
#         -scriptPath <本目录> -postScript ghidra_dump_aisound.py
#@category AiSound
#@runtime PyGhidra

import os

OUT = os.path.join(os.path.expanduser("~"), "aisound-re-dump.txt")

fm = currentProgram.getFunctionManager()
mem = currentProgram.getMemory()
ref_mgr = currentProgram.getReferenceManager()

lines = ["=== 函数总数: %d ===" % fm.getFunctionCount()]

from ghidra.app.decompiler import DecompInterface
from ghidra.util.task import ConsoleTaskMonitor

di = DecompInterface()
di.openProgram(currentProgram)
mon = ConsoleTaskMonitor()


def decomp_to(f, seconds):
    if not f:
        return "(无函数)"
    r = di.decompileFunction(f, seconds, mon)
    if r.decompileCompleted():
        return r.getDecompiledFunction().getC()
    return "(反编译失败: %s)" % r.getErrorMessage()


def fn_at(addr):
    return fm.getFunctionContaining(addr)


def read_cstr(addr, cap=240):
    out = []
    for _ in range(cap):
        b = mem.getByte(addr) & 0xFF
        if b == 0:
            break
        out.append(chr(b) if 32 <= b < 127 else ".")
        addr = addr.add(1)
    return "".join(out)


def addr_of(v):
    return currentProgram.getAddressFactory().getAddress(
        "%x" % (v & 0xFFFFFFFFFFFFFFFF))


# ---------- .rodata 原始字节 ----------
rod = mem.getBlock(".rodata")
rod_base, rod_size = rod.getStart(), rod.getSize()
buf = bytearray(rod_size)
ok = False
try:
    mem.getBytes(rod_base, buf)
    ok = True
except Exception as e:
    lines.append(".rodata 读取失败: %s" % e)
lines.append(".rodata: %s 大小 %d 读取 %s" % (rod_base, rod_size, "OK" if ok else "失败"))


def find_all(pattern):
    res = []
    if not ok:
        return res
    pb = pattern.encode("latin-1")
    start = 0
    while True:
        i = buf.find(pb, start)
        if i < 0:
            break
        res.append(rod_base.add(i))
        start = i + 1
    return res


# ---------- B. JNI 方法表 ----------
method_names = ["speakStream", "lastPanic", "setStyle", "applyGain", "stretch",
                "voiceFx", "trimNew", "trimPush", "trimFinish", "trimFree",
                "trimEndClause", "speedNew", "speedPush", "speedFlush",
                "speedFree", "loudness", "speak", "open", "close"]

ptr_set = {}
for n in method_names:
    for a in find_all(n):
        ptr_set.setdefault(a.getOffset() & 0xFFFFFFFFFFFFFFFF, n)

rows = []
if ok and ptr_set:
    for blk in mem.getBlocks():
        if not blk.isInitialized() or blk.getName() in (".text", ".plt"):
            continue
        if blk.getSize() > 8 * 1024 * 1024:
            continue
        a, end = blk.getStart(), blk.getEnd()
        while a.compareTo(end) < 0:
            try:
                v = mem.getLong(a) & 0xFFFFFFFFFFFFFFFF
            except Exception:
                a = a.add(8)
                continue
            if v in ptr_set:
                try:
                    sig = read_cstr(addr_of(mem.getLong(a.add(8))))
                    if sig.startswith("("):
                        fn_addr = addr_of(mem.getLong(a.add(16)))
                        f = fn_at(fn_addr)
                        rows.append((a.getOffset(), ptr_set[v], sig,
                                     str(fn_addr), f.getName() if f else "?"))
                except Exception:
                    pass
            a = a.add(8)

rows.sort()
lines.append("\n=== JNI 方法表 (%d 行) ===" % len(rows))
for aoff, n, sig, fa, fname in rows:
    lines.append("0x%x  %-14s sig=%-50s fn=%s (%s)" % (aoff, n, sig[:48], fa, fname))

# ---------- C. 关键 native 反编译 ----------
decomp_targets = {}
for aoff, n, sig, fa, fname in rows:
    if n in ("open", "speak", "speakStream", "setStyle", "lastPanic"):
        f = fn_at(addr_of(int(fa, 16)))
        if f:
            decomp_targets.setdefault(n, f)

for s in currentProgram.getSymbolTable().getSymbols("JNI_OnLoad"):
    lines.append("\n===== JNI_OnLoad @ %s =====" % s.getAddress())
    lines.append(decomp_to(fm.getFunctionContaining(s.getAddress()), 300))

for n in sorted(decomp_targets):
    f = decomp_targets[n]
    lines.append("\n===== native %s → %s =====" % (n, f.getName()))
    lines.append(decomp_to(f, 600))

# ---------- D. 取证字符串引用 ----------
forensic = ["AIRZ", "AIRM", "IsRe", "open: unpack failed",
            "convert_byte_array", "panicked at "]
for t in forensic:
    addrs = find_all(t)
    lines.append("\n===== '%s' 出现 %d 次 =====" % (t, len(addrs)))
    dumped = 0
    dumped_names = set()
    for a in addrs:
        for ref in ref_mgr.getReferencesTo(a):
            f = fn_at(ref.getFromAddress())
            tag = f.getName() if f else "?"
            lines.append("  @%s ← %s (%s)" % (a, tag, ref.getReferenceType()))
            if f and t == "open: unpack failed" and tag not in dumped_names and dumped < 3:
                dumped_names.add(tag)
                lines.append(decomp_to(f, 600))
                dumped += 1
        if dumped >= 3:
            break

with open(OUT, "w", encoding="utf-8") as fh:
    fh.write("\n".join(lines))
print("[aisound-dump] 已写入 %s (%d 行)" % (OUT, len(lines)))
for l in lines:
    if "===" in l or "fn=" in l or "←" in l:
        print(l)
