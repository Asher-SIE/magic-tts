# VVTTS 调研笔记 (阶段 1 完结, 2026-10-17)

> 结论先行: VVTTS 的内核是 **IBM ViaVoice TTS 中文版 (Windows i386 PE, 完整
> ECI API)**, 被打包在 libq4.so 里、运行时靠 Box64 模拟执行。macOS 上**不存在
> 直接调用路径**, 唯一集成方式是 yongde 式纯 C 逆向重写, 工作量 ≥ 永德逆向。
> 引擎本体已解出 (CRC 全过), 解包器 `tools/unpack_vvtts.py` 可复现。

## 一、APK 分层结构 (全部实证)

| 文件 | 真身 |
|---|---|
| `lib/arm64-v8a/libz9.so` (24.6M) | **Box64 arm64 v0.4.5** 原样内嵌 (x86→ARM64 用户态转译器) |
| `lib/arm64-v8a/libq4.so` (2.1M, i386 静态 ELF, 未 strip) | 自制 daemon: PE 加载器 (`load_module_data`/`link_imports`) + zlib 解包器 (`unpack_container`/`inflate_raw`) + 协程调度 (`co_switch`/`co_schedule`), 尾部携带 ~1.9M 打包的 ViaVoice 引擎 |
| `smali/com/vvtts/lan/VvttsTextToSpeechService` | Android TTS 服务: ProcessBuilder 拉 daemon、stdin/stdout 协议、看门狗; assets/nvda 标点词典做文本预处理 |

## 二、解出的引擎 (→ `VVTTS/unpacked/`, 解包器逆向自 unpack_container)

| 文件 | 大小 | 角色 | 导出 |
|---|---|---|---|
| ibmeci.dll | 311K | **ECI 引擎主库** (type=0 "main dll") | **62 个标准 ECI API**: eciNew/eciNewEx/eciAddText/eciSynthesize/eciRegisterCallback/eciGetVoiceParam/eciLoadDict/eciGeneratePhonemes… |
| chs50.syn | 2.0M | 中文合成语言模块 (.text 1.5M 代码为主) | getObject |
| chsrom.dll | 1.3M | 中文语音库 ROM (.rdata 1148K, **结构化指针表, 未加密**) | getRomObject |
| torrent.dll | 893K | 韵律/组件 (.text 260K + .data 564K) | getObject + Cmm 信号量 |
| chs.uil | 299K | 词典 (.rsrc 272K, 低熵明文) | — |
| ibmcmm.dll | 53K | IPC/内存工具 (带 C++ 类符号) | 67 个 Cmm* |

## 三、容器格式 (unpack_vvtts.py 内有完整注释)

- 文件尾 8B = u32 目录大小 + u32 魔数 `0xC26B3DA7`
- 目录区逐字节混淆: `plain[i] = enc[i] ^ (((i>>8) ^ (i*31)) & 0xFF) ^ 0x5B`
- 目录 = 9B 头 (`9E 3B C7 51 01` + 条目数) + 条目 `[type][nlen][name][size:4][crc32:4]`
- 条目表后为**单条 raw deflate 流**, 解压结果 = 全部文件按序无缝拼接
- libq4 运行时把引擎解到 `files/d1`, 写 `.m` 标记 (type=0 的 ibmeci 为 main dll)

## 四、可行性判定

**直接调用路径全死**: Box64 是 Linux 用户态模拟器 (ELF linker/信号/proc 语义,
appex 沙盒不容); i386 Windows DLL 在 Apple Silicon 无 32 位执行路径
(Rosetta 仅 x86_64; Wine wow64-on-arm64 实验品)。

**逆向重写 (yongde 路径) 的资产与债务**:
- ✅ ECI API 完整公开文档 (边界清晰, 适配器形态现成 — 同 ms/yongde 适配器)
- ✅ 语音数据未加密 (chsrom = 升序指针表结构; chs.uil = 明文词典)
- ✅ 体量小 (引擎+数据共 4.9M, 拼接合成时代产物)
- ❌ ~2MB i386 代码跨 4 个模块 (ibmeci 编排 + chs50.syn 前端/合成 + chsrom
  数据 + torrent 韵律), 模块间 getObject/getRomObject 接口也要逆
- ❌ ibmeci 内部符号实际为空 (COFF ptr=0xFFFF 哨兵, 仅导出名)
- ❌ ViaVoice 内部格式无公开文档

**结论**: 可做但属于数周级专职逆向项目 (≥ 永德逆向量级)。若仅要中文音色,
扩 MS 包成本近零; 若要 ViaVoice 经典音色再立项。逆向建议次序:
① ibmeci 的 ECI 入口管道 → ② chsrom ROM 表结构 (与 .vl 相似性最高)
→ ③ chs.uil 词典导出 → ④ chs50.syn 前端 (可能可借鉴永德 pinyin/phrases 经验)。

## 五、viaemu 实现进度 (2026-10-17 晚, 进行中)

**架构已定**: `Bridge/viaemu/` 四文件 — ve_memory(guest 区+堆), ve_pe(PE32 加载),
ve_cpu(i386+x87 全量解释器), ve_win(266 导入垫片+线程/事件/文件句柄), ve_eci(ECI 宿主)。
macOS arm64 无低 4GB 固定映射 → 恒等映射不可行, 改**加法偏移翻译** (g_ve_off)。
冒烟 CLI: `clang -IBridge/include -IBridge/viaemu -o /tmp/vvtts_cli
scripts/test_vvtts_cli.c Bridge/viaemu/*.c -lpthread -framework CoreFoundation`。

**ECI 规范 (来源: davidacm/NVDA-IBMTTS-Driver `_ibmeci.py`)**:
- eciNewEx(0x00060000)=MandarinChinese(GB); eciSetOutputBuffer **3 参带 handle**;
- 语速/音高/音量走 eciSetVoiceParam(h,0,param,v): pitchBaseline=2,speed=6,volume=7;
- eciSampleRate=5 (SetParam), 值 0=8k/1=11025/2=22050; 回调 eciWaveformBuffer msg=**0**,
  lp=样本数; 32 位 eciVersion(ptr)。

**已修 bug 清单 (每条都有惨痛现场)**:
1. PE 节表偏移: VirtualSize@8/VirtualAddress@12/RawSize@16/RawPtr@20 (曾全错→映像全零)
2. ve_call_guest 压栈序: 参数先压、STOP 哨兵最后 (曾哨兵最深→ret 弹到 arg1=模块基址)
3. 垫片参数宏 A(n)=[esp+4n] (FF/2 已弹返回地址; 曾 +4 错位→一切参数读错一位)
4. FF/4 尾调用导入: 弹自身返回地址→跑垫片→回到调用者 (曾按 noreturn 弃赛)
5. FS 段前缀: modrm 加 seg_add + **A0-A3 moffs 寻址同样生效** (fs:[0] 即 A1 形态)
6. stdcall 垫片弹栈审计 (GetModuleFileNameA/LoadLibrary/GetProcAddress/SearchPath/
   TlsGetValue/Interlocked×2/RegOpenKeyExA 出参 A(4)…)
7. MSVCRT 全 cdecl 不弹栈; fopen 族返回 guest 伪 FILE (宿主 FILE* 48位地址进 guest 即炸弹)
8. Cmm 层 (ibmcmm/torrent) DllMain 跳过 (等跨进程服务, 同进程模式必死锁)
9. WaitForSingleObject INFINITE 封顶 5s→WAIT_TIMEOUT; 解释器 3e9 指令预算防死循环
10. 崩溃自诊断: SIGBUS/SIGSEGV handler 打印 eip/esp/seg_fs/栈 (lldb 需管理员授权不可用)

**当前状态**: chs.uil/chsrom DllMain ✅ 通过; chs50.syn rc=5 (跳模块基址=仍有栈错位);
ibmeci DllMain 深入 CRT/注册表初始化后, 尾跳 memcpy 收到 n=垃圾 → 拷越界 @0x3F800000
(上游栈被 0x80808080 污染, 污染点待查 — 疑某个垫片写穿或 __chkstk/alloca 类)。

**下轮继续的抓手**:
- VE_TRACE=1 跑, 找 0x80808080 首次出现的 eip (污染源); 对照 objdump 该函数
- 剩余嫌疑: 0xC8 enter 实现 (clobbers EBP)/0x62 bound/回调重入 esp/某个 MSVCIRT this 透传
- 模块基址跳转捕获器已内置 (rc=5, 打印 [esp]) — 每次栈错位直接定位
- DllMain 全绿后: eciNewEx→AddText→SetOutputBuffer→Synthesize→回调 PCM→WAV 验证
- 之后接 Bridge 适配器 (engine table "viavoice", 同 yongde 形态) + Engines/viavoice/ 安装

## 六、第二轮进展 (同日晚, 续)

- **memset 垫片参数序 bug** (val/n 串位 → 604MB 屠场, 0x80808080 污染源) — 已修
- **ALU 立即数操作码组补齐** (04/05...3C/3D: cmp eax,imm 等 16 个)
- **operator new (??2@YAPAXI@Z) 垫片**
- **GetMessageA 真语义**: 无消息阻塞而非返回 WM_QUIT (曾杀死引擎窗口线程→事件永不触发);
  PostMessage/GetMessage 跨线程队列打通
- **ECIINI 伪注册表**: 数据表直接从 libq4 的 eciini_vals 符号 dump 而来 (子键 "6.0":
  Path/Path_Rom/Path_Cmm/Path_Cat + Version/Concatenative/CallbackFlag + Voice1..8;
  "LanguageIndependent": Path_Cmm/Path_Cat); 路径 {D} 展开为引擎目录
- **fopen/CreateFileA 虚拟路径**: 剥 "C:\...\" 前缀取文件名映射引擎目录
- eciNewEx 已能跑进引擎初始化 (堆分配/线程/注册表读取全部活跃)
- **当前卡点**: 引擎内部经 wrapper 调 RegOpenKeyExA 时 phkResult 已是垃圾 (0x56) —
  上游某处仍有一处栈漂移; 定位法: VE_TRACE 该 wrapper (0x10020ae0) 的调用者,
  检查其入参来源 (其 phkResult 应为栈上 leal 地址, 却是 0x56)

## 七、第三轮进展 (续)

- 注册表键一律打开成功 + 子键路径含 "6.0"/"LanguageIndependent" 直接映射 +
  根句柄全表回落搜索 (曾因打开失败改变 DllMain 走向造成回归, 已修)
- 崩溃处理器升级: 沿 guest 栈扫描模块代码区返回地址链 (直接给出调用链)
- 分配垫片大额日志: new/calloc/HeapAlloc/VirtualAlloc > 0x10000000 打印现场
- **当前精确卡点**: `operator new(0x3EFEFF9F)` — 大小参数为栈地址值, 且
  A(0) 与返回地址槽同值 → 调用形态为 "push size; jmp new" (无 call 返回地址)
  或该处栈已漂移。两次 1GB 分配即源于此 (new 返回 0 → 上游对象 NULL →
  ctor this=NULL 崩于 +0x4c, 调用链 0x1000d14e→0x100134f0 已定位)。
- **下轮第一步**: 在 ??2 垫片打印 [esp-8..esp+12] 6 槽 + 用 objdump 查
  0x1000b416 附近 (上次 memset ret=0x2400b41b) 该 new 调用点的真实指令
  (确认 push+jmp 还是 call), 若为栈漂移则回溯该函数入口的固定调整差

## 八、第四轮进展 (标志位大修 + 编译器混用发现)

**本轮三个永久性修复**:
1. **CPU 标志位三连 bug** (scripts/viaemu_cputest.c 自测 11/11 全过):
   - SF 提取错误: `r & SF` 取了位7, 应为 `(r>>24) & SF` (位31→位7) — 影响
     一切符号分支!  ② PF 奇偶表反了 → __builtin_parity  ③ inc/dec 的 AF
     公式 (低半字节 0xF/0x0 判定)。此前所有"玄学走错分支"多源于此。
2. **Borland/MSVC 混用约定**: 同一批 DLL 里 ibmeci/torrent 的 operator new/
   delete 按被调方弹 4 (Borland), chs.uil 等按 cdecl 调用方清栈 (实测
   0x1000d11d push size; call new; 无清理)。thunk 表改造为带导入方模块名
   (ve_shim_slot_mod), borp 标记仅 ibmeci/torrent 弹参。
3. RegQueryInfoKeyA 出参填写 (调用方据 lpcMax*Len 分配缓冲, 之前读栈残渣
   → 1GB 分配元凶); operator new(0x143) 链路打通。

**当前最新前沿** (下轮从这里继续):
- 引擎线程生态已活跃: _beginthreadex + CreateWindowExA + GetMessage 循环 +
  主线程并行动作 ✓; eciNew 深入到 "加载模块对象→虚调用" 阶段
- 新卡点 A: "eip 落入 thunk 区" (eax=0xF003F) — 引擎把导入地址当函数指针
  存进数据后 call — 查 0x24001c60/0x24001c93 的调用形态 (call *eax 应已被
  FF/2 覆盖, 疑 push+ret 或 lea 直跳形态)
- 新卡点 B (A 之后): vtable NULL+0x40 虚调用 (对象加载返回 NULL, 链
  0x1001fdd6→0x1000be05)
- REGQ 追踪已确认 eciNew 阶段只查 TraceLevel/Path_Trace (LanguageIndependent)

## 九、第五轮进展 (里程碑: 全流水线无崩溃贯通)

**卡点 A 已解**: thunk 表容量 400→1200 (6 模块按模块去重后 ~450+ 导入溢出,
静默喂哨兵 → RegOpenKeyExA 槽变 0x0EF00000 哨兵 → 被直跳)。IAT 登记表同步
扩容; 体检阈值同步。溢出现在有告警。

**当前状态**: eciNewEx → SetOutputBuffer → RegisterCallback → AddText →
Synthesize → Synchronize → eciDelete **全链路无崩溃跑完** (历史首次),
唯返回 0 样本 — 因引擎初始化中一处中断:
- 现场: CreateWindowExA → new×2 之后, 新对象 vtable 指针 = **0x15A6**
  (未重定位的旧值形态), call [vtable+2] → eip≈0x15a8 → 在零页漫步至
  0x0D001000 (TEB2, bytes=ff ff) 报 FF/7
- 下轮抓手: VE_TRACE 定位存 0x15A6 的构造点 (疑点: ① 某构造函数从
  GetProcAddress/注册表数据抄 vtable ② chs50 运行期拷贝带旧基址指针且
  未经 reloc 覆盖的通道 ③ 尾调用 new 返回后 ebp 未恢复的旧问题的另一
  变体 — 看 new×2 返回链是否又跳层)
- 剩余已知噪音: chs50.syn DllMain 仍 rc=5 (模块基址跳) — 不阻塞主线

## 十、第六轮进展 (调用形态全谱收网)

- 新增四类转移目的地校验: FF/2 间接 call 低地址/TEB 区拦截、C2/C3 ret 落点
  非代码区拦截、E8/E9/EB 直跳落点校验、ve_call_guest 入口校验 — 现在任何
  控制流异常**当场捕获带现场**, 不再零页漫步污染诊断
- **??2/??3 撤销 Borland 弹参恢复 cdecl** — 铁证 0x1001f7c0:
  `call 转发桩; add esp,4`(cdecl) 且 0x1000d127 = `59 pop ecx`(也是调用方清);
  上轮"borp"为误判 (真因 RegQueryInfoKeyA 出参, 已修)。结论: 全部 MSVC cdecl。
- CALL直跳日志现显示引擎线程通过函数指针调 0x2400f8f3/0x2400f742 (合法
  内部调用, 噪音) — 引擎线程虚调用生态活跃
- **当前回到原始 ctor this=NULL (0x24013510)**, 下轮抓手 (换新眼光):
  ① 重新审计 0x1000d100-0x1000d150: 0x1000d122 call 0x10024976 转发桩实际
  目标是 **IAT 0x1002613c = strcpy** (非 new! 之前误认) — 该段是注册表子键
  字符串构造; ctor 的 this 真正来源要沿 0x1000d0e7→0x1000d114 (call 0x1001a996)
  重查 — 0x1001a996 才可能是分配/工厂函数
  ② 引擎线程 (0x39e 栈) 与主线程并发竞争同一 guest 堆 — ve_heap 无锁竞态?
  (heap lock 有; 但 VeCPU 各自跑解释器, guest 数据竞争由引擎自己的锁保护,
  而引擎的锁=我的 pthread 事件 — 若事件语义不严可能竞态) — ctor this=NULL
  或为引擎线程写入与主线程读取竞争 → 检查该对象是否被引擎线程构造

## 十一、第七轮进展 (排除法收网到内层)

- 重定位抽查机制上线: ibmeci+0x12913 内联 vtable 立即数 = 0x24026528 ✓
  (代码内立即数重定位**正确**, 旧基址嫌疑排除)
- 0x100128bc 中段反汇编: 直线流到正常 epilogue (mov fs:[0],[ebp-0xc];
  leave; ret), **无尾跳** — 但 ret 出口探针证明从不经此返回 → 必是其内层
  call (0x1001fa60 ×2 构造 this+0x40/0x44 成员, 或 0x1001fc40) 提前退出/
  尾跳出整帧
- 值加载函数 0x1001f7a0 确认: 无 SEH helper、不动 ebp、内联 vtable
  (mov [esi], 0x100268a0)、`push size; call new桩; add esp,4` cdecl ✓
- IAT 槽位标注勘误: 0x2613c=??2(new), 0x260e0=??3(delete) (此前解析错位
  一位, 不影响行为, 垫片按名分发)
- **下轮抓手**: 0x1001fa60 / 0x1001fc40 出入探针 (哪个提前退出); 若它们
  内部判错 → 沿判错条件找数据源 (注册表值/字符串/分配)

## 十二、第八轮：十连修 — 引擎从"ctor 崩溃"推进到"最后一哩" (2026-10-18)

### 修复清单 (全部实锤, 按 discovery 顺序)
1. **_beginthreadex 误 STD(24)**: MSVCRT 全 cdecl, 调用方 0x1001fc60 `add esp,0x18`
   自清 — 双重清栈 24 字节 → ret 读错槽 → 基类 ctor 0x100128bc 跳帧返回
   (esp 对、ebp 不恢复 → 外层 ctor 用错帧读 this=NULL → 0x10013510 崩).
   "ebp 漂移-0x8C"实为子帧正常遮蔽, 非 bug.
2. **RegQueryValueExA 尺寸取自 lpReserved(A(2))**: 应读 *lpcbData(A(5)指针)
   → memcpy 永不执行 → 引擎永远拿空串 → LoadLibraryA("") 失败 (chs.uil/
   chsrom/chs50.syn DllMain rc=5 的真因).
3. **x87 m64/m32 访存漏 GADDR**: fld_m64/WM64(FST m64/FILD m64)/DB-FILD m32/
   DA-FIST m16/FRSTOR — chs50.syn DSP 代码一进 double 就炸.
4. **sprintf 格式器丢前导 %**: piece 拼成 "u"/"d" 无 % → "Phoneme%u" 出
   "Phonemeu"、"%u.u" 出 "u.u" (诡异子键名的真因).
5. **sprintf/fprintf 变参起点 esp+12**: 应 esp+8 (FF/2 已弹返回地址) → 指针
   当整数打印 ("Phoneme371202264").
6. **Win32 long=4 字节**: sscanf %ld 写 8 字节重叠邻位字段 (音素表 0x30/项
   全毁); l 计数改 lng≥2 才 8 字节.
7. **jle(0x7E) 条件错**: cond case7 实现成 SF==OF (那是 NL) — 正确为
   ZF||SF≠OF. **全部 guest jle/jg 判反** (chs50 0x10176cdd 直接因此走错支).
8. **qsort 垫片把未初始化 malloc 垫进表里** (先写后读); 重写为插入排序,
   暂存元素放 guest 堆临时块 (比较器只认 guest 地址).
9. **FF/2 thunk 后不修 eip**: ve_call_guest(比较器/回调)把 eip 打成 STOP
   哨兵 — FF/4 尾跳路径有 c->eip=tail_ret 自愈, FF/2 call 路径没有 → qsort
   一返回主循环就在 0x0EFFFFFF 零页漫步. 补 c->eip=retpc.
10. **strtok/pow/exp/log 把 guest NULL(0)/double 位模式当指针**: strtok(NULL)
    续读状态必须映射宿主 NULL; math 族参数按值读 {A(2n),A(2n+1)} 两槽.
    另补 _ftol(_ftol2): st0→截断→edx:eax.
- 附: 66 前缀 ALU imm (04/05/0C/0D…/81/83/B8) 按 osize 取 imm16 — `add ax,1`
  曾滑 2 字节跳过 idx 写回 → 音素循环死循环 260 万次 (探针地址比对抓到).
- sscanf 全量重写: %[*][w][l/h]diuxXofegscn, 空白/字面/宽度/抑制齐全.

### 里程碑
- ECIINI 伪注册表升级为 **libq4 内嵌表全量** (i386 指针基址 0x08048000 +
  delta=文件偏移): "6.0" 下 Path/Path_Rom/Path_Cmm/Path_Cat/Version/
  Concatenative/CallbackFlag("0x33f")/Voice1..8/**Phoneme0..84** + 
  LanguageIndependent 两条. 引擎全链装载: 枚举子键→6.0→85 条音素→qsort→
  8 条 Voice strtok 分词→CallbackFlag 解析 ("0x33f"%10x ✓).
- 指令预算改环境变量 VE_BUDGET (单位 G, 默认 20G); 哨兵区执行拦截
  (0x0EE00000..0x0F000000 报警而非漫步).

### 当前唯一卡点: eciNew 返回 NULL = 语言工厂 -14 错误码
精确调用链 (全部探针证实):
```
eciNewEx → 0x1000bdc4 (new(0xBA) obj)
  → init 0x1000d0dd (ctor 0x100134eb ✓ 全通过)
  → 0x1000d8b7(this+0x52, handle, param)   "启动"
      枚举 ECIINI 子键→取最小名 "[6.0]"→剥括号 "6.0" ✓
      new(0x11) 工厂 → 0x10013e36(this, factory)
          → 0x1000e75d(this+0x50, factory)   [返回 NULL → -14]
              → 0x1000e681(this, "6.0") 语言查找/装载
                  ① 0x1000ea11 缓存查 → miss
                  ② 0x1000e7fa(this, name) 装载器 ← ★失败点 (下一步)
                  ③ new(0x18) 包装 → 0x1000ea64 入缓存
  → [obj+0xB6] = -14 → delete obj → eciNew NULL
```
**下轮从 0x1000e7fa 开始** (疑似: 按 Path 注册表值 LoadLibrary/读文件 →
chs50.syn 语言注册). 已知未实现导入: MSVCIRT iostream 家族 (ifstream/open/
get/close ~20 个, chs50.syn 数据文件读取疑似走它) + torrent→ibmcmm Cmm API
17 个 (共享内存层, 或不需要). 静态导入全清单: /tmp/imports.txt (266 项).
- 构建命令 (必须 cd 前缀!): cd MagicTTS && clang -std=c11 -O2 -g -IBridge/include
  -IBridge/viaemu -o /tmp/vvtts_cli scripts/test_vvtts_cli.c Bridge/viaemu/*.c
  -lpthread -framework CoreFoundation
- 关键探针 (ve_cpu.c probe 块): D8B7-A/B(枚举) D8B7-CHK/FAIL-RET ENGOBJ-ERR
  ERRSRC-A/B VS-ENTRY/VS-JLE PHN-LOAD/PHN-FLOW SSCANF/SPRINTF/STRTOK 定点日志.

## 十三、第九轮后的方向转折：macOS 内置 Eloquence ("Kona") 调研 (2026-10-18)

**结论先行**: macOS 自带的 Eloquence 发音人 (Eddy/Flo/Grandma/Grandpa/Reed/Rocko/Sandy/Shelley)
不是"另一个相似引擎"，而是 **IBM ViaVoice/Eloquence 血统引擎的原生 arm64 移植**，
每个发声人带 14 个语言变体 —— 含 zh_CN/zh_TW。`say -v "Eddy (中文（中国大陆）)"` 直接出声
(22050Hz, 实测 RMS 2310, ~100x 实时)。

### 实现方式全图 (本机 macOS 26 实测)
- 管道: say/AVSpeech → TextToSpeech.framework (Swift, TTSExecutor 队列) → XPC/AU
- 引擎: `/System/Library/PrivateFrameworks/TextToSpeechKonaSupport.framework/Frameworks/`
  14 个语言 dylib: enu eng deu esp esm fra frc ita fin jpn kor ptb **chs cht**
  - 磁盘上该目录为空壳，实体在 dyld 共享缓存 (dlopen 按安装名即可加载)
  - **符号未剥离**: chs.dylib 7056 symtab / 2136+ external
- 血统铁证 (chs.dylib 导出):
  - `getObject` 工厂 —— 与 APK chs50.syn 同名同 ABI (viaemu 里我们调的就是它)
  - `eloqc_new/eloqc_delete` (Eloquence C 运行时)
  - `EngineWrapper` C++ 类 (COM 风格: vt[0]=queryInterface vt[3]=start vt[5]=processSentences
    vt[11]=flush vt[17]=setSynthToCallback vt[19]=setKlattConstHook vt[28]=close …全解码)
  - `DeltaProc_*` 文本前端 (registerLanguages/main/process_sentences…) + `Klatt*` 合成核
    (KlattOpen/KlattSynth/KlattSetConstParms) + `eciLink*`
  - Resources: `KonaVoicePresets.plist` —— eciVoiceNumber/pitchBase/headSize/breathiness/
    roughness/vocalTract = ECI 参数体系; `chtvoice.sapi.txt` = GBK 编码 \Vce=/\Spd= 演示稿
- 版本: version.plist ProjectName=AccessibilitySpeechEngines BuildVersion=129

### "换我们的数据" 判定: 不可行
- 语言数据与代码一起编译进 code-signed SIP 共享缓存 dylib，无任何外置数据文件
  (对照: Vocalizer ('purr', Li-Mu 等) 有 SpeechBase/Voices/*.SpeechVoice 数据包,
  Eloquence/Kona 没有)
- 不动 SIP/不重签名就无替换点; APK 的 chs50.syn 数据 (i386 PE) 也无对应加载通道

### 直驱实验进展 (/tmp/kona_drv*.c, 已验证到最后一哩)
- dlopen chs.dylib ✓ → getObject(2,&obj) ✓ (rc=1) → EngineWrapper vtable 全解析 ✓
- setSynthToCallback/start/processSentences/flush/end 无崩溃跑通, 但回调零次 (无音频)
- enu 的 processSentences 在 pc=0 崩 (内部函数表未接线) → 缺宿主侧初始化
- ibmeci.dll 0x1000e45e 反汇编: getObject 后须连调 表+0xc (须返回非零) / 表+0x68 / 表+0x8;
  我们的调用序缺这些 (苹果版 vtable 布局又与 i386 版不同)
- `DeltaProc_registerLanguages()` 裸调即崩 (需参数/上下文)
- 突破口: 反汇编 TextToSpeech Swift 胶水 (say 采样定位到 load+0x626a5 等) 或试
  DeltaProc ABI —— 有界但需半天到一天

### 对项目路线的影响
- VVTTS 中文音色若与 Apple chs 同源 (极可能), "还原 VVTTS 听感" ≈ 调参数而非搬数据:
  试听样本 VVTTS/sample_eddy_zh.aiff; KonaVoicePresets 的 pitchBase/speed 等可调
- MagicTTS 集成最优解排序:
  1) 系统已自带该声音 → MagicTTS 可只做路由/包装 (甚至无需自研引擎)
  2) 直驱 chs.dylib (私有 API, 需攻克初始化序列) → 参数级控制
  3) viaemu (备胎, 全控制但 5-15 MIPS 性能风险)

## 十四、第十轮：直驱 AXKonaSpeechEngine — MagicTTS 集成完成 (2026-10-18)

**结果**: `Bridge/kona_adapter.m` 进程内直驱系统 Kona 引擎, 双语方案 (双语方案·一 =
Eddy 中文+英文) 在 appex 内经 `say` 验证出声 (8.09s, peak 19204, 三段 中→英→中)。

### 路径排除记录 (全部实测)
1. **AVSpeechSynthesizer.write 在语音提供者 appex 内被系统拒绝**: 沙盒内 197 个发音人
   可枚举、voiceWithIdentifier OK, 但 writeUtterance 回调永不到达 (main/自泵线程双路
   6s 超时)。CLI 同代码正常 → 是对 speech-provider 扩展的合成服务限制, 无解。
2. **macOS 26 所有常规合成路径都走 XPC**: say/AVS/NSSpeechSynthesizer/Carbon 的合成
   实际发生在 `KonaSynthesizer.appex` (ExtensionKit, /System/iOSSupport/...) 进程内
   (96% CPU 实测); say 进程内映射的 Kona dylib 是 `unused` (仅枚举预载)。
   SpeechSynthesisServerXPC 只是代理。
3. **DYLD_INSERT 注入追踪不可行**: dyld4 对插入库自身镜像也 interpose (r_dlopen 解析
   成自己 → 无限递归栈溢出, +load 也救不了); 系统进程 SIP 剥离 DYLD_*。

### 直驱配方 (最终, /tmp/ax_test.m 可复现)
```
dlopen(".../TextToSpeechKonaSupport.framework/TextToSpeechKonaSupport")  # 缓存镜像
engine = [[AXKonaSpeechEngine alloc] initWithVoice:voice]   # voice 来自 +allVoices
[engine setParameters:p]        # AXKonaParameters, 8 属性全 NSInteger — 必须整数直传!
[engine synthesizeText:文本]     # 同步合成, 毫秒级返回
while (kb = [engine nextBuffer]) { pcm += kb.buffer; }       # AXKonaBuffer→AVAudioPCMBuffer
```
三个关键坑:
- **不要手动 setOutputFormat** (自建 AVAudioConverter 会静默产出 0 帧 buffer);
  用引擎默认 engineFormat (16k Int16)。
- **AXKonaParameters 属性是 NSInteger**: performSelector withObject:@(50) 传的是
  NSNumber 指针值 → 巨大垃圾值 → 合成截断 0.2s。用 objc_msgSend 整数直传。
- **不注入 volume 参数则输出 ~1/40 幅度** (peak 823 vs 19841) — KonaVoicePresits
  的 volume (90-95) 必须带上。预设全表已录入适配器 (Eddy eci=9 pitch=69 …)。

### 对象模型 (KonaSupport 缓存二进制内, ObjC 运行时可枚举)
AXKonaSpeechEngine (synthesizeText/nextBuffer/cancelSynthesis/klatt hooks/
eciCallback:iParam:instanceData:) + AXKonaVoice (allVoices 112=8人×14语言) +
AXKonaParameters (speed/volume/pitchBase/fluct/breath/rough/headSize/tract) +
AXKonaConfiguration (dylib=chs dialectCode=6.0 encoding=GB18030 iniString=[6.0]) +
AXKonaBuffer (buffer=AVAudioPCMBuffer, markers)。

### 调用链实锚 (KonaSynthesizer.appex 96%CPU 采样)
ETIThread::execute (eci.dylib) → ETImessage::process → SynthThread::addTextToEngine →
EngineWrapper::processSentences → engsynProcessSentences → DeltaProc_process_sentences →
process_input → apply_rules → run_speech_module → generate_acoustic_vals →
synthesizeRange → synthesize → sendArrayParameters → -[AXKonaSpeechEngine
klattDynamicHook:] — 即引擎常驻消息线程 + Klatt 钩子回调, 宿主类就是 AXKonaSpeechEngine。

### 引擎层逆向沉淀 (chs.dylib, 备将来绕过 ObjC 层用)
getObject(1/2)=新建 EngineWrapper(license 校验) / getObject(3)=RequestLicense 单例;
wrapper+0x10=delta 会话 (delta_new 于构造); delta+0xe0=256B 链路缓冲 (ecilink_new
填函数表, 空 3 槽=事件回调) / +0xe8=eciLinkNew() 数据通道 (文本经 eciLinkDataFromECI
入引擎); engsynStart→etiwinMainDLL→vcmdinit→initializeIO→DeltaProc_start; 返回值=
checkEngsynError (link+0xd8 错误码, **0=成功**); engsynFlush(0)→engsynRestart 触发;
无工作线程 — 全靠宿主泵 (eci.dylib 的 ETI 消息线程即官方泵)。
