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
