# 定向导出脚本: 函数清单 + JNI_OnLoad 反编译 + 关键字符串引用函数
# 用途: 解 libaisound_jni.so 的 RegisterNatives 表与 open() 解压框架
# 运行: analyzeHeadless <proj> tmp -import <so> -postScript dump_aisound.py
#@category AiSound
#@runtime PyGhidra

import os

OUT = r"C:\Tools\aisound-re\dump.txt"
os.makedirs(os.path.dirname(OUT), exist_ok=True)

fm = currentProgram.getFunctionManager()
st = currentProgram.getSymbolTable()

lines = []
lines.append("=== 函数总数: %d ===" % fm.getFunctionCount())

# 1) JNI_OnLoad 反编译 (RegisterNatives 表在此构建)
from ghidra.app.decompiler import DecompInterface
from ghidra.util.task import ConsoleTaskMonitor
di = DecompInterface()
di.openProgram(currentProgram)
mon = ConsoleTaskMonitor()

def decomp(addr):
    f = fm.getFunctionContaining(addr)
    if not f:
        return "(无函数)"
    r = di.decompileFunction(f, 180, mon)
    if r.decompileCompleted():
        return r.getDecompiledFunction().getC()
    return "(反编译失败: %s)" % r.getErrorMessage()

onload = st.getSymbols("JNI_OnLoad")
for s in onload:
    lines.append("\n===== JNI_OnLoad @ %s =====" % s.getAddress())
    lines.append(decomp(s.getAddress()))

# 2) 引用关键字符串的函数 (定位 open/解压实现)
targets = ["AIRZ", "AIRM", "open: unpack failed", "speakStream",
           "convert_byte_array", "lastPanic"]
listing = currentProgram.getListing()
ref_mgr = currentProgram.getReferenceManager()
for t in targets:
    lines.append("\n===== 字符串 '%s' 的引用 =====" % t)
    found = False
    it = listing.getDefinedData(True)
    for d in it:
        v = d.getValue()
        if v and t in str(v):
            for ref in ref_mgr.getReferencesTo(d.getAddress()):
                f = fm.getFunctionContaining(ref.getFromAddress())
                if f:
                    lines.append("  %s @ %s <- %s" % (str(v)[:60], d.getAddress(), f.getName()))
                    found = True
            if found:
                break
    if not found:
        lines.append("  (未找到)")

# 3) 全函数清单 (地址+名, 供后续定位)
lines.append("\n=== 函数清单 (前 3000) ===")
n = 0
for f in fm.getFunctions(True):
    lines.append("%s  %s" % (f.getEntryPoint(), f.getName()))
    n += 1
    if n >= 3000:
        break

with open(OUT, "w", encoding="utf-8") as fh:
    fh.write("\n".join(lines))
print("已写入 %s" % OUT)
