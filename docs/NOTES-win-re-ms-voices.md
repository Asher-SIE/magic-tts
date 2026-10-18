# Windows grtts (NVDA) 逆向报告 — MS 嵌入式语音的 key/license 机制与 HD 延迟线索

2026-10-19，在 Windows 侧对 NVDA 插件 `grtts`（`%APPDATA%\nvda\addons\grtts\synthDrivers\`）
做静态逆向（pefile 解 PE + capstone 反汇编交叉引用），回答两个问题：

1. 为什么 Windows 能调用全部 14 个 NeuralVoices 发音人，而 macOS 只有 7 个 license 包可用；
2. macOS 上 HD 语音每句 ~120ms 首块台阶延迟的来源线索。

工具注：本机 REA 深度分析提供器（Ghidra/IDA）未配置，改用 pefile+capstone 手工反汇编，
所有结论均落到具体函数地址，可复核。

## 0. 语音包物料盘点（两平台同源）

`NeuralVoices/NeuralVoices-voice-*/` 共 14 包，分两类：

| 类别 | 包 | 凭证文件 | Tokens.xml 特征 | 数据特征 |
|---|---|---|---|---|
| OneCore key 包 (7) | Aria / Guy / Jenny / Ryan / Sonia / Xiaoxiao-Natural / Yunxi | `SynthModel.key`（90 字节） | 无 LicenseVersion | AppxManifest.xml + AppxSignature.p7x（Windows 语言包原样导出） |
| license 包 (7) | Andrew-HD / Ava-HD / Xiaoxiao-HD / Eric / HsiaoChen / Michelle / YunJhe | `SynthModel.license`（721 字节纯文本） | `LicenseVersion="1"` | HD 包为双模型（标准 am+vocoder 再加 hd_ 前缀两件套） |

关键事实（md5 实测）：

- **7 个 `.key` 文件内容完全相同** —— 一把全局 OneCore 主密钥，不是每包一把；
- license 只有两种文本：HD 组共用一种，新 Natural 组（Eric/HsiaoChen/Michelle/YunJhe）共用另一种；
- license 全文就是纯文本许可声明（含 "reference number 2774316"），无签名尾巴。

### 主密钥（grtts_x64.dll 内硬编码兜底值，0x18009cab0）

```
Key:ZCjZ7nHDSLvf4gpELteM4AnzaWUjTpn7UkV7D@vvksl0w1SNgon6d1905WANbktDc9S39oaA4r29HJNayXvTq8fJsq
```

即 `"Key:" + SynthModel.key 的 90 字符`。文件缺失时 Windows 直接用这把兜底，因此
所有 OneCore key 包（以及理论上任何缺失凭证的包）都能被它解密。

## 1. Windows 侧调用链

```
NVDA Python (_grtts.py, YongDe_* ctypes)
  └─ grtts_x64.dll v1.1.0（"广荣TTS"）
       └─ Azure Speech SDK 1.45.0（NeuralVoices/azure/cognitiveservices/speech/lib/x64）
            ├─ Microsoft.CognitiveServices.Speech.core.dll            1.45.0
            ├─ ...extension.embedded.tts.dll                          1.45.0
            ├─ ...extension.onnxruntime.dll                           "Internal Build" ← 内部构建
            └─ SpeechSDKShim.dll 0.2.8（自制 "提供给 Azure Speech SDK 的兼容层", API 转发垫片）
```

初始化（grtts_x64.dll `0x1800458d0`）：

1. 扫描 `NeuralVoices-voice-*`，要求 `data/*/Tokens.xml`（枚举函数 `0x180041510`）；
2. 读包根的 `SynthModel.key` / `SynthModel.license`（`0x1800419e2` / `0x180041a39`）；
3. `embedded_speech_config_create`；
4. **每个发音人** `embedded_speech_config_add_path("<包>/data/<VoiceId>")` —— 传的是
   模型叶子目录（Tokens.xml / `<LCID>.INI` / 模型 bin 所在处）；
5. 属性：输出格式 3102 = raw-24khz-16bit-mono-pcm（枚举 ID 16）；
   `RequestSentenceBoundary=true`（0x106a）、`RequestPunctuationBoundary=false`（0x1069）；
6. `synthesizer_create_speech_synthesizer_from_config`，**常驻复用**，
   PushAudioOutputStream + synthesizing 回调流式取音频；
7. 选发音人：`embedded_speech_config_set_speech_synthesis_voice(cfg, 完整名, 凭证)`
   （帮助函数 `0x180046950`）。

## 2. 凭证机制（问题 1 的答案）

`embedded_speech_config_set_speech_synthesis_voice` 在这套 SDK 里是**三参数** C API。
反汇编 core.dll `0x180023e20`：

```
第 3 参数 (r8) → 属性 SPEECH-SynthesisModelKey (PropertyId 3106)
发音人名 (rdx) → 属性 SPEECH-SynthesisOfflineVoice (3105)
二者经 embedded_speech_config_set_model 下发
```

凭证字符串的构造规则（grtts_x64.dll `0x180041b02`–`0x180041d25`）：

1. `SynthModel.key` 读到 → 凭证 = `"Key:" + 文件内容`（先查重前缀再拼接；
   文件缺失时用硬编码主密钥兜底）；
2. 否则 `SynthModel.license` 读到 → 凭证 = license 文件全文；
3. 都没有 → 空 → 兜底主密钥。

引擎侧（extension.embedded.tts.dll）按**前缀分流**：

- `Key:` 前缀 → 解密密钥路径：`AESGCMEncryptionEngine` AES-GCM 解密模型 bin
  （组件名 "Key:"、"##Encrypted##" 在引擎静态初始化里注册，`0x18000123a` 一带）；
  失败 → `EMBEDDED_TTS_ERROR_WRONG_DECRYPTION_KEY`；
- 无前缀 → license 文本校验路径（内置 Microsoft 根证书链、LicenseVersion=1）；
  失败 → `EMBEDDED_TTS_ERROR_INVALID_LICENSE`。

**对 macOS 的推论**：此前 2026-10-17 把 key 文件内容裸传给
`set_speech_synthesis_voice` 得到 reason=1，就是因为没有 `Key:` 前缀 —— 引擎把
90 字符当作 license 文本校验，必然失败。修复 = `"Key:" + 内容`（适配器已改，
`ms_get_session`）。license 包传法不变（全文）。

与官方文档互相印证：Embedded Speech 官方样例
`SetSpeechSynthesisVoice(voiceName, EMBEDDED_SPEECH_MODEL_LICENSE)` 第二参数即凭证；
"Key:xxx" 形态是 Windows OneCore 语音包的私有约定。

## 3. HD 每句 ~120ms 台阶延迟的线索（问题 2）

HD 包的 `data/<VoiceId>/<LCID>.INI`（如 2052.INI / 1033.INI）与普通包的关键差异：

```ini
[Output]
EnableBuffer=false        ; 不做输出缓冲
BatchSize=16000
EnableThrottling=false    ; 不做节流
[Pipeline]
EnableHDVoice=yes
HDAMModelPath=hd_am_v5    ; HD = 双模型: 标准 am+vocoder 外再加 hd_am+hd_vocoder
HDVoiceSeqLen=5           ; HD 声学模型按 5 帧序列生成
HDVoiceThreshold=10
```

引擎侧（embedded.tts.dll）已证实的两点：

- `Pipeline.HDVoiceSeqLen` 读取处 `0x1800d109e`：**INI 没读到时默认 24**（0x18），
  读到才用包内配置的 5；
- `Output.EnableBuffer / BatchSize / FirstBatchSize / EnableThrottling`（读取处
  `0x1800a3190` 一带）同样来自该 INI。

**两平台调用方式的对应差异**：

| | Windows grtts | macOS 适配器（改前） |
|---|---|---|
| add_path 路径 | `model/<包>/data/<VoiceId>` 叶子目录 | `Engines/<key>/model` 父目录 |

macOS 传父目录时 SDK 仍能递归发现 Tokens.xml（语音能列出、能合成），但 INI 的
加载路径可能随之失效 → HDVoiceSeqLen 退化为默认 24 → HD 模型按 24 帧一批生成，
首音频前的固定前端批处理成本放大 ~5 倍 —— 与实测"HD 多字词句 108-190ms 台阶式、
不随句长增长，非 HD 12-23ms 零惩罚"的形态吻合（见 NOTES-ms-streaming.md）。

适配器已改为传叶子目录（`ms_get_session` 组 `model/<pkg>/data/<vdir>`）。
**验证方法**：复测 HD 首块延迟矩阵（晓晓 HD / Andrew HD / Ava HD 多字词句），
若仍在 ~120ms，则结论回退为"该 SDK 构建的 HD 前端整句处理成本"，保留
NOTES-ms-streaming.md 原判（流水线掩盖 / 配非 HD）。

其他 Windows 做法对照（macOS 已等价或不需要）：

- 输出 raw-24khz-16bit-mono-pcm：macOS 已同（3102 属性）；
- 常驻 synthesizer + 回调流：macOS 等价（会话懒建 + wav 文件跟随）；
- 句边界 true / 标点 false：NVDA 索引用途，macOS 不需要；
- Windows 冷启也在首次选音人时付（NVDA 启动即建），macOS 已有 .lastvoice 预热对齐。

## 4. 版本差异备忘

- Windows 插件 SDK = **1.45.0 内部构建**（onnxruntime 扩展标 "Internal Build"，
  另配自制 SpeechSDKShim 兼容层）；
- macOS 当前用公开 NuGet **1.48.2**（fetch-ms-sdk.sh）；
- `Key:` 前缀约定与 AES-GCM 解密组件在 1.45.0 内部构建确认存在；1.48.2 公开版
  是否同样支持需实测（若 set_voice 直接报错或 reason=1 复现，可降 1.45.x 或
  换 embedded 变体包验证）。

## 5. macOS 实验清单（按优先级）

1. `.key` 包（如 Yunxi/Aria）+ `"Key:"+内容` 传 set_voice → 看 reason 码
   （0 = 成功；若 VOICENAME_NOTEXIST 检查完整名提取，若 reason=1 记录 SDK 版本待查）；
2. 叶子目录 add_path 后复测 HD 首块延迟矩阵（vs 父目录基线 108-190ms）；
3. 交叉验证错误码语义：key 包裸传 license 文本 / license 包传 "Key:" 前缀，
   确认 WRONG_DECRYPTION_KEY 与 INVALID_LICENSE 的分流；
4. 若 1 失败于 1.48.2：用 NuGet 拉 1.45.x dylib 同法再试；
5. 主密钥兜底：删除某 .key 包的 SynthModel.key 后传硬编码主密钥，验证通用性。

## 6. 证据索引（复核用）

| 结论 | 位置 |
|---|---|
| SynthModel.key/license 读取 | grtts_x64.dll `0x1800419e2` / `0x180041a39` |
| "Key:" 前缀拼接 + 硬编码兜底 | grtts_x64.dll `0x180041b52`(std::search "Key:") `0x180041c02`(写入前缀) `0x180041d14`(兜底串, 94 字节) |
| 三参数 set_speech_synthesis_voice → 属性 3105/3106 | core.dll `0x180023e20`→`0x180023c40` |
| 属性 ID↔名称表 (3104 OfflineDataPath / 3105 OfflineVoice / 3106 ModelKey) | core.dll `0x180006e35` 一带 switch |
| add_path 叶子目录 (data/<VoiceId>) | grtts_x64.dll `0x180045bc6`（枚举产物 r13 记录, 步长 0x60） |
| 输出格式 ID 16 = raw-24khz-16bit-mono-pcm | core.dll `0x18003c326` |
| HDVoiceSeqLen 默认 24 | embedded.tts.dll `0x1800d109e` (`mov [r14+0x12c], 0x18`) |
| Output.EnableBuffer/BatchSize/FirstBatchSize/Throttling 读取 | embedded.tts.dll `0x1800a3190`–`0x1800a33c4` |
| "Key:" / "##Encrypted##" / MicrosoftTTS 组件注册 | embedded.tts.dll `0x1800011d0`–`0x18000172a` |
| AESGCMEncryptionEngine / WRONG_DECRYPTION_KEY / INVALID_LICENSE | embedded.tts.dll RTTI 与错误表 `0x180005730` 一带 |

## 7. macOS 实现正确性复查与文档冲突清单（2026-10-19）

对照本报告逐项复查 macOS 侧实现（`Bridge/ms_embedded_adapter.c` 全文 +
`scripts/ms_stream_probe.c` + 各 NOTES/注释）。

### 7.1 与 Windows 证据一致、判为正确的做法

| macOS 做法 | Windows 证据 | 判定 |
|---|---|---|
| 三参数 `set_speech_synthesis_voice(cfg, 名, 凭证)` | core.dll `0x180023e20` 同 ABI | 正确（10-17 实测先于本文档还原出该 ABI） |
| 语音名用 Tokens.xml 完整显示名 | Windows 传入名同样取自 SDK voices 列表（完整名） | 正确 |
| 输出格式属性 3102(name=NULL)="raw-24khz-16bit-mono-pcm" | Windows 枚举 ID 16 = 同一字符串（`0x18003c326`） | 正确；"wav 无 RIFF 头" 与 raw 格式互证 |
| license 包传全文 | grtts `0x180041c76` 分支同款 | 正确 |
| 会话/合成器懒建 + 跨句常驻 | Windows 常驻单 synth | 正确（等价；Windows 是全量单 config，macOS 每发音人一套，叶子目录模式下等价成立） |
| 模型包物料跨平台直接复用 | 同一批包 | 正确 |

### 7.2 Windows 有而 macOS 不同的点（合理适配，无需改）

- 音频取出：Windows = PushAudioOutputStream + synthesizing 回调；macOS = wav
  文件跟随（macOS 实测事件/直取均 0 字节，文件跟随是当地可行解，两者都属
  AudioConfig 目的地）；
- 句边界 true / 标点 false：NVDA 朗读索引用途，macOS 场景不需要；
- SDK 版本：Windows 内部 1.45.0 vs macOS 公开 1.48.2（见 §4，"Key:" 支持
  待实测——这是当前唯一悬而未决的兼容风险）。

### 7.3 与本次调查冲突的文档/注释（已全部就地标记）

| # | 位置 | 冲突内容 | 处理 |
|---|---|---|---|
| 1 | NOTES-ms-streaming.md「发音人物料现状」 | "云希 .key 包被拒 reason=1" — 诊断过期：根因是裸传 key 内容（缺 `Key:` 前缀被引擎当 license 校验），非包不可用 | 就地 ⚠️ 标注 |
| 2 | NOTES-ms-streaming.md「HD vs 非HD」结论 | "~120ms 为 SDK/模型内在成本，适配层无法移除" — 与 §3 线索冲突（Windows 无此惩罚且叶子目录使 INI 生效，SeqLen 5 vs 默认 24） | 就地 ⚠️ 标注，HD 矩阵待复测 |
| 3 | Bridge/include/engine_bridge.h | "含 SynthModel.license 的包一个发音人" — 漏 .key | 注释已直接修正 |
| 4 | ms_embedded_adapter.c ms_create 注释 | 同上（上次改动遗留） | 注释已直接修正 |
| 5 | scripts/ms_stream_probe.c | 只挑 .license 包，无法测 .key（实验清单 #1 需走适配器路径或先扩展 pick_pkg） | 源码注释已标注 |

### 7.4 复查结论

macOS 实现的两处真实偏差（.key 凭证格式、add_path 路径粒度）已在上一提交
修正；其余差异属合理平台适配。剩余不确定性收敛为一项：**公开 1.48.2 对
`Key:` 前缀约定的支持**，实验清单 #1/#4 直接回答。
