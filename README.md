# Magic TTS

macOS 多引擎中文语音合成器 — 通过 `AVSpeechSynthesisProviderAudioUnit` 扩展把三个
TTS 引擎注册为系统语音（VoiceOver / 朗读内容 / `say` 均可用）。

## 语音

| 语音名 | 引擎 key | 类型 | 采样率 |
|---|---|---|---|
| 永德 | `yongde` | 逆向复刻的拼接式引擎（槽位编码 + .vl 语音库），无原生库依赖 | 16 kHz |
| 自然语音 | `naturalvoice` | 微软嵌入式神经网络（晓晓 HD） | 24 kHz |
| OneCore | `onecore` | 微软嵌入式神经网络（当前同为晓晓 HD，见下） | 24 kHz |

> **OneCore 现状**：两个 MS 引擎目录当前都指向晓晓 HD。模型包分两种授权文件：
> `SynthModel.license`（HD 包，离线可用）与 `SynthModel.key`（Natural 包，实测传入
> `set_speech_synthesis_voice` 后合成失败 reason=1，需服务器侧激活）。本地只有晓晓
> HD 带 license，云希 Natural 包只有 .key 暂不可用。想换声音：用
> `scripts/fetch-voice-models.sh` 下载另一个 **HD** 包放进
> `Engines/<key>/model/`，并把 `Engines/<key>/voice.txt` 改成包内 Tokens.xml 的
> 完整语音名即可。

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
./scripts/build-clt.sh                    # 产出 build/MagicTTS.app (ad-hoc 签名)
cp -R build/MagicTTS.app ~/Applications/
open -a ~/Applications/MagicTTS.app
# 若语音未出现在 say -v '?' 列表: killall speechvoiced 后重试
```

无 Xcode 依赖（Command Line Tools 即可）。改了 `Bridge/` 后**必须**重新构建并
替换 `~/Applications` 里的 app——源码新不等于安装新。

## 引擎数据

运行位置：`~/Library/Group Containers/com.magictts.shared/Engines/<key>/`

```bash
./scripts/populate-engines.sh             # yongde←项目资源; MS←旧容器或从零
./scripts/fetch-ms-sdk.sh                 # 从零: 下载微软 Speech SDK 运行库
./scripts/fetch-voice-models.sh <用户名> <密码>   # 从零: 下载语音包
```

- `yongde/`: `voices/ydvoice00.vl`（24B 头 + 24576×8B 稀疏索引 + PCM）+
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
