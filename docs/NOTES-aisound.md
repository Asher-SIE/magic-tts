# AiSound 引擎调研与集成笔记 (2026-10-20)

> 结论先行: AiSoundTts 0.7 (com.xztom.aisound) 的内核是**Rust 复刻的
> 讯飞风格离线合成引擎** (aisound-core, PSOLA 拼接), 消费单一资源文件
> `Resource.irf` (AIRM 容器 + LZMA 压缩, 内含 8 个基础音色, 含永德系列)。
> 本次已把 **调用方式** (JNI API 全语义, 本文 §2-§4) 与 **资源**
> (`resources/engines/aisound/Resource.irf`) 拿入本仓库, 并按本仓库适配器
> 模式接入 `Bridge/aisound_adapter.c` + 引擎表。**引擎本体是 Android
> arm64 ELF 且仅导出 `JNI_OnLoad`, 无 Darwin 版、无公开 Rust 源码** —
> 出声还差一个 macOS 版引擎库 `libaisound.dylib` (ABI 契约见 §6, 对
> Rust 侧是 ~百行 JNI→C shim 的移植量)。

素材: `d:\Desktop\AiSoundTts-0.7-源码包` (APK 逆向重建源码包: smali +
jadx 参考 Java + 全部 so/assets)。以下全部结论均有该包内文件佐证。

## 一、引擎二进制形态 (实证)

| 项 | 值 |
|---|---|
| 文件 | `lib/arm64-v8a/libaisound_jni.so`, 8,590,776 B |
| ELF | 64-bit LSB, **EM_AARCH64 (183)** — 仅 Android arm64 |
| 导出 | **仅 `JNI_OnLoad` 一个动态符号** (native 方法全部运行时 `RegisterNatives`) |
| 语言 | Rust (panic 路径还原 crate 布局): 自研 `aisound-core` (`psola/synth.rs` `psola/ctx.rs` `psola/dsp_tables.rs` `psola/interpolate.rs` `psola/deps.rs` `psola/loudness_normalize.rs` `psola/noise_window.rs`) + `aisound-zd` (`lifted.rs`) |
| 第三方 crate (版本实据) | jni-0.21.1 / lzma-rs-0.3.0 / miniz_oxide-0.9.1 / cesu8-1.1.0 / hashbrown-0.17.1; rustc `b940084d` |
| 方法名字符串 | `.rodata` 连续区可见全部 JNI 方法名+签名 (RegisterNatives 表): `open([BI)J` `close(J)V` `setStyle(JI)V` `speak(J…)[S` `speakStream(JLjava/lang/String;IIIZDDDLcom/xztom/aisound/ChunkListener;)I` `onChunk([S)Z` `lastPanic` `applyGain` `stretch` `voiceFx` `trimNew/Push/Finish/Free` `trimEndClause` `speedNew/Push/Flush/Free` `loudness` |

随包另有两个与合成无关的播放库: `libttssrv_audio.so` (OpenSL ES 低延迟
sink, 经 `NativeAudio`/`NativeOut` Java 胶水) 与 `libc++_shared.so` —
macOS 侧无对应需求 (AU 渲染管线即播放后端), 不引入。

## 二、调用方式 (JNI API, 全语义)

Java 侧入口 `com.xztom.aisound.Native` (单例, `SAMPLE_RATE=16000`,
输出 16kHz/16bit/mono):

| 方法 | 语义 |
|---|---|
| `long open(byte[] irf, int mode)` | 传 **Resource.irf 全量字节** + 模式号 (1..8, 选 .irf 内基础音色) → 引擎句柄; 0=失败 |
| `void close(long h)` | 关句柄 |
| `void setStyle(long h, int style)` | 1=连读 (默认), 0=逐字 (设置项 `one_by_one`) |
| `int speakStream(long h, String text, int rate, int pitch, int flags, boolean hasFx, double fx0, double fx1, double fx2, ChunkListener)` | 流式合成: 每块 16bit PCM 经 `onChunk(short[])` 回调, **回调返回 false 中止**; 返回 0=失败。`flags` 恒 49 (原版硬编码, 含义未逆向) |
| `short[] speak(long h, String, int rate, int pitch, int)` | 批量版 (同参) |
| `String lastPanic()` | 引擎 panic 信息 (失败诊断) |
| `short[] applyGain(short[], float)` / `float loudness(int)` | 增益 / 响度 |
| `speedNew/Push/Flush/Free` (Sonic 式变速) | App 侧语速>100 的后处理 |
| `trimNew/Push/Finish/Free` + `trimEndClause(h, ms)` | 尾静音裁剪 |
| `short[] stretch(short[], double)` / `short[] voiceFx(short[], d,d,d)` | 变速 / 变声 (离线版) |

**调用序列** (AiSoundService.g(), 逐条对应):

1. 句柄池: `HashMap<mode, handle>`, **上限 4, 满 4 全清重开**
   (b() 方法); `.irf` 字节全量缓存复用 (f88b)。
2. `setStyle(h, 1 或 0)` → `speakStream(h, text, rate, pitch, 49, …)`。
3. 返回 0 → 记 `lastPanic`, **close 并移出池**, 下次请求重开
   (panic 自愈; 我们的适配器里 = 同流内立即重试一次)。
4. 文本预处理在 App 侧 (见 §7), 引擎收纯 UTF-8 文本。

## 三、语速/音高刻度 (AiSoundService.smali f() 实证提取)

Android `SynthesisRequest` 刻度 → 引擎刻度:

```
rate  = getSpeechRate() * 速度增益设置
rate  = clamp(rate, 10, 3000)
engine_rate = rate >= 100 ? 100 : rate / 2        # 引擎原生只有慢速域
speed       = rate > 100 ? rate / 100.0 : 1.0     # 快于常速 → App 侧 Sonic 变速
engine_pitch = clamp(getPitch() * 49 / 100, 0, 100)   # Android 100(常) → 49
```

即引擎侧: **rate 域 [5..100], 100=常速, 慢速靠引擎, 快速靠后处理**;
**pitch 域 [0..100], 49=常音高**。本仓库桥刻度 (rate 20..500/100 常,
pitch 0..100/50 常) 的换算写在 `aisound_adapter.c` 文件头。
(引擎对 rate<100 的内部映射是否线性未逆向 — 若日后出声听感异常,
先核此段。)

## 四、发音人表 (a/x.java + MainActivity 官方显示名)

模式号 = `open()` 第二参; fx 三元组 = `speakStream` 变声参数
(依数值推测序为 变速/音高/音量: 唐老鸭 0.738/1.2549/0.965 = 慢+高调,
许宝宝 1.244/1.2549/0.963 = 快+高调, 佐证前两位语义; 第三位未验证)。

| 模式 | 音色 (显示名) | locale | fx |
|---|---|---|---|
| 1 | 晓燕 | zh-CN | — |
| 2 | 晓美 | zh-HK | — |
| 3 | Catherine | en-US | — |
| 4 | 小风 | zh-CN | — |
| 5 | 大龙 | zh-CN | — |
| 6 | 永德燕萍 | zh-CN | — |
| 7 | 永德雨峰 | zh-CN | — |
| 8 | 永德小美（粤语） | zh-HK | — |
| 1+fx | 许久 / 许多 / 小平 / 唐老鸭 / 许宝宝 | zh-CN | 0.587,0.790123,1.025 / 0.516,0.739884,1.03 / 0.907,0.895105,1.012 / 0.738,1.254902,0.965 / 1.244,1.254902,0.963 |
| 6+fx | 永德许久 / 永德许多 / 永德小平 / 永德唐老鸭 / 永德许宝宝 | zh-CN | 同上五组 |

另有 `XIAOYAN_BI`/`XIAOMEI_BI` 双语变体 (同模式 1/2): 引擎调用完全
相同, 双语性只体现在 **App 侧分发** (双语音色不预切中英段, 整段直送
引擎)。本仓库方案系统自带切段分发, 无需该机制。App 选择器共 17 条
(不含 Catherine 与双语变体); 我们目录取 17 + Catherine = 18 条。

## 五、Resource.irf 容器 (字节实证)

- 17,905,905 B, md5 `df7e22053b2f68a31bf4b44512b40358`。
- 头 40 B: `AIRM` + u32 ver=4 + u32 头长 0x28 + 7×u32 (LE),
  **布局已破**: `[块0长, 块1位置, 块1长, 块2位置, 块2长, 块3位置, 块3长]`
  —— 块长 = 相邻标签间距, 与文件尾严丝合缝 (0xd9cf13+0x3769de=文件长)。
- 数据 = 4 个 `AIRZ` 标签块, 块内头 20 B: `AIRZ + a:u32 + b:u32 + c:u32 +
  d:u32 + c:u32`:
  | 块 | a | b | c (×2) | d |
  |---|---|---|---|---|
  | 0 | 1771695c | **18dd**cfaa | 89c45262 | **00d7ee98** (14.2M) |
  | 1 | 20fc5788 | **18dd**fb34 | bff590e7 | 003dfe30 (4.07M) |
  | 2 | 81cd3230 | **18dd**fb34 | b4859524 | 003d5682 (4.02M) |
  | 3 | ef0f41bc | **18dd**fb34 | cf333c6d | 00483b28 (4.73M) |
  d ≈ 解压后尺寸 (合计 ~27M); b 高 16 位恒 0x18dd; c 存两遍 (疑校验/种子);
  a 疑压缩侧校验。payload (头后) 高熵 (~165/256 唯一字节), 无任何已知
  压缩魔数 (zstd/lz4/zlib/gzip/xz/bz2 均查)。
- **解压尝试失败记录** (勿重试): alone/xz 全偏移 4..80; raw deflate;
  raw lzma1 × lc0-3 lp0-1 pb0-2; 合成 alone 头 (props∈{各头字节,0x5D,
  0xE0..0xEF} × size=d × 偏移 24/25)。结论: **块内非裸 LZMA/deflate,
  引擎在解压前/后有自定义混淆或框架 —— 必须 .so 逆向定论** (引擎同时
  链接 lzma-rs 与 miniz_oxide, 何者用于 AIRM 待证)。
- vendor 原厂资源为另一代容器 `IsResV4` (6.7MB, 三语音), 与 AIRM 不同。

## 六、macOS 集成 (本次改动) 与补齐路径

**已落地** (引擎库就位即出声):

| 文件 | 改动 |
|---|---|
| `resources/engines/aisound/Resource.irf` | 资源迁入 (gitignore 按物料二进制惯例; 原件在源码包 `apktool-project/assets/`) |
| `Bridge/aisound_adapter.c` | 适配器: probe (AIRM 魔数) / scan (§4 表 18 音色) / create (dlopen + irf 全量载入) / 流式 (worker 线程 + 队列缓冲 + Sonic 快速档 + panic 重试), 调用序列严格对齐 §2 |
| `Bridge/engine_router.c` | 引擎表 +aisound, 槽位 4→5, 发音人槽 16→32 (kona 16 满需余量) |
| `App/EngineImporter.swift` | 物料识别 + `Resource.irf` |
| `.gitignore` / `README.md` | 相应条目 |

**引擎库 C ABI 契约** (`Engines/aisound/libaisound.dylib` 需导出, 即
JNI 层的 C 化镜像):

```c
typedef int (*as_chunk_fn)(const int16_t *pcm, int64_t n, void *user);
long       as_open(const void *irf_bytes, int64_t irf_len, int voice_mode);
void       as_close(long handle);
int        as_set_style(long handle, int style);
const char *as_last_panic(void);
int        as_speak_stream(long handle, const char *utf8_text,
                           int rate, int pitch, int flags,
                           int has_fx, double fx0, double fx1, double fx2,
                           as_chunk_fn on_chunk, void *user);
```

补齐路径: Rust 源 (aisound-core/aisound-zd) 以 aarch64-apple-darwin
目标重编 + 把 jni crate 入口换成上述 5 个 `#[no_mangle] extern "C"`
(~百行) 即可; 物料与适配器均已就绪。**排除的路线**: 直接加载 .so
(Android ELF/arm64+bionic, Darwin 不容); Box64/qemu-user (Linux 用户态
语义, appex 沙盒不容, 同 VVTTS 结论); 纯 C 复刻引擎 (需先破 AIRZ 块内
单元库格式 + PSOLA 参数, 工作量 ≥ 永德逆向, 见 §5 未逆向项)。

## 七、原版 App 侧管线 (备查, 本仓库已有等价物)

- **切段**: 标点 (。！？；，、：…\n,;!? 与句尾 .:) + 30 字上限切分;
  非双语音色再把中文单字 / 拉丁词组拆开逐段喂 (c()/e() 方法)。
  ≈ 本仓库 `TextSegmenter`。
- **分块后处理** (a/e.onChunk): `trimPush` 尾静音裁剪 → `speedPush`
  变速 (rate>100) → 增益/限幅 (`volume_gain`/`volume_limit` 设置) →
  `NativeOut` (OpenSL 原生 sink, 失败回退 AudioTrack)。
  ≈ 本仓库馈送线程尾静音裁剪 + 渲染级增益 + Sonic。
- 引擎失败自愈: close → 池移除 → 重开 (§2.3)。

## 八、引擎血统与 Windows 版 (2026-10-20 补查, NVDA 插件包实证)

NVDA 中文站有 [Sunshine AiSound 合成器](https://www.nvdacn.com/index.php/archives/1588/)
(0.1.1, 普通话/粤语/英语 = 晓燕/晓美/Catherine)。解包
`SunshineAISound-0.1.1.nvda-addon` (zip) 得:

| 文件 | 真身 |
|---|---|
| `AiSound.dll` (326KB, x64 PE) | **原厂引擎** (非 Rust 复刻): .strings 含讯飞标记 `IFLYTEKKETYLFI`; PDB 路径 `D:\ScreenReader\bin\Release\plugins\TTS\AiSound.pdb` — 某国产读屏器的 TTS 插件 (与争渡读屏 aisound.dll 补丁帖互证) |
| `SSNAPI.dll` (84KB) | 兼容宿主: 把原厂插件桥给 NVDA (装音频钩子捕获 PCM) |
| `TTS/AiSound/Resource.irf` (6.7MB) | 原厂资源, **容器是 `IsResV4` 代** — 与安卓包的 `AIRM` v4 (17.9MB, 8 模式) 是两代封装; 引擎运行时按 `SsnGetPath` 给的插件目录自读 |
| `sunshine.py` / `_engine.py` | Python 驱动源码, Windows 调用链全部可读 |

**结论: 血统 = 讯飞系原厂引擎 (Windows 插件) → 安卓 Rust 复刻 (AIRM 重打包)
→ 我们要的 Darwin 版两者都没有公开产物。**

Windows 侧调用链 (与安卓 JNI 层完全不同的另一套 API, 备查):
`SSNAPI`: `SsnInstallAudioHook`/`SsnSetAudioCallback`(PCM 16k/16bit/mono
回调)/`SsnSetStatusCallback`(token+状态机)/`SsnGetFuncById(id)` 按号取函数 —
6=openVoice 7=closeVoice 8=speak(voiceId,wtext,token) 10=stop
11..18=get/set volume/pitch/rate/device (NVDA 百分制 0..100,
默认 rate 50/pitch 50/vol 100, speak 前逐段 set);
`AiSound.dll`: `PiPlugInEntry`/`PiInit`/`PiUnInit`。
voiceId: `xiaoyan`/`xiaomei`/`catherine` (via `SsnRegisterVoiceInfo` 注册)。

## 九、编译验证 (Windows 侧)

本机无 Mac, 用 winget 全局安装的 zig 0.17 (`zig cc` 内置 clang+跨平台头)
对 Bridge 做真编译验证 (2026-10-20, 全部通过):

```bash
zig cc -target x86_64-linux-gnu -std=gnu11 -c -Wall -IBridge/include \
    Bridge/<file>.c -o /tmp/<file>.o
# aisound_adapter.c / engine_router.c: 零告警
# yongde_adapter.c / ms_embedded_adapter.c: 仅既有告警 (未动)
```

注意 `-std=c11` 严格模式在 glibc 目标下会藏 `clock_gettime`/`strdup` 声明
(报错属目标差异, macOS 构建不受影响); 用 `-std=gnu11` 与 Darwin 实际可见性对齐。

## 十、Ghidra 逆向实录 (2026-10-20, Windows 快赢, 四轮 headless)

工具链: Ghidra 12.1.4 (`C:\Tools\ghidra_12.1.4_PUBLIC`, JDK 21 于
`C:\Tools\jdk-21`, `launch.properties` 已设 `JAVA_HOME_OVERRIDE`)。
.ghidra 工程已分析存盘 (`C:\Tools\aisound-re\ghidra-proj`, 3467 函数,
Ghidra 自动套用 `rust-common` 类型档案), 后续轮次 `-process -noanalysis`
秒进。脚本: `tools/ghidra_dump_aisound{,2,3,4}.java` (Java 版 — headless
默认无 PyGhidra, .py 不可用)。dump 输出: `C:\Tools\aisound-re\dumps\`。
REA 注意: 其 Windows Ghidra 桥只准入 x86/x64 PE, 本 .so (arm64 ELF) 进
不去 — REA 分析须在 macOS 侧。

### 10.1 RegisterNatives 全表 (JNI 名 → native 实现, 均在 0x25xxxx JNI 簇)

| 方法 | native | 方法 | native |
|---|---|---|---|
| open | `switchD_0025a7d8::caseD_3d` | setStyle | `FUN_0025bdf8` |
| close | `FUN_0025ae90` | stretch | `FUN_0025b4c0` |
| speak | `FUN_0025aee4` | voiceFx | `FUN_0025bb88` |
| **speakStream** | **`FUN_0025d07c`** | trimNew/Free/Push/EndClause/Finish | `0059bae0`/`0025c180`/`0025c210`/`0025d7e8`/`0025cf8c` |
| lastPanic | `FUN_0025c5bc` | speedNew/Free/Push/Flush | `0025bedc`/`0025c80c`/`0025c8a4`/`0025cc40` |
| loudness | `FUN_0025babc` | applyGain | `FUN_0025c3f8` |

(方法名/签名是 Rust 切片拼进 .rodata 字符串池的, 按长度+签名比对落位;
JNI 簇 crate = `aisound_android/src/lib.rs`。)

### 10.2 引擎架构真相 — 不是复刻, 是 "lift"(二进制提升)

.oredata 内嵌两个 AIRZ 加密块, 解密后分别为:
- `0x122277` + 0x3037e B → **原厂争渡 AiSound 的 PE 镜像** (panic 标签
  `"zd_pe_image"`, crate `aisound_zd/src/engine.rs`), 由
  `aisound_lift/src/rt.rs` 自研运行时执行 (0x1000 分页内存表, FUN_0026a154);
- `0x1e5ba3` + 0x8283 B → lift 运行时表 (`aisound_lift/src/rt.rs`)。

**open() 分叉** (caseD_3d 反编译):
- **mode 1-5** (晓燕/晓美/Catherine/小风/大龙) → `FUN_008d440c`: 解
  Resource.irf 条目 0 → 内含 **`IsResV4` 目录** (魔数 `IsResV4` +
  0x18B 行, 行首双魔数 `0x074a358fa2b4c6d1`/`0x190a3f7e2a5c1e9b`), mode 4/5
  按 8/6 字符音色名在目录里选条目装入 zd 运行时; 产物 = 0x2a8 结构;
- **mode 6-8** (永德彦萍/雨峰/小美) → `FUN_0059ae20 → FUN_008d3f9c(条目
  k = mode-5)`: 各自整块喂 **`aisound_yd`** (`src/lib.rs` + `src/arm.rs`
  = 永德 ARM 代码 lifter, FUN_0059b1a0 建 3MB 竞技场+地址映射, 内存布局
  硬编码 0x110000/0x12f800/0x2feffc 等页)。

即: **所谓 Rust 引擎 = 原厂 PE/ARM 代码的宿主, 合成算法本体仍是原厂
二进制**。macOS 意义: 拿 Rust 源重编这条路依旧成立; 但"离线解出数据"
的价值更大 — 音色单元数据可能像永德 .vl 一样可被我们的纯 C 复刻直
接消费 (尤其 mode 6-8 本就是永德系音色)。

### 10.3 AIRM 容器最终定论 (修正 §5 的字段对齐)

```
头 40B:  "AIRM" u32ver(=4) u32hdrlen(=0x28) u32len0 u32off1 u32len1
         u32off2 u32len2 u32off3 u32len3        ← 7 值是 [头长,长0,位1,长1,位2,长2,位3,长3]
条目k:   (u32@(8+8k), u32@(12+8k)) = (offset, length); 条目0 = (0x28, len0)
块 AIRZ: "AIRZ" key:u64@4 crc:u32@12 dec_size:u32@16 crc:u32@20 密文@24
块 AIRC: 同布局, 只解密不压缩
处理:    FUN_008d49bc = KDF(头12B: key+crc) → FUN_008d40a8 解密 →
         FUN_008d5ee0 LZMA-alone 解压 → 校验 CRC32(解压后)==u32@20
         (FUN_008d440c = 条目0 取块; FUN_008d3f9c = 条目k 取块, 供 yd 路径)
```

§5 字段表勘误: 旧解的 a/b 实为 key:u64 的两半; c(×2) = CRC32(解压后);
d = 解压尺寸 — 与本轮反编译完全吻合。块 0 解压后 14.2MB ( zd PE 镜像 +
IsResV4 音色数据), 块 1-3 各 ~4MB ( 永德三音色 )。

### 10.4 密码学组件 (离线提取的最后一环)

| 组件 | 地址 | 身份 |
|---|---|---|
| KDF | `FUN_008d3acc` | 私有 ARX (48 轮, 旋转常量 25/35/30/20/15 随轮 +1, 轮步进 `0xC2B2AE35` = Skein 调度增量, 混合乘 `0x85EBCA6B`), 输入 12B (key:u64+crc:u32) → 256 位密钥; 常量表 8×u64 @ .rodata `0x110fc8` (已提取: `63fe55d2b222a31d 10a51c5e638cb0cf 73b68442032adbf9 9b1c3f40c1225539 83fe45b26523a619 86fd9cdfc4a6241b 00ae02a23d04fb76 9106bf5d0b9a2362`) |
| 流加密 | `FUN_008d40a8` | **ChaCha20 轮结构** (ror 16/12/8/7 四车道 ×10 双轮, 块计数器从 1 起, 64B 块, NEON 尾处理), 状态字为自定义 (0x10d480..0x10d498: `[0,1,2,3]`… 非
标准 sigma); crate `aisound_core/src/pack.rs` |
| 解压 | `FUN_008d5ee0` | LZMA-alone (lzma-rs: 0x8d8284 头解析 + 0x8e97a0 读入 + 0x8d65e8 范围解码) |
| 校验 | — | 反射 CRC-32 (运行时建表, 表区 0x1fe7c0 为变序表) |

**验证 oracle**: 解密+解压后 CRC32 必须等于块头 +20 — 转写正确性可
即测。**下一步 (唯一剩余)**: 按汇编逐指令转写 KDF + ChaCha 状态装配
→ Python 解出全部 4 块 → 解析 IsResV4 目录 (对照 Windows 原厂
AiSound.dll + NVDA SSN 调用链三方印证)。

### 10.5 speakStream 补充语义 (FUN_0025d07c)

- 引擎内部 **rate 域 0..50** (FUN_0025b384 里 `>0x31 则截 0x32`), JNI 层
  或 App 侧必有 ×2 映射 (App 已知传 ≥100 为常速 — 精确换算点待查,
  疑在 speak 的 native `FUN_0025aee4`); pitch 域 0..100。
- **hasFx 时**: `rate = clamp(rate + fx0基率 - 50, 0, 50)`, `pitch =
  clamp(pitch + fx1基调 - 50, 0, 100)` → §4 表 fx 三元组实为
  **(基率, 基调, 基量)** — 久许 0.587=慢读, 唐老鸭 1.2549=高调, 与旧
  推测一致且现在坐实前两位。
- 参数经控制码下发: 0x502=rate, 0x503=pitch (FUN_0059bd48, lifted 运行
  时通道); setStyle 走 `FUN_0059bdec` 同通道。
- 返回值: 1 = 正常完成, 2 = ChunkListener 回调返回 false 中止。
- 句柄是 enum: 判别值 0/1 = 两种引擎形态 (FUN_002600c4/FUN_002637a0),
  其余 = lifted zd 路径 (FUN_0059b048 携回调蹦床 `PTR_FUN_009282e8`)。
