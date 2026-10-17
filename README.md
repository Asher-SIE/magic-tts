# Magic TTS

macOS 多引擎中文语音合成器 — 通过 `AVSpeechSynthesisProviderAudioUnit` 扩展把三个
TTS 引擎注册为系统语音（VoiceOver / 朗读内容 / `say` 均可用）。

## 语音（按发音人列出）

每个引擎的**全部发音人**都会注册为系统语音：

| 发音人 | 引擎 key | 类型 | 采样率 |
|---|---|---|---|
| 永德·语音0 ~ 语音4 | `yongde` | 逆向复刻拼接引擎（槽位编码 + 5 个 .vl 音色库），按句自动切换 | 16 kHz |
| 自然语音·晓晓 HD | `naturalvoice` | 微软嵌入式神经网络（流式） | 24 kHz |
| OneCore·晓晓 HD | `onecore` | 微软嵌入式神经网络（流式） | 24 kHz |

- 永德：`voices/` 下每个 `ydvoiceNN.vl` 就是一个发音人，首次使用某发音人时加载；
- MS 引擎：`model/` 下每个含 `SynthModel.license` 的包 = 一个发音人（懒建会话）。
  **仅 .key 的 Natural 包不可用**（传入 `set_speech_synthesis_voice` 后合成
  reason=1，需服务器侧激活）。想增加发音人：用 `scripts/fetch-voice-models.sh`
  下载另一个 **HD** 包放进 `Engines/<key>/model/`，重启 App 即自动列出。
  `voice.txt`（完整语音名）可指定该引擎的默认发音人。

> **流式与延迟**（2026-10-17 实测）：MS 引擎改为 `start_speaking_ssml` + wav 文件
> 跟随的流式架构——首块音频 ~0.73s（模型首次推理）后即开始朗读，合成以 ~1.6×
> 实时供给；appex 进程与合成会话跨句复用，首句之后即热。旧架构需等整段合成
> 完才出声（长段落延迟随篇幅线性增长）。say 总时长里另有 ~1.1-1.4s 的系统
> out-of-process AU 管道固定开销（不阻塞出声）。

## 目录结构

```
App/            SwiftUI 宿主（引擎状态表 + 试听 + 刷新注册）
Extension/      AU 扩展（MagicSpeechProviderAU + SSML 解析）
Shared/         App Group 路径约定
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

```
resources/
├── yongde/
│   ├── voices/ydvoice00-04.vl   # 全部 5 个音色语音库 (~51M, 未入 git)
│   ├── pinyin.txt / phrases.txt # 拼音词典
│   └── libyongde_tts.so         # Android arm64 引擎本体 (二期逆向参考: Sonic 变速/多音色)
└── reference/
    ├── tts.apk                  # 原始 APK (347M, 未入 git) — 一切素材的最终出处
    └── restored*/               # Android 调用链 Java 还原源 (注释出处引用)
```

永德数据以 `resources/yongde` 为权威源，`populate-engines.sh` 默认从这里安装；
MS 引擎数据体量大（870M），不入项目目录，由组容器 + 下载脚本承担。

## CLI 快速验证

```bash
clang -std=c11 -IBridge/include -o /tmp/mtts_cli scripts/test_cli.c Bridge/*.c -ldl -lpthread
MT_ENGINES="$HOME/Library/Group Containers/com.magictts.shared/Engines" \
  /tmp/mtts_cli yongde "中东西通红龙用"
say -v '永德' "中东西通红龙用"
```

每次合成会在 `Engines/.debug/last_<key>.wav` 留下引擎侧输出，用于与宿主收到的
音频比对（相差应仅为 float 往返 1 LSB）。

## 已知坑

1. **替换 app 后要 `pkill -f MagicTTS; pkill -f MagicTTSSynth`**——残留 appex
   进程会让后续 `say` 挂起。
2. **新 AU 组件首次不被发现**：`killall speechvoiced` 强制语音目录重扫。
3. MS 引擎冷启动数秒；渲染块有 15s 有界等待（离线 AU 模式）。

## 二期方向

流式合成（MS Synthesizing 事件渐进输出）、Sonic 变速接入 yongde 的 rate、
音节间微停顿调优、ydvoice01-04 音色切换、无锁环形渲染缓冲、公证分发。

## 出处文档

- `docs/NOTES-yongde-re.md` — 永德引擎完整逆向笔记（.vl 格式、编码公式、勘误）
- `docs/yongde-formula.json` — 槽位编码权威表（`slot = ZC[声母] + F[韵母] + 声调号`）
