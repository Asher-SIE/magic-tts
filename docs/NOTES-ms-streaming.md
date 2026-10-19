# 微软嵌入式引擎流式架构排障记 (2026-10-17)

从"整段合成后回读"改为流式的全部实测结论。实验工具: `scripts/ms_stream_probe.c`
(mode 1/2/3/4/5)、`scripts/test_cli.c`。

## 可用的 C API 形态 (与公开 C SDK 的差异)

- `synthesizer_start_speaking_ssml(h, ssml, len, &result)` — 4 参 (多一个长度),
  立即返回, 合成后台进行;
- `synth_result_get_audio_data(res, buf, cap, &copied)` — 4 参, 不支持 NULL 探尺寸;
- completed/canceled 回调 ABI: `cb(arg1=synth句柄, arg2=事件句柄)` —
  **set_callback 的 ctx 不回传**, 按 arg1 查进程内注册表路由;
- 音频只走 AudioConfig 目的地: 空 AudioConfig 下 Synthesizing 事件全部 0 字节
  (result 直取同理) → 流式 = **wav 文件跟随** (SDK 渐进写, 首块 ~0.73s,
  ~1.6× 实时; 文件头 44 字节为零, 实为 raw PCM)。

## 状态机红线 (违反即 0 字节空完成 / ORT 段错误)

1. **同一 synth 上并发两次合成** → ONNX Runtime 推理线程段错误
   (ValidateInputOutputCounts)。两引擎 dylib 各自 dlopen, 但链接同一个
   libonnxruntime → 全局连坐 (一个引擎打坏, 另一个也僵)。
2. **推理进行中调 stop_speaking** → 同上 (ORT 僵死)。
3. 读尽上一句后**紧接** start_speaking → 0 字节空完成 (completed 立即到,
   无音频)。**句间 stop_speaking + ~150ms 复位拍后 start 正常**
   (probe M4/M5 与 CLI 交叉验证; M4 无 stop 也成功 — SDK 行为不完全确定,
   适配器统一采用 stop+150ms 的保守路径)。
4. 陈旧 completed 回调会落到"当前 active 流"上 → reader 在 off==0 时
   **不做**完成判定 (门限: 读到过数据才允许 stable-判定)。

## 并发结构 (ms_embedded_adapter.c)

- 每发音人一会话 (懒建): cfg + set_voice(license) + wav acfg + synth + 回调;
- 流对象: 跟随 fd + off + completed/canceled + 稳定判定; 20s 无进展兜底;
- **墓园**: stream_free 不立即销毁 (在途回调可能仍持指针) — 入 vs->grave,
  下次 stream_start 持锁统一回收;
- 发音人枚举与引擎初始化分离 (mtts_global_scan 毫秒级 / init 秒级后台):
  系统查询 speechVoices 时 AU init 必须秒回, 否则注册超时失败。

## 预热之死

后台空跑一句以消掉首推理 ~0.8s 的尝试全部失败: 预热与真实请求的 launch 竞争
落在红线 1/3 上 (即使加 got_real 让位/在途 stop/墓园回收均无法同时满足全部
时序)。**会话跨句复用 ⇒ 首句冷、之后即热**, 预热收益仅每天第一句, 弃。
ms_warm_thread 保留在代码中备查。

## 发音人物料现状

本地可用 HD 包仅 1 个 (晓晓 HD): naturalvoice 与 onecore 各有一份拷贝。
云希 Natural 包只有 SynthModel.key
(⚠️ 修正 2026-10-19 逆向, 见 NOTES-win-re-ms-voices.md: "被拒 reason=1" 的根因
是当时裸传 key 内容 — Windows 侧须传 `"Key:"+内容`, 引擎按前缀走解密路径;
适配器已改, .key 包理论可用, 待实测)。更多发音人需用
fetch-voice-models.sh 从广荣服务器下载 HD 包 (需要账号)。

## 语速封顶与 Sonic 补速 (2026-10-18)

**实测**: 嵌入式 SDK 的 SSML prosody rate 封顶 **200%** — r250/r300/r400
与 r200 的输出逐字节相同 (17564 样本), ≤200% 则真实生效 (r50=1.46s /
r100=1.10s / r200=0.73s, 同文本)。云版 Azure 文档的 rate 上限亦为 200%,
嵌入式行为一致。

**修复** (ms_embedded_adapter.c): 桥刻度 rate 20..500 拆两段 —
引擎原生跑 `min(rate, 200)`, 超出部分由 Sonic 流式补速
(`sonicSetSpeed = rate/200`, 上限 2.5×)。Sonic 嵌在 ms_stream_read
路径: 文件块 → sonicWrite → 循环顶 sonicRead 排干; 终局 (稳定判定/
20s 超时) sonicFlush 后排尽才置 done; cancel 路径不 flush。
≤200% 不建 Sonic 流, 行为与修复前逐字节一致 (实测 26400/17564 样本不变)。
验证: r300/400/500 → 0.48s/0.36s/0.29s (≈ 0.73s ÷ 1.5/2/2.5)。

另一实测: SDK 写的 "wav" 文件**无 RIFF 头** (size = 样本×2, 首段零电平
样本), 即裸 PCM — 流路径自 offset 0 读即为纯样本, 喂 Sonic 无需去头。

## 响应延迟治理 (2026-10-18)

**实测画像** (lat_probe, 短句"好", 适配器全路径):
- 旧版每句固定付 stop+150ms 拍: 热 TTFB 285ms (拍 150 + SDK 热路径 ~135),
  冷 TTFB 948ms;
- 拍的本源 ("读尽后紧接 start 得 0 字节空完成") 实为**陈旧 completed 回调
  误判**, read 的 off==0 守卫加入后已根治 — 裸探针 3 轮句间无 stop 全部
  完整输出。

**修复** (ms_stream_start / ms_stream_cancel):
- 空闲 (active==NULL) 直接 start, 零等待;
- 仅打断在途 (active!=NULL) 时 stop + 30ms 拍 (ORT 红线路径保留余量);
- cancel 的 stop 之后设 50ms 结算窗口 (settle_until, CLOCK_MONOTONIC),
  紧接的 start 等窗口过期; 等待在 vs->lock 外睡, 不阻塞 cancel 路径。

**结果**: 热 TTFB **285→~130ms**, 冷 948→~730ms (每语音每进程一次,
会话复用即热; Windows 侧同模型冷启亦需加载)。打断压测 (start→部分读→
cancel→紧接 start ×20, 两引擎): 无空输出、无挂死、无 ORT 崩, 打断循环
~350ms。

**冷启不再尝试预热的理由**: 跨会话并发推理触碰 ORT 崩溃红线 (见
"预热之死"), 而同语音预热与首个真实请求必然竞争; 锁内串行化会让首请求
等待整个预热周期, 与冷启同价, 无净收益。

## 延迟测量方法论与稳态优化 (2026-10-18 续)

**三个测量陷阱 (本次全部踩过)**:
1. `say` 每次调用是一条新 XPC 连接 → 系统**每命令新开一个 appex 进程**
   (跨进程监控实证: 合成期新进程 CPU 96%/RSS 1.36GB, 旧进程闲置) →
   say 测到的 ~600ms 永远是冷启, 不代表 VO (持久连接) 的稳态;
2. 反复重装 app 会触发已知的注册管道僵死 (见 EngineRegistry 试听注释) —
   say 静默回退系统语音, 测试全部作废; **替换 app 后必须 open 宿主 app**
   (MagicTTSApp 启动即 updateSpeechVoices) 恢复注册;
3. CLI 直测适配器不含 AU/系统调度层, 数字偏乐观; AU 内探针
   (latency.txt: launch=/first-sample=, reqStart→首样本) 才是用户体感。

**稳态数据** (vo_sim: 单进程持久客户端 × 6 句, 1.5s 间隔, = VO 形态):
- 冷句 first-sample ~637ms (每 appex 进程一次: 模型加载+冷推理);
- 句2 58ms (墓园未积); 句3+ 稳定 **78-84ms** (launch ~55ms + SDK 首字节 ~25ms)。
- 墓园保险拍 100ms→20ms 是本轮最大稳态收益 (launch 140→55ms)。

**结构结论**: VO 为持久 XPC 客户端 → appex 进程与 MS 会话跨句常驻,
除进程首句外全程热; 用户"重装后测试仍慢"= 每轮重装+pkill 都让测试句
沦为冷句 + 一次注册僵死假回落。

## 稳态 <50ms 达成 (2026-10-18 终)

三处自伤延迟移除 (适配器相位日志定位):
1. **completed 流的 cancel 不再 stop** — 系统播完照例 cancel, 无脑 stop 设
   50ms 结算窗 → 下一句 launch 白付 ~54ms (相位日志 settle=54 实锤);
   现仅 completed=0 (真打断在途) 才 stop+窗 (ORT 红线路径保留);
2. **墓园去睡眠** — 锁握手即内存栅栏 (回调须持 st->lock 才触碰墓园流),
   原每具 20/100ms 串行 sleep 在积压时叠加至 165ms;
3. AU 热路径: SSML 正则进程级缓存 / 请求路径去 NSLog / 渲染 pending
   轮询 20ms→4ms; 探针逐段化 (latency.txt 每段 first-sample)。

**最终稳态** (appex 内探针, 请求到达→首样本):
- naturalvoice 23-34ms / onecore 24-25ms / kona 13ms / 永德 ~17ms — 全部 <50ms;
- 方案多段: 首段 22-25ms; 后续段绝对时刻 177-450ms 但均早于其播放轮到点
  (流水线预启动掩盖, 无缝);
- 冷启 616-697ms 每模型每进程一次 (进程常驻: VO 持久连接下 appex 存活
  2h+ 实测, 会话跨句复用, 模型不卸载)。

**SDK 级约束 (无解, 已被流水线掩盖)**: 同引擎两个模型 (方案主/副侧)
交替合成时在共享 libonnxruntime 上串行 — seg1 首样本 ≈ seg0 推理完成时刻。
多引擎 (naturalvoice+onecore) 并发同理 (互测污染教训: 测延迟须单客户端)。

## "晓晓启动慢"根因与安全预热 (2026-10-18 晚)

**逐语音冷启实测** (每语音独立进程): 晓晓 HD 628 / Andrew HD 716 /
Ava HD 667 / Eric 173 / Michelle 217 (ms) — 非"晓晓专属", 是 **HD 模型
家族**的加载+首推理成本 (~650ms); 热状态**每会话私有**, 跨会话/跨引擎
不共享 (同模型新会话照样 608ms, 实测) → 侧路预热无效, 必须走正式会话。

**安全预热** (ms_warm_voice / mtts_warm_last_voice):
- 记忆: 每次 MS 真实合成把 "engine voice" 写 Engines/.lastvoice (去重);
- 触发: appex 内首次 ensureBridge (scan>0, 即合成宿主; 守护进程 scan=0
  天然不触发) 后台线程读 .lastvoice → 建会话 + 一次"嗯。"微型合成,
  launch→读尽→入墓全程**绝不 stop** (ORT 红线); 会话创建加 g_sess_lock
  防双建;
- 让位: 预热入口/真实请求互斥 — stream_start 遇 warming 在 vs->lock
  cond_wait (等待 ≤ 冷启成本, 竞争实测 609ms ≈ 冷启, 永不双冷);
- 效果 (端到端, appex): 进程被其它语音拉起后 2s, 晓晓**首句 24ms**
  (原 628-651ms); 进程与请求同时拉起时 651ms ≈ 等待预热, 不亏。

**边界**: 进程在第一句晓晓时才被系统拉起 → 该句仍 ~650ms (模型必须
加载一次; Windows 在 NVDA 启动时付)。首次拉起另含 appex 进程生成本身
~0.5s (永德首句总 582ms, 首样本 1ms — 拉起成本与引擎无关)。

## HD vs 非HD 模型的每句首字节差异 (2026-10-18 夜, "只有晓晓每句开头慢"定案)

**矩阵实测** (CLI 独立进程, 热会话, 首块=首批音频字节):

| 模型 | 单字/单词 | 多字词句 |
|---|---|---|
| 晓晓 HD / Andrew HD / Ava HD | 11-25ms | **108-190ms (台阶式, 不随长度增)** |
| Eric / Michelle (非HD) | 13ms | **12-23ms (零惩罚)** |

- 与引擎无关 (naturalvoice/onecore 同)、与速率无关 (@100/@220 同)、
  **与调用方式无关**: 同一适配器路径; 裸 SDK 探针交替"带/无 prosody"
  SSML → 116-131ms 无差别 (prosody 排除); 单字 13-25ms 证明链路本身极快;
- 结论: HD (NaturalHD 神经模型) 前端在出首音频前需完成整句前端处理,
  非 HD 逐块直出。**~120ms 为 SDK/模型内在成本, 适配层无法移除**;
  (⚠️ 修正 2026-10-19 逆向, 见 NOTES-win-re-ms-voices.md §3: "适配层无法移除"
  结论存疑 — Windows 侧无此惩罚且 add_path 传 `data/<VoiceId>` 叶子目录,
  `<LCID>.INI` 随之生效: HDVoiceSeqLen=5(未读到 INI 时引擎默认 24)+
  关输出缓冲/节流。适配器已改传叶子目录;
  复测 2026-10-19 A/B 实测 (NOTES-win-re-ms-voices.md §9.3): 父/叶子目录
  两轮热态首音频均 134ms、字节流逐块一致 — 台阶延迟与 add_path 粒度无关,
  原判"内在成本"以 A/B 实证恢复成立。注: 1.48.2 其实含全套 HD INI 键名
  (UTF-16LE 存放, ASCII 搜索搜不到), 但至少 SeqLen/缓冲项不影响首块时机);
- 用户感知: dual2 主侧=晓晓 HD @220 → 每句开头 ~120-190ms (方案里英文
  段多为后续段, 被流水线掩盖故"只有晓晓明显"); 冷启 ~650ms 已由
  .lastvoice 预热治理。极致低延迟场景可配非 HD (Eric/Michelle) 或
  kona/永德。
