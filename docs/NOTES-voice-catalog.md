# 系统语音注册机制实测笔记 (macOS 26.7 / 2026-10-17)

> 背景: MagicTTS 曾出现"App 内试听正常、系统目录无语音、say 输出 0.01s 空音频"
> 的双重失效, 前一会话误诊为"注册管道僵死"。本笔记是一次彻底复盘后确立的
> 机制认知与代码不变量, 供此后任何注册/合成排障直接引用, 不必重新调查。

## 一、目录管道的真实形态 (全部实测)

1. **枚举在守护进程上下文 in-process 实例化 AU**。系统查询发音人时不经 appex
   进程, 而是加载扩展二进制到守护进程里跑 `speechVoices` getter。该上下文:
   - 读不了 appex 私有容器 (`~/Library/Containers/<id>/...`) → getter 依赖的
     任何文件扫描在那里必为空;
   - 绝不能 dlopen 引擎库 (2026-10-17 中午 MagicTTSSynth 6 连崩即源于 AU
     构造期在守护进程里做重初始化, 崩在 `EXExtensionContext` 初始化)。
2. **合成的 AU 托管在 appex 进程** (sandboxSafe=true 的 AUv3 一律进程外),
   每次请求按需拉起、结束即退 (ps 里 `-AppleLanguages` 参数的实例)。
3. **目录按 AU 组件码 (type, subtype, manufacturer) 缓存 provider**。首次枚举
   成功时组件码与当时的 appex bundle id 绑定, 之后 voice id 前缀沿用该 id。
   身份实验轮换后复用旧组件码 → 目录持续用废弃前缀 (现用 `mtT5`)。
4. **语音列表对"未变化的 app"不重查**。`updateSpeechVoices()` 只对二进制/
   签名已变的扩展生效。可靠重扫触发: 重装 app (`rm -rf` + `cp -R` + `open`)、
   卸载/安装任一带语音扩展的 app、重启。macOS 26 无 `speechvoiced` 进程;
   `com.apple.speech.voice.prefs` 也是空的; `speechsynthesisd` 归 user 域且
   受 SIP 保护不能 kickstart。
5. **沙盒内外 home 含义不同**: appex 内 `homeDirectoryForCurrentUser` = 容器
   Data 本身; 宿主/CLI 内才是真实家目录。同一条相对路径在两侧指向不同物理
   位置 (appex 侧会得到容器内嵌套路径)。

## 二、两条代码不变量 (违反即复现本次事故)

1. `speechVoices` getter 必须在**零文件可用**时仍返回非空表 (静态兜底,
   `MagicSpeechProviderAU.staticVoices`)。合成端对未知 voiceKey 自动回退
   默认包, 故兜底 id 可用固定键。
2. 引擎路径必须按进程上下文解析 (`MagicPaths.appexContainerData` 按
   `Bundle.main.bundleIdentifier` 分流)。AU 构造期只做轻量扫描, 引擎预热
   由 `mtts_stream_start` 首次合成懒加载。

## 三、排障手册 (症状 → 定位)

探针目录: `~/Library/Containers/com.magictts.voices.synth/Data/Engines/.debug/`
(`entry_trace.txt` = beginRequest/createAudioUnit/AU-init 留痕,
`voices_queried.txt` = 每次 speechVoices 上报的完整清单, `last_*.wav` =
每次合成的引擎侧输出)。⚠️ 本机 shell 无 Full Disk Access 且用户非 admin,
`log show/collect` 全不可用——一律靠文件探针。

| 症状 | 定位步骤 |
|---|---|
| 系统目录无语音 | `swift scripts/check_system_voices.swift`: AU 组件在、语音无 → getter 在枚举上下文返回空, 查 `voices_queried.txt` 是否有"上报0条"; 组件都不在 → 重装 app 触发重扫 |
| say 0.01s 空音频 | appex 侧合成失败: 查 `entry_trace` 是否有 AU-init、有无新 `last_*.wav`; 再用 CLI 直测 (`MT_ENGINES=<容器Engines> /tmp/mtts_cli <engine> "测试"`) 区分数据/桥问题 |
| say 挂起 | 残留 appex 进程: `pkill -f MagicTTS; pkill -f MagicTTSSynth` |
| voice id 前缀不对 | 目录组件码缓存了旧绑定: 换新 subtype 重键 (改 `Extension/Info.plist` + 重建重装) |
| 改了 Bridge 没生效 | 源码新 ≠ 安装新: 必须重跑 `build-clt.sh` 并替换 `~/Applications` 里的 app |

判决性实验 (供复用): 把已知正常的旧语音 app 装回启动——若它立即注册, 说明
目录管道健康, 问题必在自己的 getter/构建; 2026-10-17 即以此排除"系统僵死"。

## 四、事故档案 (简)

- 7b412ac 将 speechVoices 改为扫描驱动 + 引擎数据迁入 appex 自有容器 →
  枚举侧 (守护进程读不到容器) 上报 0 条 = 不注册; appex 侧 (嵌套路径 bug)
  找不到引擎 = say 空音频; 宿主 App 无沙盒路径正确 = 试听正常, 构成完美误导。
- 排障弯路: 探针实际写进了嵌套路径 `.../Data/Library/Containers/<id>/Data/...`,
  真路径无文件被误读为"系统从未查询我们"。教训: 探针没出现 ≠ 代码没运行,
  先确认探针路径在当前上下文到底解析到哪。
- 修复: 静态兜底 + AU 构造去重初始化 (72c1af9 之前) + MagicPaths 上下文
  分流 + subtype 升级 mtT5 重键 (72c1af9)。
