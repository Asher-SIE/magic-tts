// aisound 第三轮定向反编译 — AIRM 解包器与 speak 派发:
//   FUN_008d440c  irf 解包 (open mode 1-5 分支, "open: unpack failed" 来源) ★
//   FUN_008d3f9c  irf 装载 (open mode 6-8, FUN_0059ae20 的实现)
//   FUN_008d49bc  AIRZ 内嵌 blob 解析器 (zd_pe_image / lift rt)
//   FUN_0025b384  引擎 speak 派发 (speakStream 内层)
//   FUN_0025bdf8  native setStyle (rate/pitch 域换算线索)
//   FUN_0026a154  mode 4/5 音色选择器
//@category AiSound

import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;

import java.io.PrintWriter;
import java.util.*;

public class ghidra_dump_aisound3 extends GhidraScript {

    @Override
    public void run() throws Exception {
        String out = System.getProperty("user.home") + "\\aisound-re-dump3.txt";
        DecompInterface di = new DecompInterface();
        di.openProgram(currentProgram);
        List<String> lines = new ArrayList<>();

        long[] targets = {0x008d440cL, 0x008d3f9cL, 0x008d49bcL,
                0x0025b384L, 0x0025bdf8L, 0x0026a154L};
        for (long t : targets) {
            Address a = toAddr(t);
            Function f = currentProgram.getFunctionManager().getFunctionAt(a);
            if (f == null) f = currentProgram.getFunctionManager().getFunctionContaining(a);
            lines.add("\n===== 0x" + Long.toHexString(t)
                    + " → " + (f == null ? "?" : f.getName()) + " =====");
            if (f == null) continue;
            DecompileResults r = di.decompileFunction(f, 600, monitor);
            lines.add(r.decompileCompleted()
                    ? r.getDecompiledFunction().getC()
                    : "(反编译失败: " + r.getErrorMessage() + ")");
        }

        try (PrintWriter pw = new PrintWriter(out, "UTF-8")) {
            for (String l : lines) pw.println(l);
        }
        println("[aisound-dump3] 已写入 " + out + " (" + lines.size() + " 行)");
    }
}
