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
