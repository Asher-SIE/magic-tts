// aisound 第二轮定向反编译 — 已知地址直取 (复用已分析工程, -process 模式):
//   FUN_0059ae20  Resource.irf 解析入口 (native open 的核心调用)
//   FUN_0059b1a0  引擎上下文构建
//   FUN_005976c4 / FUN_00597794 / FUN_005a01b0 / FUN_005a0270  AIRZ 块处理
//   上述函数的调用者 (一层), 定位容器遍历器
//   FUN_0025d07c  native speakStream (流式合成入口)
//@category AiSound

import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Reference;

import java.io.PrintWriter;
import java.util.*;

public class ghidra_dump_aisound2 extends GhidraScript {

    private DecompInterface di;
    private final List<String> lines = new ArrayList<>();
    private final Set<Long> dumped = new HashSet<>();

    private Function fAt(long off) {
        Address a = toAddr(off);
        Function f = currentProgram.getFunctionManager().getFunctionAt(a);
        if (f == null) f = currentProgram.getFunctionManager().getFunctionContaining(a);
        return f;
    }

    private String decomp(Function f, int seconds) {
        if (f == null) return "(无函数)";
        if (!dumped.add(f.getEntryPoint().getOffset())) return "(已反编译, 跳)";
        DecompileResults r = di.decompileFunction(f, seconds, monitor);
        if (r.decompileCompleted()) return r.getDecompiledFunction().getC();
        return "(反编译失败: " + r.getErrorMessage() + ")";
    }

    private List<Function> callersOf(Function f) {
        List<Function> out = new ArrayList<>();
        if (f == null) return out;
        for (Reference ref : currentProgram.getReferenceManager()
                .getReferencesTo(f.getEntryPoint())) {
            Function c = currentProgram.getFunctionManager()
                    .getFunctionContaining(ref.getFromAddress());
            if (c != null) out.add(c);
        }
        return out;
    }

    @Override
    public void run() throws Exception {
        String out = System.getProperty("user.home") + "\\aisound-re-dump2.txt";
        di = new DecompInterface();
        di.openProgram(currentProgram);

        long[] targets = {0x0059ae20L, 0x0059b1a0L, 0x005976c4L, 0x00597794L,
                0x005a01b0L, 0x005a0270L, 0x0025d07cL};
        for (long t : targets) {
            Function f = fAt(t);
            lines.add("\n===== 目标 0x" + Long.toHexString(t)
                    + " → " + (f == null ? "?" : f.getName()) + " =====");
            lines.add(decomp(f, 600));

            if (t != 0x0025d07cL) {   // speakStream 的调用者是 JNI 层, 不展开
                for (Function c : callersOf(f)) {
                    lines.add("\n--- 调用者 0x" + c.getEntryPoint() + " "
                            + c.getName() + " ---");
                    lines.add(decomp(c, 600));
                }
            }
        }

        try (PrintWriter pw = new PrintWriter(out, "UTF-8")) {
            for (String l : lines) pw.println(l);
        }
        println("[aisound-dump2] 已写入 " + out + " (" + lines.size() + " 行)");
    }
}
