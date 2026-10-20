// aisound 第四轮 — 密码与解压本体 (离线提取数据的最后一环):
//   FUN_008d3acc  密码状态初始化 (u64 key → state)
//   FUN_008d40a8  解密 payload (流密码本体)
//   FUN_008d5ee0  LZMA 解压包装 (lzma-rs 调用形态/参数)
//   FUN_008d58e8  顶层块的二次密钥推导 (FUN_008d440c 内调用)
//@category AiSound

import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;

import java.io.PrintWriter;
import java.util.*;

public class ghidra_dump_aisound4 extends GhidraScript {

    @Override
    public void run() throws Exception {
        String out = System.getProperty("user.home") + "\\aisound-re-dump4.txt";
        DecompInterface di = new DecompInterface();
        di.openProgram(currentProgram);
        List<String> lines = new ArrayList<>();

        long[] targets = {0x008d3accL, 0x008d40a8L, 0x008d5ee0L, 0x008d58e8L};
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
            // 这些函数小, 顺带附汇编首 60 条便于核对立即数
            lines.add("--- 汇编 (首 60 条) ---");
            var it = currentProgram.getListing().getInstructions(f.getEntryPoint(), true);
            int n = 0;
            while (it.hasNext() && n < 60) {
                lines.add("  " + it.next().toString());
                n++;
            }
        }

        try (PrintWriter pw = new PrintWriter(out, "UTF-8")) {
            for (String l : lines) pw.println(l);
        }
        println("[aisound-dump4] 已写入 " + out + " (" + lines.size() + " 行)");
    }
}
