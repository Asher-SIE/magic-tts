# 永德引擎复刻 — 逆向笔记（2026-10-17）

> 目标：无 Darwin 引擎本体（Windows 编译在 grtts.dll 内、Android .so 为 bionic ELF），
> 以逆向数据格式 + 算法 + 公开组件**重实现** macOS 版。

## 1. 架构（来自 libyongde_tts.so 未 strip 的 C++ 符号，194KB）

```
YongDeEngine::synthesize(text)
  → PinyinDict::textToPinyin   (词典 = mozillazg/pinyin-data + phrase-pinyin-data, 公开!)
  → unordered_map<string,int>  (拼音 → 槽号)
  → VoiceLibrary::getPCM(槽号) ×N (vector<uint8_t>)
  → 拼接 → YongDeEngine::processSpeed(pcm, rate) → sonic 变速 (开源, Sonic 库)
```
其余公开组件：TextSegmenter（中英分段）、yongde::text::normalizeDateTime（数字/日期念读）。
调速率：setRate/setPitch/setVolume/setRateMultiplier(默认1.0)。

## 2. .vl 语音库格式（已破解 ✓）

```
u32  slot_count       // ydvoice00 = 24576
u32  version?         // 0x00010001
u32  sample_rate      // 16000 (缺省值也是 16000)
u32  byte_rate        // 32000
u16  block_align      // 2
u16  bits             // 16 (单声道)
u32  reserved
u32  reserved
索引: slot_count × { u32 offset; u32 length }   // [0x18 起], 零条目=空槽
音频: 16bit mono PCM @16kHz
```
- ydvoice00: 1499 非零条目、407.9 秒音频、条目无缝覆盖（0 空洞）
- 加载后存 unordered_map<int 槽号, {offset,length}>
- 同格式推定适用 ydvoice00–04（头部同构）

## 3. 槽号结构（2026-10-17 两轮人耳听审，规则 100% 定案 ✓）

- **组内公式**（20/20 锚点验证）：`槽号 = 组基址 + 偏移`
  - **轻声型组**（组内最大偏移=4，共 124 个）：偏移 0=轻声、1-4=声调1-4（ba/bei/gu/ji/lang/de/duo ✓）
  - **无轻声型组**（最大偏移≤3，共 325 个）：偏移 0-3 = 声调1-4（an/ai/beng/deng/dong/zhan/xue/zuan/zun/a/e/cu ✓）
  - **判型规则是结构性的**（max_offset==4 ⇔ 轻声型），无需听审即可判定
- 大多数组录满全四声（即使无声调字，如 zuan2/zun2）→ 词典声调指纹不可用，
  定名必须靠顺序插值 + 人耳抽查
- **已锚定 21 组**（docs/yongde-groups.json 的 anchors）：a=1, A=8, an=49, ang=289,
  ao=353, ai=369, B=424, beng=497, bei=688, cu=1153, deng=1329, dong=1409, e=1729,
  gu=2816, ji=3872, lang=4864, ruan=7281, zhan=8369, xue=9585, zuan=10609, zun=10801
  （另 ba=416, de=1311, duo=1471, duan=1456, dia≈1505, nie≈5715 待位精确化）
- **特殊条目**：槽 8=字母"A"、槽 424=字母"B"（英文字母混在音节表！）、槽 24=静音、
  槽 1/24 物理位于文件末尾（补录）；若干单条目组（"测""孽"等）性质待查
- 5 个语音库槽布局完全一致 → 映射表全库通用
- 组间顺序近字母序但有例外（ai 在 ao 后、zh 在 x 前）；锚点段间距规整
  （多处恰 49 组/段，暗示底层是固定网格编码）
- 组表产物：docs/yongde-groups.json（449 组: base/kind/members）

## 4. 编码公式（2026-10-17 深夜，✅✅ 二进制级定案 — 无需再听审）

**槽号 = ZC[声母] + F[韵母] + 声调号**（轻声=0，一~四声=1..4；组内缺该声调时
回退到任一存在成员）。

**定案方式**：跳过联立求解，直接解码 0x1c8b4（map builder）的原始指令字节——
自写迷你 ARM64 解码器（movz/movk/add/str 家族），在每次 bl 前对栈镜像做快照，
按 0x20 网格（key@+0x00 SSO、value u32@+0x18）解码、跨快照并集。
声母表 23 项、韵母表 36 项全量恢复（scripts/yongde-formula.json，权威）。

- 声母：b=416 c=832 d=1248 f=2080 g=2496 h=2912 j=3744 k=4160 l=4576 m=4992
  n=5408 p=6240 q=6656 r=7072 s=7488 t=7904 zh=8320 ch=8736 **sh=3328** w=9152
  x=9568 y=9984 z=10400（**w/y 本身是声母行**：wu=w+u、wen=w+en、yong=y+ong）
- 韵母：a=0 ai=368 an=48 ang=288 ao=352 | e=64 ei=272 en=96 eng=80 er=240 |
  o=224 ou=240 | i=128 ia=256 ian=32 iang=192 iao=176 ie=304 iu=384 in=144
  ing=112 iong=160 | u=320 ua=256 uo=224 uai=336 ui=16 uan=208 un=400 uang=192 |
  v=336 ve=16 van=208 vn=400 | ng=96 n=96
- 零声母（synthesize 拆分后 initial 为空时，按韵母首字母分类）：
  a/i/u/v 类 → ZC=0（a=1, an=49, ang=289, ao=353, ai=369 ✓听审）
  e 类 → ZC=1665（e=1729=额e2 ✓）  o 类 → ZC=5825（o=6049, ou=6065）
- 韵母大量合并共享录音：o=uo ia=ua iang=uang iong=ong ui=ve uan=van un=vn
  er=ou en=ng=n
- 切分规则：strip 最长声母（含 y/w），**韵母=剩余原样**（wang=w+ang、you=y+ou、
  yuan=y+uan、yong=y+ong）；仅 ü→v 和 ue→ve 两个归一化（jue/que/xue/yue 走
  ve=16，l+ve 只有 4 声=略、n+ve 只有 4 声=虐，实证）；ju/qu/xu/yu 走 u=320。
- 声调验证：ba轻@416+0、八@417（round-1 直听）；an1@49；ang3@291 缺失与听审
  吻合；mu3@5314=5312+2；ri 组仅 +4（只有 rì 日）；er 组仅 +2..4（儿/耳/二）。
  旧求解版的 p=832 实为 c，其余 ±1/±2 为听审导出偏移噪声，以本表为准。
- 特殊槽：字母 A-Z = 8+416k（26 个全存在）；静音 = 24（6036 字节 ≈0.19s）。
- .vl 索引：offset=文件内绝对字节，**length=字节数**（Σlen == PCM 区大小，
  逐字节吻合）。ydvoice00/01 索引相同（1499 条）；02/03/04 略有增删。
- 覆盖：431 音节库存 383 直接命中；缺失=引擎未录边沿音节（shei/biang/rua/den…）。

## 4b. 适配器（已上线, 2026-10-17 验收 + 当日勘误）

`Bridge/yongde_adapter.c`：.vl 整读 + pinyin.txt/phrases.txt 哈希 +
词组最长匹配/单字首选读音 + ASCII/全角字母走字母槽 + 数字走 kDigits +
标点走静音槽 + 直接拼接。

**二期（2026-10-18）rate/pitch 已接入（Sonic 后处理，对齐原版
processSpeed）**：整段 PCM 拼接完成后过 Sonic —— 桥刻度 rate 20..500 →
`sonicSetSpeed = rate/100`（变速不变调）；pitch 0..100 →
`sonicSetPitch = 2^((pitch-50)/50)`（0.5..2.0，±1 八度，变调不变速）；
常速常调位直通。数值验证：rate 50/200/400/500 时长比 1.995/0.499/0.246/
0.197；同音节文本 r100/r200/r400 F0 均 320.0Hz（变速严格保调）；
pitch 0/100 → F0 104/356Hz（约 ×0.5/×1.9）。Sonic 库 vendor 在
`Bridge/sonic.{c,h}`（Apache 2.0，master 2026-10 快照），build-clt.sh
经 `Bridge/*.c` 通配自动编入。

**勘误①（F[er]）**：初版 F[er]=240 系网格解码串读（与 ou 混淆），实际 **239**。
er 组基=1904，成员 1906/1907/1908=ér/ěr/èr(+2/+3/+4)，+0/+1 空（无 ēr）。
定案手段：F0 基频实测（scripts/yd_f0.py，自相关；1908 强降调 213→148Hz=èr、
1907 升=ér、1906 降升=ěr）+ 用户耳审（1906=儿）。修复前数字"2"/汉字"二"因
声调回退链落到 1906 被读成"儿"。旧锚点"mu3@5314"亦为听审噪声（5314 实为
mú，mu 组三成员 +2/+3/+4）。

**勘误②（-ong 跳字）**：适配器曾漏 `{"ong",160}`（只有 iong），-ong 音节全体
静默丢弃（中/东/同/红/用/龙…），用户听感"很多中文字缺少发音"。源码修复后
**app 构建时间早于该修复**导致症状延续 —— 教训：改 Bridge 后必须重新
build-clt.sh + 替换 ~/Applications + pkill 双进程再验证。

**就近回退表 kFallback**：引擎未录音音节（shei/biang/rua/den/chua/n/m/hm/
fiao/cei/din/bong/wong/nun/零声母eng，共 31 字 0.1%）→ 近似音节替代，
不再静默跳字。

**覆盖数据**（scripts/yd_cover.c）：41923 单字 99.9% 直接命中；
47120 词组 143885 音节 token 仅 8 处需回退（shéi×4 + 注释行）。

## 5. 工具与产物

- 组表+锚点: docs/yongde-groups.json（449 组, 判型/锚点/成员偏移）
- 听审样本: /tmp/yongde_verify/（65 个, 命名 run槽tone）
- RE 工具: radare2（brew）；反汇编片段 /tmp/synth.asm
- 关键反汇编: VoiceLibrary::load @0x15474、synthesize @0x158a0（libyongde_tts.so）
- Sonic 变速库: 已 vendor 到 Bridge/sonic.{c,h}（github.com/waywardgeek/sonic,
  Apache 2.0）—— 二期已用, 见 §4b
