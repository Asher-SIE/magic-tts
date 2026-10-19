import AVFoundation
import Foundation

/// Magic TTS 语音合成提供器.
///
/// 生命周期:
///   原版 onSynthesizeText   → synthesizeSpeechRequest
///   原版 audioAvailable     → internalRenderBlock 输出 Float32
///   原版 onStop             → cancelSpeechRequest
///
/// 流式: 请求线程毫秒级返回; **馈送线程** (feeder) 独占全部引擎流的
/// 启动/阻塞读/释放, 逐段拉取并线性换算到输出采样率后推入共享 srcBuf;
/// 渲染块只从 srcBuf 顺序消费, 绝不触碰引擎 (无阻塞 → 无输出断流).
/// MS 引擎首块音频 ~0.8s 到达 (模型首次推理), 之后 ~1.6× 实时供给,
/// feeder 供给快于播放 → 领先量持续累积, 段切换点数据恒就绪.
///
/// 段队列 (双语方案): 一次请求切分为多段, 每段一个引擎流, feeder 逐段
/// 拉尽切换. 相邻段必不同侧 (TextSegmenter 合并同侧相邻段); 读当前段时
/// 预启动下一段 (同 (engine,voice) 不预启动 — MS 会话每 voice 单 wav 文件
/// + 单 active 流, 必须串行, 故音高变体的孤立字母段自然顺延).
/// 拼接缝断层治理 (2026-10-19): 引擎段尾自带 ~130ms+ 静默 (dump 实测
/// kona 135ms/段首 0ms), 双语逐段拼接时每个切换点白停一拍 → feeder 在
/// 段读尽时裁掉 srcBuf 尾部未消费的静默 (trimSeamTail, 按段尾标点分档
/// 保留 50/100/160ms). feeder 领先播放进度, 裁剪时静默尚未播出, 故可整段裁去.
public final class MagicSpeechProviderAU: AVSpeechSynthesisProviderAudioUnit {

    // MARK: - 调试探针 (统一日志对未签名构建不可用 → 写文件)

    private static func probeMarkStatic(_ name: String) {
        let dir = MagicPaths.enginesRoot.appendingPathComponent(".debug", isDirectory: true)
        try? FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        let url = dir.appendingPathComponent("entry_trace.txt")
        let line = "\(Date()) \(name)\n"
        if let old = try? String(contentsOf: url, encoding: .utf8) {
            try? (old + line).write(to: url, atomically: true, encoding: .utf8)
        } else {
            try? line.write(to: url, atomically: true, encoding: .utf8)
        }
    }

    /// 延迟探针: 请求到达 → 引擎首批样本入缓冲 的全程耗时 (逐句, 上限 400 行轮转).
    /// 读法: 首句 ~700ms+ = 冷推理 (每语音每进程一次), 之后 ~130-180ms = MS 热路径.
    private static func probeLatency(_ ms: Double, engine: String, rate: Int32, text: String) {
        probeWrite("first-sample=\(Int(ms))ms engine=\(engine) rate=\(rate) text=\(text)")
    }

    /// 延迟探针: 分段相位 (launch = mtts_stream_start 返回耗时).
    private static func probePhase(_ phase: String, ms: Double, engine: String) {
        probeWrite("\(phase)=\(Int(ms))ms engine=\(engine)")
    }

    private static func probeWrite(_ line: String) {
        let dir = MagicPaths.enginesRoot.appendingPathComponent(".debug", isDirectory: true)
        try? FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        let url = dir.appendingPathComponent("latency.txt")
        let full = "\(Date()) \(line)"
        if let old = try? String(contentsOf: url, encoding: .utf8) {
            var lines = old.split(separator: "\n", omittingEmptySubsequences: true).map(String.init)
            lines.append(full)
            if lines.count > 400 { lines.removeFirst(lines.count - 400) }
            try? lines.joined(separator: "\n").appending("\n").write(
                to: url, atomically: true, encoding: .utf8)
        } else {
            try? full.appending("\n").write(to: url, atomically: true, encoding: .utf8)
        }
    }

    // MARK: - 引擎桥 (C)

    private static let bridgeQueue = DispatchQueue(label: "magictts.bridge.init")
    private static var scanned = false

    /// 轻量扫描 (毫秒级, 同步) — 仅此而已.
    /// ⚠️ AU 构造可能在**语音目录守护进程**内发生 (系统枚举发音人时 in-process 实例化):
    /// 那里读不了 appex 容器、更不能 dlopen 引擎库 (2026-10-17 实测, 中午的 6 连崩即
    /// 源于在守护进程上下文里做重初始化). 引擎预热由 mtts_stream_start 首次合成时懒加载.
    private static func ensureBridge() {
        bridgeQueue.sync {
            guard !scanned else { return }
            let root = MagicPaths.enginesRoot.path
            let ready = mtts_global_scan(root)
            NSLog("[MagicTTS] bridge scan: \(ready) 个引擎物料就绪")
            scanned = true
            /* 后台预热上次使用的语音 (HD 模型冷启 ~650ms 治理):
             * 守护进程上下文 scan=0 → 不触发 (重初始化红线);
             * 合成宿主 → 会话+微型推理; 真实请求到来时等待而非打断. */
            if ready > 0 {
                DispatchQueue.global(qos: .utility).async {
                    mtts_warm_last_voice()
                }
            }
        }
    }

    // MARK: - 段队列与音频缓冲 (stateLock 保护; 请求线程写, feeder 读写, 渲染线程读)

    /// 一段 = 一个引擎流. gain 为渲染级逐样本增益 (方案音量); eq* 为该侧三段 EQ 增益.
    private struct SegPlan {
        let engine: String
        let voice: String?      // nil = 引擎默认发音人
        let text: String
        let rate: Int32
        let pitch: Int32
        let gain: Float
        var eqLow: Int = 0
        var eqMid: Int = 0
        var eqHigh: Int = 0
    }

    private let stateLock = NSLock()
    private var plans: [SegPlan] = []
    private var streams: [OpaquePointer?] = []   // 引擎流 (feeder 启动/读尽/释放)
    private var started: [Bool] = []             // 已尝试启动
    private var rates: [Int32] = []              // 各段引擎采样率
    private var gen: UInt64 = 0                  // 请求代际 (渲染侧据此复位缓冲)
    private var cancelled = true
    private var prefetchDone = false             // feeder 已读尽全部段
    private var dumpAudio = false                // .debug/dump 存在时写调试 WAV
    private var reqStart: DispatchTime? = nil    // 延迟探针: 请求到达时刻 (状态锁保护)

    /// 共享音频缓冲: feeder 产 (已换算到输出采样率), 渲染线程消费.
    private var srcBuf: [Float] = []
    private var srcPos: Int = 0                   // 渲染消费位置 (srcBuf 索引空间)
    private var dbg: [Float] = []                 // 调试转储 (与 srcBuf 尾部对齐)

    // 渲染线程私有 (仅 render block 触碰)
    private var curGen: UInt64 = 0
    private var dumpActive = false

    private let outputBus: AUAudioUnitBus
    private var storedRenderBlock: AUInternalRenderBlock!

    // MARK: - AU 构造

    public override init(componentDescription: AudioComponentDescription, options: AudioComponentInstantiationOptions = []) throws {
        Self.probeMarkStatic("AU-init")
        Self.ensureBridge()

        let format = AVAudioFormat(standardFormatWithSampleRate: 24000, channels: 1)!
        outputBus = try AUAudioUnitBus(format: format)

        try super.init(componentDescription: componentDescription, options: options)

        storedRenderBlock = makeRenderBlock()
    }

    public override var outputBusses: AUAudioUnitBusArray {
        AUAudioUnitBusArray(audioUnit: self, busType: .output, busses: [outputBus])
    }

    deinit {
        Self.probeMarkStatic("AU-deinit")
        for s in streams {
            if let s { mtts_stream_cancel(s); mtts_stream_free(s) }
        }
    }

    // MARK: - Speech Synthesis Provider (核心三件套)

    /// 声明语音: 只注册方案槽位 (scheme.json 出现的键); 发音人不注册 —
    /// 仅作方案素材与 App 内试听.
    /// 调试: Engines/voicemode.txt (0=仅方案[默认] 1=旧3条 2=永德+方案 3=全部) — 注册机制排查用.
    public override var speechVoices: [AVSpeechSynthesisProviderVoice] {
        get {
            var mode = 0
            if let s = try? String(contentsOf: MagicPaths.enginesRoot.appendingPathComponent("voicemode.txt"),
                                    encoding: .utf8),
               let m = Int(s.trimmingCharacters(in: .whitespacesAndNewlines)) {
                mode = m
            }

            var voices: [AVSpeechSynthesisProviderVoice] = []
            if mode == 1 {   // 对照组: 昨天可注册成功的旧三条
                let old: [(String, String)] = [
                    ("永德", "com.magictts.yongde"),
                    ("自然语音", "com.magictts.naturalvoice"),
                    ("OneCore", "com.magictts.onecore"),
                ]
                for (name, id) in old {
                    voices.append(AVSpeechSynthesisProviderVoice(
                        name: name, identifier: id,
                        primaryLanguages: ["zh-CN"], supportedLanguages: ["zh-CN"]))
                }
                return voices
            }
            /* 发音人注册仅调试模式用 (2=仅永德 3=全部); 正常使用只有方案槽位. */
            if mode >= 2 {
                let n = mtts_engine_count()
                for i in 0..<n {
                    guard let d = mtts_engine_at(Int32(i)) else { continue }
                    let desc = d.pointee
                    let engineKey = String(cString: desc.key)
                    let locale = String(cString: desc.locale)
                    if mode == 2 && engineKey != "yongde" { continue }
                    let vc = mtts_voice_count(engineKey)
                    for v in 0..<vc {
                        guard let vd = mtts_voice_at(engineKey, v) else { continue }
                        /* 语言按发音人 (英文包标 en-US 等); 适配器未填时回退引擎级 */
                        let voiceLocale = String(cString: vd.pointee.locale)
                        let lang = voiceLocale.isEmpty ? locale : voiceLocale
                        voices.append(AVSpeechSynthesisProviderVoice(
                            name: String(cString: vd.pointee.name),
                            identifier: String(cString: vd.pointee.identifier),
                            primaryLanguages: [lang],
                            supportedLanguages: [lang]))
                    }
                }
            }
            /* 方案槽位: 只注册已创建的 (scheme.json 出现的键); 名称带方案备注。
             * 增/删后 App 端调 updateSpeechVoices 让目录重查;
             * 槽位→实际发音人映射在请求时读 scheme.json, 改配置即时生效. */
            let all = SchemeStore.loadAll()
            for slot in SchemeStore.slots where all[slot] != nil {
                let label = all[slot]!.label
                let name = (SchemeStore.slotNames[slot] ?? slot)
                    + (label.isEmpty ? "" : " — \(label)")
                voices.append(AVSpeechSynthesisProviderVoice(
                    name: name,
                    identifier: "com.magictts.scheme.\(slot)",
                    primaryLanguages: ["zh-CN"],
                    supportedLanguages: ["zh-CN", "en-US"]))
            }
            /* 静态兜底: 语音目录枚举在系统守护进程内 in-process 实例化 AU,
             * 那里读不了 appex 容器 → 扫描必为空。没有兜底时 getter 在枚举上下文
             * 永远返回空表 = 发音人从不注册 (2026-10-17 回归根因)。
             * 合成端对未知 voiceKey 自动回退默认包, 故兜底 id 可用固定键。 */
            if voices.isEmpty {
                voices = Self.staticVoices
            }
            /* 探针: 系统查询语音列表时留痕 (统一日志对未签名构建不可用) */
            Self.probeMarkStatic("speechVoices-get")
            let mark = MagicPaths.enginesRoot.appendingPathComponent(".debug/voices_queried.txt")
            let report = "\(Date()) mode=\(mode) 上报\(voices.count)条: "
                + voices.map(\.identifier).joined(separator: ", ") + "\n"
            try? report.write(to: mark, atomically: true, encoding: .utf8)
            return voices
        }
        set {}   // ObjC 属性可仅覆写 getter; 系统不写此属性
    }

    /// 静态发音人表 = 方案槽位前两条. 语音目录枚举在系统守护进程内 in-process
    /// 实例化 AU, 那里读不了 appex 容器 → 扫描必为空。没有兜底时 getter 在枚举
    /// 上下文永远返回空表 = 槽位从不注册 (2026-10-17 回归根因)。
    /// 发音人不注册, 兜底也仅含槽位; 合成端请求时读 scheme.json 映射实际发音人.
    private static let staticVoices: [AVSpeechSynthesisProviderVoice] =
        SchemeStore.slots.prefix(2).map {
            AVSpeechSynthesisProviderVoice(
                name: SchemeStore.slotNames[$0] ?? $0,
                identifier: "com.magictts.scheme.\($0)",
                primaryLanguages: ["zh-CN"],
                supportedLanguages: ["zh-CN", "en-US"])
        }

    /// 合成请求 — 系统送来 SSML + voice. 方案语音切段分发, 普通语音单段直通.
    public override func synthesizeSpeechRequest(_ request: AVSpeechSynthesisProviderRequest) {
        /* 临时诊断: 原始 SSML 入档 (大小写/导航请求行为排查, 稳定后移除) */
        Self.probeWrite("ssml=\(request.ssmlRepresentation.prefix(300))")
        let parsed = SpeechRequestParser.parse(request.ssmlRepresentation)
        let (engineKey, voiceKey) = Self.parseVoiceId(request.voice.identifier)

        var newPlans: [SegPlan] = []
        if engineKey == "scheme", let slot = voiceKey, SchemeStore.slotNames[slot] != nil {
            let scheme = SchemeStore.loadAll()[slot] ?? SchemeStore.fallback()
            let p = SchemeStore.resolve(scheme.primary)
            let s = scheme.useSecondary ? SchemeStore.resolve(scheme.secondary) : p
            /* 方案语音: 语速/音高/音量全部 MagicTTS 托管 (scheme.json),
             * 忽略请求里的 prosody — VO 面板对方案本就无效, 不做跟随. */
            /* 朗读替换规则: 方案侧按 useRules 开关, 分段前作用于整句 */
            let text = scheme.useRules ? RuleStore.shared().apply(parsed.text) : parsed.text
            var segs = TextSegmenter.segment(text, options: scheme.rules)
            /* 单语音模式: 全部段归主侧后重并 (音高变体的孤立字母段仍独立) */
            if !scheme.useSecondary {
                segs = TextSegmenter.coalesce(segs.map {
                    TextSegment(text: $0.text, target: .primary, isolatedCapital: $0.isolatedCapital)
                })
            }
            newPlans = segs.map { seg in
                let isPrimary = !scheme.useSecondary || seg.target == .primary
                let side = isPrimary ? scheme.primary : scheme.secondary
                let r = isPrimary ? p : s
                var pitch = side.pitch
                if seg.isolatedCapital { pitch = min(100, pitch + LetterCaseStyle.pitchDelta) }
                return SegPlan(engine: r.engine, voice: r.voice, text: seg.text,
                               rate: Int32(side.rate), pitch: Int32(pitch),
                               gain: Float(side.volume) / 100.0,
                               eqLow: side.eqLow, eqMid: side.eqMid, eqHigh: side.eqHigh)
            }
            NSLog("[MagicTTS] scheme=\(slot) 主=\(p.engine) 副=\(s.engine) 二语音=\(scheme.useSecondary) → \(newPlans.count) 段 text=\(text.prefix(32))")
        } else {
            /* 普通引擎语音: 全局规则直接应用 (规则本身可逐条停用) */
            let text = TextSegmenter.splitIdentifiers(RuleStore.shared().apply(parsed.text))
            newPlans = [SegPlan(engine: engineKey, voice: voiceKey, text: text,
                                rate: Int32(parsed.rate), pitch: Int32(parsed.pitch), gain: 1.0)]
        }

        // 旧队列退休: 仅 cancel 解除阻塞读 (在途流的释放由其 feeder 收尾);
        // 随后整组换新, 音频缓冲复位
        stateLock.lock()
        for i in streams.indices {
            if let s = streams[i] { mtts_stream_cancel(s) }
        }
        plans = []; streams = []; started = []; rates = []
        srcBuf.removeAll(keepingCapacity: true)
        srcPos = 0
        dbg.removeAll(keepingCapacity: true)
        gen &+= 1
        cancelled = newPlans.isEmpty     // 空文本 → 渲染立即 Complete
        prefetchDone = newPlans.isEmpty
        reqStart = newPlans.isEmpty ? nil : .now()   // 延迟探针起点
        dumpAudio = FileManager.default.fileExists(
            atPath: MagicPaths.enginesRoot.appendingPathComponent(".debug/dump").path)
        if !newPlans.isEmpty {
            plans = newPlans
            streams = [OpaquePointer?](repeating: nil, count: plans.count)
            started = [Bool](repeating: false, count: plans.count)
            rates = [Int32](repeating: 24000, count: plans.count)
        }
        let g = gen
        stateLock.unlock()

        guard !newPlans.isEmpty else { return }
        spawnFeeder(g)
    }

    /// identifier → (engineKey, voiceKey?).
    /// 系统传来的 identifier 带 appex bundle 前缀
    /// (com.magictts.voices.synth.com.magictts.<engine>.<voicekey>),
    /// 故从最后一个 "com.magictts." 起解析; 无第四段 → voiceKey=nil (默认/旧版兼容).
    static func parseVoiceId(_ identifier: String) -> (String, String?) {
        guard let r = identifier.range(of: "com.magictts.", options: .backwards) else {
            return ("yongde", nil)
        }
        let rest = identifier[r.upperBound...]
        let parts = rest.split(separator: ".", omittingEmptySubsequences: false)
        guard let engine = parts.first else { return ("yongde", nil) }
        let voice = parts.count > 1 ? parts[1...].joined(separator: ".") : nil
        return (String(engine), voice)
    }

    /// 停止: 取消全部段 (feeder 见状释放自身流后退出).
    public override func cancelSpeechRequest() {
        NSLog("[MagicTTS] cancel")
        Self.probeMarkStatic("cancel-request")
        stateLock.lock()
        cancelled = true
        for i in streams.indices {
            if let s = streams[i] { mtts_stream_cancel(s) }
        }
        stateLock.unlock()
    }

    // MARK: - 馈送线程 (独占引擎流的启动/读取/释放)

    /// 拼接缝目标停顿 (ms) — 段尾静默裁剪的保留量, 按段尾标点分档, 听感可调.
    private static let seamKeepSentence: Double = 160   // 。！？! ? …
    private static let seamKeepClause: Double = 100     // ，、；：, ; :
    private static let seamKeepDefault: Double = 50     // 其余 (词内/无标点切换)
    private static let seamSilenceThreshold: Float = 0.01   // |x| < ~-40dB 视为静默

    /// 拼接缝收口 (feeder, 段读尽时): 裁掉 srcBuf 尾部未消费的静默, 只留目标停顿.
    /// 引擎段尾自带 ~130ms+ 静默 (2026-10-19 dump 实测 kona 135ms/段首 0ms),
    /// 双语逐段拼接时每个切换点白停一拍 = 断层感主体. feeder 领先播放进度,
    /// 静默尚未播出即可裁去; 下一段段首静默不动 (永德的句读停顿在段首,
    /// 裁它会吃掉标点停顿). dbg 转储同步裁剪以反映实际播放时间线.
    /// 调用方持 stateLock.
    private func trimSeamTailLocked(_ segEnd: Character?) {
        let keepMs: Double
        if let c = segEnd, "。！？!?…".contains(c) {
            keepMs = Self.seamKeepSentence
        } else if let c = segEnd, "，、；：,;:".contains(c) {
            keepMs = Self.seamKeepClause
        } else {
            keepMs = Self.seamKeepDefault
        }
        let keep = Int(keepMs / 1000.0 * self.outputBus.format.sampleRate)
        var sil = 0
        while sil < self.srcBuf.count
            && abs(self.srcBuf[self.srcBuf.count - 1 - sil]) < Self.seamSilenceThreshold {
            sil += 1
        }
        let trim = min(sil, self.srcBuf.count - self.srcPos) - keep
        if trim > 0 {
            self.srcBuf.removeLast(trim)
            if self.dumpAudio { self.dbg.removeLast(min(trim, self.dbg.count)) }
        }
    }

    /// 启动一段流 (feeder 线程, 阻塞至 mtts_stream_start 返回).
    /// 返回流指针; 代际已顶替或启动失败 → nil (失败仅记日志, 调用方跳段).
    private func feederLaunch(_ idx: Int, plan: SegPlan, requestGen: UInt64) -> OpaquePointer? {
        let probeT0: DispatchTime? = {
            stateLock.lock()
            defer { stateLock.unlock() }
            return reqStart
        }()
        var sr: Int32 = 24000
        let t0 = DispatchTime.now()
        let st = mtts_stream_start(plan.engine, plan.voice, plan.text,
                                   plan.rate, plan.pitch, &sr)
        if idx == 0, let t0p = probeT0 {
            let ms = Double(DispatchTime.now().uptimeNanoseconds - t0p.uptimeNanoseconds) / 1_000_000
            Self.probePhase("launch", ms: ms, engine: plan.engine)
        } else {
            let ms = Double(DispatchTime.now().uptimeNanoseconds - t0.uptimeNanoseconds) / 1_000_000
            if ms > 100 { NSLog("[MagicTTS] 第 \(idx) 段启动慢 \(Int(ms))ms") }
        }
        stateLock.lock()
        defer { stateLock.unlock() }
        guard gen == requestGen else {
            // 启动期间请求被顶替: 该流不属于当前代际, 自行释放
            if let st { mtts_stream_cancel(st); mtts_stream_free(st) }
            return nil
        }
        if st == nil {
            started[idx] = true
            NSLog("[MagicTTS] 第 \(idx) 段启动失败 (engine=\(plan.engine) voice=\(plan.voice ?? "-"))")
        } else {
            streams[idx] = st
            rates[idx] = sr
            started[idx] = true
        }
        return st
    }

    /// 馈送线程主体: 按段序 启动 → 读尽 → 裁缝 → 下一段.
    /// 读当前段前预启动下一段 (同 (engine,voice) 不预启动 — MS 每 voice 单会话
    /// 必须串行, 音高变体的孤立字母段自然顺延到前任读尽后).
    /// 强持有 self: 流的释放只发生在本线程 (与 cancel 的跨线程 cancel 错开),
    /// 退出前清完 myStreams, deinit 才可能到来.
    private func spawnFeeder(_ requestGen: UInt64) {
        DispatchQueue.global(qos: .userInitiated).async {
            let outRate = self.outputBus.format.sampleRate
            var myStreams: [OpaquePointer] = []   // 本 feeder 启动的流 (退出前统一释放)
            var scratch = [Int16](repeating: 0, count: 4096)
            var idx = 0
            // 段内线性重采样状态 (引擎采样率 → 输出采样率)
            var segPos = 0.0        // 段内已消费位置 (引擎样本空间, 累计)
            var segBase = 0         // 当前 chunk 在段内的起始样本序
            var prevTail: Float = 0 // 上一 chunk 末样本 (跨 chunk 插值用)
            var eqFilter = Eq3Band(sampleRate: outRate, low: 0, mid: 0, high: 0) // 当前段三段 EQ (段切换时重建)
            var eqIdx = -1         // eqFilter 对应的段序
            var probeIdx = -1       // 延迟探针: 已记录到第几段

            // 释放本 feeder 启动过且尚未读尽释放的流, 并清掉 streams[] 里的
            // 悬垂指针 (代际未变时; 已换新则数组整体替换过, 无需清)
            func cleanup() {
                for s in myStreams { mtts_stream_cancel(s); mtts_stream_free(s) }
                self.stateLock.lock()
                if self.gen == requestGen {
                    for i in self.streams.indices {
                        if let s = self.streams[i], myStreams.contains(s) { self.streams[i] = nil }
                    }
                }
                self.stateLock.unlock()
            }

            while true {
                // 代际/取消 快照 + 当前段参数
                self.stateLock.lock()
                let g = self.gen
                let isCancelled = self.cancelled
                let planCount = self.plans.count
                var plan = SegPlan(engine: "yongde", voice: nil, text: "",
                                   rate: 100, pitch: 50, gain: 1)
                var cur: OpaquePointer?
                var attempted = true
                if idx < planCount {
                    plan = self.plans[idx]
                    cur = self.streams[idx]
                    attempted = self.started[idx]
                }
                let probeT0 = self.reqStart
                self.stateLock.unlock()
                if isCancelled || g != requestGen { cleanup(); return }
                guard idx < planCount else {
                    self.stateLock.lock()
                    if self.gen == requestGen { self.prefetchDone = true }
                    self.stateLock.unlock()
                    return
                }

                // 确保当前段已启动
                if cur == nil {
                    if attempted {   // 启动失败 → 跳段
                        idx += 1; segPos = 0; segBase = 0; prevTail = 0
                        continue
                    }
                    guard let st = self.feederLaunch(idx, plan: plan, requestGen: requestGen) else {
                        continue   // 失败已记; 顶替已清理
                    }
                    myStreams.append(st)
                    continue
                }

                // 预启动下一段 (差异 voice 才行 — 同 voice 须等本段读尽)
                if idx + 1 < planCount {
                    self.stateLock.lock()
                    let nextStarted = idx + 1 < self.started.count && self.started[idx + 1]
                    let nextPlan = self.plans[idx + 1]
                    let nextCur = idx + 1 < self.streams.count ? self.streams[idx + 1] : nil
                    self.stateLock.unlock()
                    if !nextStarted && nextCur == nil
                        && !(nextPlan.engine == plan.engine && nextPlan.voice == plan.voice),
                       let st = self.feederLaunch(idx + 1, plan: nextPlan, requestGen: requestGen) {
                        myStreams.append(st)
                    }
                }

                // 阻塞读一块 (MS 按数据到达节律; 内存引擎立即返回)
                var done: Int32 = 0
                let got = mtts_stream_read(cur!, &scratch, Int32(scratch.count), &done)
                if got > 0 {
                    let n = Int(got)
                    self.stateLock.lock()
                    if self.gen == requestGen {
                        if idx > probeIdx, let t0 = probeT0 {
                            probeIdx = idx
                            let ms = Double(DispatchTime.now().uptimeNanoseconds - t0.uptimeNanoseconds) / 1_000_000
                            Self.probeLatency(ms, engine: plan.engine, rate: plan.rate,
                                              text: String(plan.text.prefix(16)))
                        }
                        // 线性换算到输出采样率后追加 (含增益与三段 EQ)
                        let bufBefore = self.srcBuf.count
                        let step = Double(self.rates.indices.contains(idx) ? self.rates[idx] : 24000) / outRate
                        if eqIdx != idx {
                            eqFilter = Eq3Band(sampleRate: outRate, low: plan.eqLow,
                                               mid: plan.eqMid, high: plan.eqHigh)
                            eqIdx = idx
                        }
                        while segPos + 1 < Double(segBase + n) {
                            let i0 = Int(segPos) - segBase
                            let s0: Float = i0 < 0 ? prevTail : Float(scratch[i0]) / 32768.0 * plan.gain
                            let s1: Float = Float(scratch[i0 + 1]) / 32768.0 * plan.gain
                            let frac = Float(segPos - Double(Int(segPos)))
                            self.srcBuf.append(eqFilter.process(s0 * (1 - frac) + s1 * frac))
                            segPos += step
                        }
                        if self.dumpAudio {
                            self.dbg.append(contentsOf: self.srcBuf.suffix(self.srcBuf.count - bufBefore))
                        }
                    }
                    self.stateLock.unlock()
                    segBase += n
                    prevTail = Float(scratch[n - 1]) / 32768.0 * plan.gain
                }

                if done != 0 || got == 0 {
                    // 段读尽: 摘除 (代际未变时) + 裁缝 + 释放 + 推进
                    self.stateLock.lock()
                    let mine = self.gen == requestGen && self.streams.indices.contains(idx)
                        && self.streams[idx] == cur
                    if mine {
                        self.streams[idx] = nil
                        self.trimSeamTailLocked(plan.text.last)
                    }
                    self.stateLock.unlock()
                    mtts_stream_free(cur!)
                    myStreams.removeAll { $0 == cur }
                    idx += 1; segPos = 0; segBase = 0; prevTail = 0
                }
            }
        }
    }

    // MARK: - 渲染 (只消费 srcBuf; 饥饿有界等待, 末段尽置 Complete)

    private func makeRenderBlock() -> AUInternalRenderBlock {
        { [weak self] actionFlags, _, frameCount, _, audioBufferList, _, _ in
            guard let self else { return kAudioUnitErr_FailedInitialization }

            let abl = UnsafeMutableAudioBufferListPointer(audioBufferList)
            guard let out = abl[0].mData?.assumingMemoryBound(to: Float.self) else {
                return kAudioUnitErr_InvalidParameter
            }
            abl[0].mDataByteSize = UInt32(Int(frameCount) * MemoryLayout<Float>.size)

            self.stateLock.lock()
            let g = self.gen
            var isCancelled = self.cancelled
            var allDone = self.prefetchDone
            let dump = self.dumpAudio
            self.stateLock.unlock()

            guard !isCancelled else {
                for i in 0..<Int(frameCount) { out[i] = 0 }
                actionFlags.pointee.insert(.offlineUnitRenderAction_Complete)
                return noErr
            }

            // 新请求 → 复位渲染侧状态 (srcBuf 已由请求线程复位)
            if g != self.curGen {
                self.curGen = g
                self.dumpActive = dump
            }

            /* 饥饿有界等待: feeder 尚未供数 (MS 首块/冷推理) → 4ms 粒度等;
             * prefetchDone 后不再等 (剩余缓冲播完即 Complete) */
            var waitedMs = 0
            var starved = false
            var dumpCopy: [Float] = []
            repeat {
                self.stateLock.lock()
                starved = self.srcPos >= self.srcBuf.count
                allDone = self.prefetchDone
                isCancelled = self.cancelled
                self.stateLock.unlock()
                if isCancelled || allDone || !starved || g != self.gen { break }
                usleep(4000)
                waitedMs += 4
            } while waitedMs < 15_000

            // 顺序消费 + 已消费区定期回收 (锁内; 逐样本直拷, 无重采样)
            self.stateLock.lock()
            var hitEnd = false
            if g == self.gen {
                let count = self.srcBuf.count
                for i in 0..<Int(frameCount) {
                    if self.srcPos < count {
                        out[i] = self.srcBuf[self.srcPos]
                        self.srcPos += 1
                    } else {
                        out[i] = 0
                        if allDone { hitEnd = true }
                    }
                }
                if self.srcPos > 65536 {
                    self.srcBuf.removeFirst(self.srcPos)
                    self.srcPos = 0
                }
            } else {
                for i in 0..<Int(frameCount) { out[i] = 0 }
            }
            if hitEnd && self.dumpActive { dumpCopy = self.dbg }
            self.stateLock.unlock()

            if hitEnd {
                actionFlags.pointee.insert(.offlineUnitRenderAction_Complete)
                if self.dumpActive {
                    self.writeDebugWav(dumpCopy, Int(self.outputBus.format.sampleRate))
                }
            }
            return noErr
        }
    }

    /// 调试转储 (默认关闭; 触碰 <容器>/Engines/.debug/dump 开启):
    /// 渲染结束时把实际播放时间线写为 .debug/last_<n>.wav (轮转 8 个).
    private func writeDebugWav(_ pcm: [Float], _ sr: Int) {
        guard pcm.count > 0 else { return }
        let dir = MagicPaths.enginesRoot.appendingPathComponent(".debug", isDirectory: true)
        try? FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        let stamp = Int(Date().timeIntervalSince1970) % 100000
        let url = dir.appendingPathComponent("last_\(stamp).wav")

        func le32(_ v: Int32) -> Data { withUnsafeBytes(of: v.littleEndian) { Data($0) } }
        func le16(_ v: Int16) -> Data { withUnsafeBytes(of: v.littleEndian) { Data($0) } }

        var samples = pcm.map { Int16(max(-32768.0, min(32767.0, Double($0) * 32767.0))) }
        let dataLen = Int32(samples.count * 2)
        var h = Data()
        h.append(contentsOf: [UInt8]("RIFF".utf8)); h.append(le32(36 + dataLen))
        h.append(contentsOf: [UInt8]("WAVE".utf8))
        h.append(contentsOf: [UInt8]("fmt ".utf8)); h.append(le32(16))
        h.append(le16(1)); h.append(le16(1))                      // PCM, 单声道
        h.append(le32(Int32(sr))); h.append(le32(Int32(sr * 2)))  // 采样率, 字节率
        h.append(le16(2)); h.append(le16(16))                     // 块对齐, 位深
        h.append(contentsOf: [UInt8]("data".utf8)); h.append(le32(dataLen))
        samples.withUnsafeBytes { h.append(contentsOf: $0) }
        try? h.write(to: url)

        // 轮转: 只留最近 8 个
        if let files = try? FileManager.default.contentsOfDirectory(
            at: dir, includingPropertiesForKeys: [URLResourceKey.contentModificationDateKey]) {
            func mtime(_ u: URL) -> Date {
                let rv = try? u.resourceValues(forKeys: [.contentModificationDateKey])
                return rv?.contentModificationDate ?? .distantPast
            }
            let wavs = files.filter { $0.lastPathComponent.hasPrefix("last_") }
                .sorted { mtime($0) > mtime($1) }
            for old in wavs.dropFirst(8) { try? FileManager.default.removeItem(at: old) }
        }
    }

    public override var internalRenderBlock: AUInternalRenderBlock { storedRenderBlock }
}
