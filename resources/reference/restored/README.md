# 广荣TTS (com.sgr.grtts) — 核心调用链还原

> 来源: `tts.apk` (apktool 反编译产物 `tts/`)。作者代码经 R8 混淆分散在 `com.sgr.grtts`（8 类，未混淆）
> 与 `n2`（约 220k 行 smali，混淆）中；第三方 SDK（讯飞 MSC、微软语音、Vocalizer）未混淆。
> 本目录还原 **TTS 合成主链路**（不含设置界面/支付/授权/防篡改等无关逻辑）。

## 1. 整体架构

```
Android 系统 TTS 请求
  └─ EmbeddedTtsEngine (TextToSpeechService)          [com/sgr/grtts/EmbeddedTtsEngine.java]
       ├─ onCreate  → TtsEngineManager.init()          后台线程 "grtts_init"
       ├─ onSynthesizeText
       │     ├─ engineManager.plan(text)               文本规范化 + 中英分段 + 语音路由
       │     └─ engineManager.synthesize(job, 桥)       逐段分发引擎合成 + 重采样 + 回调
       ├─ onStop     → engineManager.stop()
       └─ onDestroy  → engineManager.shutdown()
```

## 2. 类映射表 (混淆名 → 还原名)

| smali | 还原 | 说明 |
|---|---|---|
| `com/sgr/grtts/EmbeddedTtsEngine` | `EmbeddedTtsEngine` | TTS 服务（完整还原） |
| `com/sgr/grtts/GrTtsNative` | `GrTtsNative` | 永德 JNI（完整还原） |
| `com/sgr/grtts/EloquenceNative` | `EloquenceNative` | Eloquence JNI（完整还原） |
| `n2/r5` | `tts.core.TtsEngineManager` | 引擎管理器（核心方法全还原） |
| `n2/i5` | `tts.core.SynthesisJob` | 合成任务 |
| `n2/l5` | `tts.core.SpeechSegment` | 分段（文本+目标语音+韵律） |
| `n2/e5` | `tts.core.TextLangSegment` | 分段+语言（永德分段产物） |
| `n2/s5` | `tts.core.VoiceDescriptor` | 语音描述符（14 字段） |
| `n2/m0` | `tts.core.EngineType` | 引擎枚举（9 值） |
| `n2/m5` | `Lang` | 语言枚举（中/英） |
| `n2/e6` | `Gender` | 性别枚举 |
| `n2/k5` | `VoiceSelection` | 主/回退语音选择 |
| `n2/l0` | `EngineVoice` | 引擎+语音对 |
| `n2/j5` | `tts.core.Resampler` | 重采样流包装 |
| `n2/q5` | `tts.core.AudioPump` | 音频泵（重采样→回调） |
| `n2/p5` | `tts.core.SegmentSink` | 段音频出口 |
| `n2/o5` | `tts.core.StoppedCheck` | 停止检查 |
| `n2/j0` | `EmbeddedTtsEngine` 内桥 | short[]→byte[]→audioAvailable |
| `n2/i0` | 服务内 Runnable(0/1) | grtts_init / grtts_voice_refresh 线程体 |
| `n2/v0`+`n2/q0`+`n2/t0`+`n2/s0` | `tts.engines.IssTtsEngine` | 讯飞 ISS xTTS 40/30 双核 |
| `com/iflytek/speech/libisstts\|libxsstts` | 同名 | ISS JNI 绑定（完整还原） |
| `n2/j6` | `tts.engines.XunfeiEngine` | 讯飞 MSC (libmsc) |
| `n2/b0`+`v3/d0` | `tts.engines.EloquenceEngine` | Eloquence (ECI) |
| `n2/f1` | `tts.engines.MsEmbeddedEngines.NaturalVoiceEngine` | MS 嵌入式（流式 24k） |
| `n2/u1` | `tts.engines.MsEmbeddedEngines.OneCoreEngine` | MS 嵌入式（整段） |
| `n2/z5` | `tts.engines.VocalizerEngine` | Nuance Vocalizer (codefactory) |
| `n2/a5` | `tts.engines.SystemTtsEngine` | 系统 TTS 转发 |

## 3. 核心调用链（合成一次语音的完整路径）

### 3.1 初始化 `TtsEngineManager.init()` [n2/r5.E]

1. `filesDir/yongde_data` 建立；assets 拷贝 `pinyin.txt`/`phrases.txt`；扫描语音数据（`ydvoice*.vl` 拷入后）
2. **永德**: `GrTtsNative.initialize(path)`（全局锁 J + 引用计数）→
   `getVoiceCount/getVoiceId/getVoiceName/getSampleRate` 枚举语音
3. **讯飞 MSC**: `XunfeiEngine.initOk()` → 注册 17 个语音（xiaoyan…john，对应 `assets/tts/*.jet`，16kHz）
4. **ISS xTTS**: assets/isstts → `filesDir/isstts`（版本标记 `2026-07-04-11-iss13-offline`，变更重拷）；
   双核探测 `40/xTTSRes/isstts.cfg`、`30/xTTSRes/isstts.cfg`
5. **NaturalVoice / OneCore**: 各自模型目录存在则 `EmbeddedSpeechConfig.fromPath` 加载
6. **Eloquence**: ABI 需含 arm64-v8a → `loadLibrary("eloquence_jni")` → 数据部署 → `nativeInit`
7. **SYSTEM**: 枚举系统其他 TTS 引擎
8. 校验 `primary_engine`/`english_engine` 偏好，迁移并钳制 rate/volume/pitch/rate_multiplier

### 3.2 规划 `plan(text)` [n2/r5.O]

1. 应用引擎状态 → 文本规范化链: 自定义替换 → 繁→简 → 数字/日期展开 → 用户词典
2. 永德引擎额外 `GrTtsNative.normalizeDateTime`
3. `splitByLanguage` [n2/r5.d]: 永德主引擎用 `GrTtsNative.segmentText + getSegmentLanguages`；
   否则按中英文字符边界切分；中文段→主语音，英文段→英文引擎语音；丢纯标点段、合并相邻同语音段
4. 采样率取各段语音的最大值 → `SynthesisJob(originalText, sampleRate, segments)`

### 3.3 合成 `synthesize(job, callback)` [n2/r5.c0]

```
for each SpeechSegment:
    代际失效检查 (stop() 会推进 synthesisGeneration)
    授权收紧检查 (n2/g.c() — 仅免费引擎时阻止付费引擎)     ← 付费门控，未深入还原
    Resampler(voice.sampleRate → job.sampleRate)           引擎间采样率统一
    switch (voice.engine):                                  [c0 pswitch 0..7]
      SYSTEM       → SystemTtsEngine.synthesize (TextToSpeech 转发, 带 system_tts_origin 防递归)
      ELOQUENCE    → EloquenceEngine.synthesize (nativeSetProsody → nativeSynthesize → nativeStop)
      ONECORE      → OneCoreEngine.synthesize (SpeakSsmlAsync 整段)
      NATURALVOICE → NaturalVoiceEngine.synthesize (SpeakSsmlAsync + Synthesizing 事件流式)
      ISSTTS/30    → IssTtsEngine.synthesize (openSession → setParam×N → start → getAudioData 循环)
      XUNFEI       → XunfeiEngine.synthesize (synthesizeToUri + sonicStream* 变速)
      VOCALIZER    → VocalizerEngine.synthesize (VocalizerEngine.D + sonicStream* 变速)
      YONGDE       → 全局锁 J 内: setVoice/setRate/setVolume/setPitch/setRateMultiplier
                     → GrTtsNative.synthesize(text) → short[]
    音频路径: 引擎 PCM → resampleStreamProcess(统一采样率) → 桥
              桥: short[]→小端 byte[] → 按 getMaxBufferSize 分块 → SynthesisCallback.audioAvailable
```

### 3.4 停止/关闭

- `stop()` [b0]: 代际自增使在途会话全部失效 → 各引擎 stop（含 `GrTtsNative.stop`、`EloquenceNative.nativeStop`、`SpeechSynthesizer.StopSpeakingAsync`…）
- `shutdown()` [a0]: 永德引用计数释放（归零 `uninitialize`）→ 各引擎 destroy/close

## 4. 引擎与 native 库对照

| 引擎 | native 库 | 数据 |
|---|---|---|
| YONGDE | libyongde_tts.so | assets/voices/ydvoice00–04.vl → filesDir/yongde_data |
| XUNFEI | libmsc.so (com.iflytek.cloud) | assets/tts/*.jet (17 语音) |
| ISSTTS 4.0 | libissxtts40.so + libissauth.so (+xtts/lesl/opencc/idas/lzmq/zmq/websockets/gnustl) | assets/isstts/40 → filesDir/isstts/40 |
| ISSTTS 3.0 | libissxtts30.so + libissavth.so | assets/isstts/30 |
| ELOQUENCE | libeloquence_jni.so (+libeci/libchs/libchsrom/libenu) | 仅 arm64-v8a |
| NATURALVOICE/ONECORE | Microsoft.CognitiveServices.Speech.embedded.tts(.runtime) | 嵌入式模型目录 |
| VOCALIZER | libNuanceVocalizer.so (es.codefactory) | — |

## 5. 偏好键 (SharedPreferences "grtts_prefs")

`primary_engine`, `english_engine`（引擎枚举名）, `primary_voice_id`, `english_voice_id`,
`rate`(0–500, 默认100), `volume`(0–100), `pitch`(0–100), `rate_multiplier`

## 6. 尚未还原（按需求刻意跳过 / 留桩）

- `splitByLanguage` 的完整合并细节 → smali `n2/r5.smali` 的 `d/i/B/a` 四方法
- 各引擎数据安装细节（assets 拷贝循环）→ `n2/r5.f/g/h`、`n2/f1.m`、`n2/u1.h`
- 授权/付费门控 `n2/g`、防篡改 `i4/h.M`、设置/支付界面 —— 与 TTS 主链路无关
- MS 嵌入式语音的 SSML 构建与流式会话细节 → `n2/f1.b/u/m`、`n2/u1.c/m`

> 还原源码中的空方法体/桩均标注了对应的 smali 方法名，可按需继续补齐。
