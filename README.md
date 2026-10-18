# Magic TTS

macOS 多引擎中文语音合成器 — 通过 `AVSpeechSynthesisProviderAudioUnit` 扩展把三个
TTS 引擎注册为系统语音（VoiceOver / 朗读内容 / `say` 均可用）。

## 语音（按发音人列出）

每个引擎的**全部发音人**都会注册为系统语音：

| 发音人 | 引擎 key | 类型 | 语言 | 采样率 |
|---|---|---|---|---|
| 永德·语音0 ~ 语音4 | `yongde` | 逆向复刻拼接引擎（槽位编码 + 5 个 .vl 音色库） | zh-CN | 16 kHz |
| 自然语音·晓晓 HD / 晓臻 / 云哲 | `naturalvoice` | 微软嵌入式神经网络（流式） | zh-CN / zh-TW | 24 kHz |
| OneCore·晓晓 HD / Andrew HD / Ava HD / Eric / Michelle | `onecore` | 微软嵌入式神经网络（流式） | zh-CN / en-US | 24 kHz |
| Eddy/Flo/奶奶/爷爷/Reed/Rocko/Sandy/Shelley × 中英 (Kona) | `kona` | **系统内置 Eloquence/ViaVoice 直驱**（进程内，零物料依赖） | zh-CN / en-US | 16 kHz |

- 永德：`voices/` 下每个 `ydvoiceNN.vl` 就是一个发音人，首次使用某发音人时加载；
- MS 引擎：`model/` 下每个含 `SynthModel.license` 的包 = 一个发音人（懒建会话）。
  **仅 .key 的 Natural 包不可用**（传入 `set_speech_synthesis_voice` 后合成
  reason=1，需服务器侧激活）。NeuralVoices 共 14 包（离线备份在
  `resources/ms-voices/NeuralVoices/`），license 型 7 个已全部装入（中文 3 + 英文 4）。想再增加发音人：用 `scripts/fetch-voice-models.sh`
  下载另一个 **HD/license** 包放进 `Engines/<key>/model/`，重装 App 即自动列出。
  `voice.txt`（完整语音名）可指定该引擎的默认发音人。语音 locale 按包目录名
  尾段解析（en-US/zh-TW 等），未知回退引擎级。

## 语音方案（中英自动分发）

系统语音目录里有**方案槽位**「语音方案·一 ~ 语音方案·八」（id 池固定 dual1..dual8，
仅注册已创建的）：一次朗读内按文字自动切换主（中文）/副（英文）发音人——中文段、
英文段、数字、孤立字母各走各的音色，段与段无缝拼接（相邻段必不同侧，这是 MS 会话
串行的前提，切段器保证；音高变体的孤立字母段例外，见下）。

- **方案管理**：App 主界面方案列表 + 右侧 新增/编辑/删除/试听；编辑器含方案名称、
  主要引擎与参数、**启用第二语音**开关（关 = 单语音朗读，全部文本归主侧后重并）、
  分发规则。主/副侧默认语言（中文主/英文副）收敛在 `SchemeSides`，将来接系统
  语言判断时只改这一处。
- **大小写字母处理**（主要引擎参数）：朗读**孤立的单个大写字母**时可加「大写」/
  「Capital」前缀或提高音高（+15 桥刻度）。音高变体让字母独立成段不与邻段合并，
  渲染层对同 (engine,voice) 段串行启动（`popLaunchIndex` 内守卫）。
  ⚠️ 宿主限制（2026-10-18 实测）：系统语音前端会把**中文语境的孤立字母统一小写化**
  （`say "果 Z 味"` 到达 appex 时已是 `果 z 味`；拉丁语境如 `press Z now` 保留
  大小写），故该功能在系统朗读路径上仅对拉丁语境字母生效；App 内试听不走系统
  前端，完整生效。
- **分发规则**：`Shared/TextSegmenter.swift`（normalize → splitRuns → route →
  attach → merge 五段流水，特殊情况在此按条增改）；规则开关随 scheme.json 持久化。
- **参数完全 MagicTTS 托管**：方案语音忽略请求里的 prosody（VO 面板对方案本就
  无效），语速/音高/音量两侧各自独立，音量 = 渲染级逐样本增益（0-200%，兼做
  响度归一；>100% 可能削顶）。
- **配置热生效**：实际映射存 `Engines/scheme.json`，**每次请求时读取**，App 方案
  编辑器改完即生效，无需重装；增/删方案后 App 调 `updateSpeechVoices` 让目录
  重查。文件缺失/损坏 → 回退默认两条；已删槽位的残留请求回退默认方案。
- 逐段流水线：启动线程领先渲染位置 1 段预热下一段，消除段间切换静默；跨采样率
  段（永德 16k ↔ MS 24k）按段切换重采样率。

## 朗读替换规则

朗读前按序做文本替换，修正多音字、英文缩写读法、数字符号等（如「重新→虫新」
让"重"读 chóng、`(\d+)% → 百分之$1`）。全局一份 `Engines/rules.json`，App
「朗读替换规则」编辑（普通文本/正则/忽略大小写/英文整词，逐条可停用、实时
预览）；普通引擎语音全局应用，双语方案可在方案编辑里整体关闭。与 scheme.json
同模式按文件修改时间缓存——改完即时生效，无需重装。

> **流式与延迟**（2026-10-17 实测）：MS 引擎改为 `start_speaking_ssml` + wav 文件
> 跟随的流式架构——首块音频 ~0.73s（模型首次推理）后即开始朗读，合成以 ~1.6×
> 实时供给；appex 进程与合成会话跨句复用，首句之后即热。旧架构需等整段合成
> 完才出声（长段落延迟随篇幅线性增长）。say 总时长里另有 ~1.1-1.4s 的系统
> out-of-process AU 管道固定开销（不阻塞出声）。

## 目录结构

```
App/            SwiftUI 宿主（引擎状态表 + 方案编辑/试听 + 朗读规则编辑 + 刷新注册）
Extension/      AU 扩展（MagicSpeechProviderAU 段队列渲染 + SSML 解析）
Shared/         App Group 路径约定 / Scheme 方案模型 / TextSegmenter 分发规则 / 朗读替换规则
Bridge/         C 引擎桥（路由 + 各引擎适配器）
Entitlements/   沙盒 + App Group
scripts/        构建 / 数据安装 / 下载 / 诊断工具
docs/           永德引擎逆向笔记与编码公式（权威）
```

## 构建与安装

```bash
./scripts/build-clt.sh                    # 产出 build/MagicTTS.app
cp -R build/MagicTTS.app ~/Applications/
open -a ~/Applications/MagicTTS.app       # 启动即刷新发音人注册
```

- 签名：优先使用钥匙串里的真实证书（当前 `MagicToolbox Dev`），无证书退回
  ad-hoc。真实证书下 TCC 权限授权可被系统记住。
- 无 Xcode 依赖（Command Line Tools 即可）。改了 `Bridge/` 后**必须**重新构建并
  替换 `~/Applications` 里的 app——源码新不等于安装新。
- 替换后：`pkill -f MagicTTS; pkill -f MagicTTSSynth`（残留旧扩展进程会让 say
  挂起）。

## 引擎数据

运行位置：`~/Library/Containers/com.magictts.voices.synth/Data/Engines/<key>/`
（**appex 自有沙盒容器**。曾用 App Group 容器——沙盒扩展读组容器会被 TCC 视为
"访问其他 App 的数据"，每句合成都弹权限框且授权记不住；自有容器根治。
宿主 App 不沙盒化，直接读同一绝对路径，首启最多弹一次框且会被记住。）

```bash
./scripts/populate-engines.sh             # yongde←项目资源; MS←旧容器或从零
./scripts/fetch-ms-sdk.sh                 # 从零: 下载微软 Speech SDK 运行库
./scripts/fetch-voice-models.sh <用户名> <密码>   # 从零: 下载语音包 (需广荣服务器账号)
```

- `yongde/`: `voices/ydvoice00-04.vl`（24B 头 + 24576×8B 稀疏索引 + PCM）+
  `pinyin.txt`（41923 字）+ `phrases.txt`（47120 词组）
- `naturalvoice|onecore/`: 3 个 dylib + `model/<包>/` + `voice.txt`

## 项目内资源（自包含，不再依赖旧工作区素材）

> VVTTS（IBM ViaVoice 中文引擎）调研完结，集成已被 Kona 直驱取代；素材
> （APK/apktool 树/解出的 6 个 i386 PE DLL/听感样本）已随 2026-10-18 工作区
> 清理删除，复盘需重新获取 VVTTS APK 并用 `tools/unpack_vvtts.py` 重解——
> 容器格式与全部结论见 `docs/NOTES-vvtts.md`。

```
resources/
├── yongde/
│   ├── voices/ydvoice00-04.vl   # 全部 5 个音色语音库 (~51M, 未入 git)
│   └── pinyin.txt / phrases.txt # 拼音词典
└── ms-voices/                   # NeuralVoices 微软语音包离线备份 (1.4G, 14 包 / 7 个 license 型, 未入 git)
```

永德数据以 `resources/yongde` 为权威源，`populate-engines.sh` 默认从这里安装；
MS 引擎数据体量大（容器内现 924M），运行态由容器 + 下载脚本承担，语音包离线
备份在 `resources/ms-voices/NeuralVoices/`（含 7 个 HD `.license` 包；
`fetch-voice-models.sh` 依赖广荣服务器账号，未必随时可下）。
2026-10-18 工作区收敛至本仓库：反编译树/其他平台包/APK/引擎二进制
（tts.apk、grtts.nvda-addon、VVTTS 素材、libyongde_tts.so 等）已删除，
仅保留运行所需语音数据；逆向结论以 docs/ 与 git 历史为准。

## 诊断

```bash
swift scripts/check_system_voices.swift   # 语音目录 + AU 组件 + 探针 一页看
```

## CLI 快速验证

```bash
clang -std=c11 -IBridge/include -o /tmp/mtts_cli scripts/test_cli.c Bridge/*.c -ldl -lpthread
MT_ENGINES="$HOME/Library/Containers/com.magictts.voices.synth/Data/Engines" \
  /tmp/mtts_cli yongde "中东西通红龙用"
say -v '永德·语音0' "中东西通红龙用"
say -v '语音方案·一' "中文与English混读，NVDA version 3.14，字母Z分组。"   # 默认方案 = Eddy 中文+英文 (Kona)
```

调试转储**默认关闭**（正式路径零磁盘 IO）。需要引擎侧 WAV 比对时：
`touch <容器>/Engines/.debug/dump`，之后每次合成在 `Engines/.debug/last_<n>.wav`
留下渲染侧输出（轮转 8 个）。

## 已知坑

1. **替换 app 后要 `pkill -f MagicTTS; pkill -f MagicTTSSynth`**——残留 appex
   进程会让后续 `say` 挂起。
2. **改完 app 重装后若语音没出现在系统目录**：`AVSpeechSynthesisProviderVoice.
   updateSpeechVoices()` 对"未变化的 app"不触发重枚举。可靠触发法 = 重装 app
   (`rm -rf` + `cp -R` + `open`)，或删除/安装任一带语音扩展的 app，或重启。
   （旧坑清单里的 `killall speechvoiced` 在 macOS 26 上该进程根本不存在。）
3. **`speechVoices` getter 必须零文件依赖可用**：系统枚举发音人时在守护进程
   上下文里实例化 AU，那里读不了 appex 容器——扫描拿不到引擎数据。getter
   末尾的静态兜底表就是为这个准备的（2026-10-17 回归根因：无兜底时枚举侧
   永远上报 0 条 = 发音人从不注册，且 App 内试听正常、极易误诊为"管道僵死"）。
4. **沙盒内外 home 含义不同**：appex 内 `homeDirectoryForCurrentUser` = 容器
   Data 本身；宿主/CLI 内才是真实家目录。拼引擎路径必须区分进程上下文
   （`MagicPaths.appexContainerData`，同日事故根因二：拼同一条相对路径得到
   容器内嵌套路径，appex 永远找不到引擎 → say 空音频 0.01s）。
5. **AU 组件码 (subtype/manufacturer) 是语音目录的缓存键**，首次枚举成功后
   与当时的 appex bundle id 绑定（voice id 前缀由此而来）。身份实验轮换后
   不要复用旧组件码，否则目录持续用陈旧前缀（现用 `mtT5`）。
6. MS 引擎冷启动数秒；渲染块有 15s 有界等待（离线 AU 模式）。

## 二期方向

流式合成（MS Synthesizing 事件渐进输出）、Sonic 变速接入 yongde 的 rate、
音节间微停顿调优、ydvoice01-04 音色切换、无锁环形渲染缓冲、公证分发。

## 出处文档

- `docs/NOTES-voice-catalog.md` — 系统语音注册机制实测笔记（目录管道形态、两条代码
  不变量、症状→定位排障手册、2026-10-17 事故档案）— 注册/合成问题先读它
- `docs/NOTES-yongde-re.md` — 永德引擎完整逆向笔记（.vl 格式、编码公式、勘误）
- `docs/yongde-formula.json` — 槽位编码权威表（`slot = ZC[声母] + F[韵母] + 声调号`）
- `docs/NOTES-ms-streaming.md` — MS 引擎流式架构与 SDK 状态机红线
