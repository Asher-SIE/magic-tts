import AVFoundation
import Foundation

/// Magic TTS 语音合成提供器.
///
/// 生命周期:
///   原版 onSynthesizeText   → synthesizeSpeechRequest
///   原版 audioAvailable     → internalRenderBlock 输出 Float32
///   原版 onStop             → cancelSpeechRequest
///
/// 流式: 请求线程 start (毫秒级返回), 渲染块按帧从 mtts_stream 拉取重采样.
/// MS 引擎首块音频 ~0.8s 到达 (模型首次推理), 之后 ~1.6× 实时供给.
///
/// 段队列 (双语方案): 一次请求切分为多段, 每段一个引擎流, 逐段拉尽切换;
/// 相邻段必不同侧 (TextSegmenter 合并同侧相邻段), 故 MS 同一 voice 会话
/// 永远不会出现并发流 (ms 适配器要求每 voice 串行, launch 前 stop+复位拍).
/// 启动线程领先渲染位置 1 段流水线预热, 消除段间切换静默.
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
        }
    }

    // MARK: - 段队列状态 (stateLock 保护; 渲染线程读, 请求/启动线程写)

    /// 一段 = 一个引擎流. gain 为渲染级逐样本增益 (方案音量).
    private struct SegPlan {
        let engine: String
        let voice: String?      // nil = 引擎默认发音人
        let text: String
        let rate: Int32
        let pitch: Int32
        let gain: Float
    }

    private let stateLock = NSLock()
    private var plans: [SegPlan] = []
    private var streams: [OpaquePointer?] = []   // 已启动的流 (nil = 未启动或失败)
    private var started: [Bool] = []             // 已尝试启动
    private var rates: [Int32] = []              // 各段引擎采样率
    private var nextToStart = 0                  // 启动线程游标
    private var currentIdx = 0                   // 渲染游标
    private var retired: [OpaquePointer] = []    // 已取消/读尽待释放 (渲染线程释放)
    private var gen: UInt64 = 0                  // 请求代际 (渲染侧据此复位缓冲)
    private var pending = false                  // 请求已接受, 第 0 段未启动完
    private var cancelled = true
    private var dumpAudio = false                // .debug/dump 存在时写调试 WAV

    private let starterQueue = DispatchQueue(label: "magictts.segstart")

    // 渲染线程私有 (仅 render block 触碰)
    private var srcBuf: [Float] = []        // 引擎采样率 Float32 累积缓冲
    private var srcPos: Double = 0          // 消费位置 (srcBuf 索引空间, 小数步进)
    private var curGen: UInt64 = 0
    private var curRate: Double = 24000
    private var dumpActive = false
    private var dbg: [Float] = []
    private var dbgRate: Int = 24000

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
        for s in streams {
            if let s { mtts_stream_cancel(s); mtts_stream_free(s) }
        }
        for s in retired { mtts_stream_free(s) }
    }

    // MARK: - Speech Synthesis Provider (核心三件套)

    /// 声明语音: 每个引擎的每个发音人一条 + 双语方案槽位.
    /// 调试: Engines/voicemode.txt (1=旧3条 2=仅永德 3=全部, 默认3) — 注册机制排查用.
    public override var speechVoices: [AVSpeechSynthesisProviderVoice] {
        get {
            var mode = 3
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
            /* 双语方案槽位: 固定注册 (目录对未变化的 app 不重查);
             * 槽位→实际发音人映射在请求时读 scheme.json, 改配置即时生效. */
            if mode >= 2 {
                for slot in SchemeStore.slots {
                    voices.append(AVSpeechSynthesisProviderVoice(
                        name: SchemeStore.slotNames[slot] ?? slot,
                        identifier: "com.magictts.scheme.\(slot)",
                        primaryLanguages: ["zh-CN"],
                        supportedLanguages: ["zh-CN", "en-US"]))
                }
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

    /// 静态发音人表 = 标准物料布局的固定集合 (永德 5 音色 + MS 每引擎默认 1 发音人
    /// + 双语方案槽位). 永德键与 .vl 文件名一致; MS 用无键 id (解析得 voiceKey=nil
    /// → 默认包), 故无论实际装了哪些包, 兜底 id 均可被合成端解析.
    private static let staticVoices: [AVSpeechSynthesisProviderVoice] = {
        func v(_ name: String, _ id: String) -> AVSpeechSynthesisProviderVoice {
            AVSpeechSynthesisProviderVoice(
                name: name, identifier: id,
                primaryLanguages: ["zh-CN"], supportedLanguages: ["zh-CN"])
        }
        var list = (0...4).map {
            v("永德·语音\($0)", "com.magictts.yongde.ydvoice0\($0)")
        }
        list.append(v("自然语音·晓晓 HD", "com.magictts.naturalvoice"))
        list.append(v("OneCore·晓晓 HD", "com.magictts.onecore"))
        /* Kona (系统 Eloquence): zh/en 各 8 发音人 — 系统组件恒在,
         * 守护进程上下文枚举不了系统发音人, 也用静态表兜底。 */
        let konaNames = ["Eddy", "Flo", "Grandma", "Grandpa", "Reed", "Rocko", "Sandy", "Shelley"]
        let konaZh = ["Eddy", "Flo", "奶奶", "爷爷", "Reed", "Rocko", "Sandy", "Shelley"]
        for (i, n) in konaNames.enumerated() {
            list.append(v("\(konaZh[i])·中文 (Kona)", "com.magictts.kona.\(n.lowercased())-zh"))
        }
        for n in konaNames {
            list.append(AVSpeechSynthesisProviderVoice(
                name: "\(n)·English (Kona)", identifier: "com.magictts.kona.\(n.lowercased())-en",
                primaryLanguages: ["en-US"], supportedLanguages: ["en-US"]))
        }
        for slot in SchemeStore.slots {
            list.append(AVSpeechSynthesisProviderVoice(
                name: SchemeStore.slotNames[slot] ?? slot,
                identifier: "com.magictts.scheme.\(slot)",
                primaryLanguages: ["zh-CN"],
                supportedLanguages: ["zh-CN", "en-US"]))
        }
        return list
    }()

    /// 合成请求 — 系统送来 SSML + voice. 方案语音切段分发, 普通语音单段直通.
    public override func synthesizeSpeechRequest(_ request: AVSpeechSynthesisProviderRequest) {
        let parsed = SpeechRequestParser.parse(request.ssmlRepresentation)
        let (engineKey, voiceKey) = Self.parseVoiceId(request.voice.identifier)

        var newPlans: [SegPlan] = []
        if engineKey == "scheme", let slot = voiceKey, SchemeStore.slotNames[slot] != nil {
            let scheme = SchemeStore.loadAll()[slot] ?? SchemeStore.defaults()[slot]!
            let p = SchemeStore.resolve(scheme.primary)
            let s = SchemeStore.resolve(scheme.secondary)
            /* 方案语音: 语速/音高/音量全部 MagicTTS 托管 (scheme.json),
             * 忽略请求里的 prosody — VO 面板对方案本就无效, 不做跟随. */
            newPlans = TextSegmenter.segment(parsed.text, options: scheme.rules).map { seg in
                let isPrimary = seg.target == .primary
                let side = isPrimary ? scheme.primary : scheme.secondary
                let r = isPrimary ? p : s
                return SegPlan(engine: r.engine, voice: r.voice, text: seg.text,
                               rate: Int32(side.rate), pitch: Int32(side.pitch),
                               gain: Float(side.volume) / 100.0)
            }
            NSLog("[MagicTTS] scheme=\(slot) 主=\(p.engine) 副=\(s.engine) → \(newPlans.count) 段 text=\(parsed.text.prefix(32))")
        } else {
            newPlans = [SegPlan(engine: engineKey, voice: voiceKey, text: parsed.text,
                                rate: Int32(parsed.rate), pitch: Int32(parsed.pitch), gain: 1.0)]
            NSLog("[MagicTTS] synthesize engine=\(engineKey) voice=\(voiceKey ?? "(默认)") rate=\(parsed.rate) text=\(parsed.text.prefix(32))")
        }

        // 旧队列退休 (cancel 立即; 释放留给渲染线程, 避免与在途 read 竞争)
        stateLock.lock()
        for i in streams.indices {
            if let s = streams[i] { mtts_stream_cancel(s); retired.append(s); streams[i] = nil }
        }
        plans = []; streams = []; started = []; rates = []
        nextToStart = 0; currentIdx = 0
        gen &+= 1
        cancelled = newPlans.isEmpty     // 空文本 → 渲染立即 Complete
        pending = !newPlans.isEmpty
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

        // 同步启动第 0 段 (pending 就此解除; MS 会话含 stop+复位拍 ~150ms)
        let idx0 = popLaunchIndex()
        if idx0 >= 0 {
            launchSegment(idx0, requestGen: g)
        }
        stateLock.lock(); pending = false; stateLock.unlock()
        scheduleStarter()   // 后续段流水线启动 (领先渲染位置 1 段)
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

    /// 停止: 取消全部段.
    public override func cancelSpeechRequest() {
        NSLog("[MagicTTS] cancel")
        stateLock.lock()
        cancelled = true
        pending = false
        for i in streams.indices {
            if let s = streams[i] { mtts_stream_cancel(s); retired.append(s); streams[i] = nil }
        }
        stateLock.unlock()
    }

    // MARK: - 段启动 (请求线程同步起第 0 段; 后续段由 starter 流水线起)

    /// 取下一个待启动段 (须 ≤ 渲染位置+1, 保持流水线深度 1). 无 → -1.
    private func popLaunchIndex() -> Int {
        stateLock.lock()
        defer { stateLock.unlock() }
        if cancelled { return -1 }
        guard nextToStart < plans.count, nextToStart <= currentIdx + 1 else { return -1 }
        let idx = nextToStart
        nextToStart += 1
        return idx
    }

    private func launchSegment(_ idx: Int, requestGen: UInt64) {
        let plan: SegPlan = {
            stateLock.lock()
            defer { stateLock.unlock() }
            return plans[idx]
        }()
        var sr: Int32 = 24000
        let st = mtts_stream_start(plan.engine, plan.voice, plan.text,
                                   plan.rate, plan.pitch, &sr)
        if st == nil {
            NSLog("[MagicTTS] 第 \(idx) 段启动失败 (engine=\(plan.engine) voice=\(plan.voice ?? "-"))")
        }
        stateLock.lock()
        if gen == requestGen {
            streams[idx] = st
            rates[idx] = sr
            started[idx] = true
        } else if let st {
            // 启动期间请求被顶替: 该流不属于当前代际, 立即退役
            mtts_stream_cancel(st)
            retired.append(st)
        }
        stateLock.unlock()
    }

    /// 启动线程: 保持 nextToStart ≤ currentIdx+1; 空闲/取消即退出,
    /// 渲染推进或新请求会再次调度 (重复调度无碍 — 取段在锁内原子).
    private func scheduleStarter() {
        starterQueue.async { [weak self] in
            guard let self else { return }
            while true {
                self.stateLock.lock()
                let g = self.gen
                let cancelled = self.cancelled
                let idle = self.nextToStart >= self.plans.count
                    || self.nextToStart > self.currentIdx + 1
                self.stateLock.unlock()
                if cancelled || idle { return }
                let idx = self.popLaunchIndex()
                if idx < 0 {
                    usleep(10_000)   // 无段可启 (被并发启动线程抢走) → 稍后再看
                    continue
                }
                self.launchSegment(idx, requestGen: g)
            }
        }
    }

    // MARK: - 渲染 (流式: 段队列逐段拉取 + 线性重采样, 末段尽置 Complete)

    private func makeRenderBlock() -> AUInternalRenderBlock {
        { [weak self] actionFlags, _, frameCount, _, audioBufferList, _, _ in
            guard let self else { return kAudioUnitErr_FailedInitialization }

            let abl = UnsafeMutableAudioBufferListPointer(audioBufferList)
            guard let out = abl[0].mData?.assumingMemoryBound(to: Float.self) else {
                return kAudioUnitErr_InvalidParameter
            }
            abl[0].mDataByteSize = UInt32(Int(frameCount) * MemoryLayout<Float>.size)

            // 退休流释放 (仅渲染线程操作, 与 read 无竞争)
            self.stateLock.lock()
            let retiredNow = self.retired
            self.retired = []
            let g = self.gen
            var isCancelled = self.cancelled
            var isPending = self.pending
            var firstStarted = self.started.first ?? true
            let dump = self.dumpAudio
            self.stateLock.unlock()
            for s in retiredNow { mtts_stream_free(s) }

            /* 请求在途而第 0 段未启动 → 有界等待 (stream_start 毫秒级, MS 会话含复位拍) */
            var waitedMs = 0
            while !isCancelled && isPending && !firstStarted && waitedMs < 15_000 {
                usleep(20_000)
                waitedMs += 20
                self.stateLock.lock()
                isCancelled = self.cancelled
                isPending = self.pending
                firstStarted = self.started.first ?? true
                self.stateLock.unlock()
            }

            guard !isCancelled else {
                for i in 0..<Int(frameCount) { out[i] = 0 }
                actionFlags.pointee.insert(.offlineUnitRenderAction_Complete)
                return noErr
            }

            // 新请求 → 复位渲染侧状态
            if g != self.curGen {
                self.curGen = g
                self.srcBuf.removeAll(keepingCapacity: true)
                self.srcPos = 0
                self.curRate = 24000
                self.dumpActive = dump
                self.dbg.removeAll(keepingCapacity: true)
            }

            var step = self.curRate / self.outputBus.format.sampleRate
            let needed = Int(ceil(Double(frameCount) * step)) + 2

            // 拉取源样本: 当前段读尽 → 退役 → 切下一段 (阻塞至有数据/段结束)
            var scratch = [Int16](repeating: 0, count: max(needed, 1))
            var total = 0
            var allDone = false
            var segWaitMs = 0
            while total < needed {
                self.stateLock.lock()
                let idx = self.currentIdx
                let planCount = self.plans.count
                let st = idx < self.streams.count ? self.streams[idx] : nil
                let attempted = idx < self.started.count ? self.started[idx] : true
                let gain = idx < self.plans.count ? self.plans[idx].gain : 1
                let nextRate = self.rates.indices.contains(idx) ? self.rates[idx] : 24000
                self.stateLock.unlock()

                if idx >= planCount { allDone = true; break }
                if nextRate > 0 && Double(nextRate) != self.curRate {
                    self.curRate = Double(nextRate)   // 段切换: 重采样率随段换
                    step = self.curRate / self.outputBus.format.sampleRate
                }
                if st == nil {
                    if !attempted {
                        // 段尚未启动: 促发启动线程并等待 (兜底防启动线程早退死等)
                        self.scheduleStarter()
                        usleep(50_000)
                        segWaitMs += 50
                        if segWaitMs > 10_000 {   // 启动超时 → 按失败跳段
                            self.stateLock.lock()
                            self.currentIdx += 1
                            self.stateLock.unlock()
                        }
                        continue
                    }
                    // 启动失败 → 跳段
                    self.stateLock.lock()
                    self.currentIdx += 1
                    self.stateLock.unlock()
                    continue
                }

                var done: Int32 = 0
                let got = mtts_stream_read(st!, &scratch[total], Int32(needed - total), &done)
                if got > 0 {
                    let base = total
                    self.srcBuf.reserveCapacity(self.srcBuf.count + Int(got))
                    for i in 0..<Int(got) {
                        self.srcBuf.append(Float(scratch[base + i]) / 32768.0 * gain)
                    }
                    total += Int(got)
                    if self.dumpActive {
                        self.dbg.append(contentsOf: self.srcBuf.suffix(Int(got)))
                    }
                }
                if done != 0 || got == 0 {
                    // 段读尽: 退役 (若未被新请求顶替) 并推进
                    self.stateLock.lock()
                    if idx < self.streams.count && self.streams[idx] == st {
                        self.retired.append(st!)
                        self.streams[idx] = nil
                        self.currentIdx = idx + 1
                    }
                    self.stateLock.unlock()
                    self.scheduleStarter()   // 渲染推进 → 促发下一段启动
                }
            }

            // 线性重采样输出
            var hitEnd = false
            for i in 0..<Int(frameCount) {
                let i0 = Int(self.srcPos)
                if i0 + 1 < self.srcBuf.count {
                    let frac = Float(self.srcPos - Double(i0))
                    out[i] = self.srcBuf[i0] * (1 - frac) + self.srcBuf[i0 + 1] * frac
                } else {
                    out[i] = 0
                    if allDone { hitEnd = true }
                }
                self.srcPos += step
            }

            // 已消费区定期回收
            if self.srcPos > 65536 {
                let drop = Int(self.srcPos)
                self.srcBuf.removeFirst(min(drop, self.srcBuf.count))
                self.srcPos -= Double(drop)
            }

            if hitEnd {
                actionFlags.pointee.insert(.offlineUnitRenderAction_Complete)
                if self.dumpActive {
                    self.writeDebugWav(self.dbg, self.dbgRate)
                }
            }
            return noErr
        }
    }

    /// 调试转储 (默认关闭; 触碰 <容器>/Engines/.debug/dump 开启):
    /// 渲染结束时把引擎侧输出写为 .debug/last_<n>.wav (轮转 8 个).
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
