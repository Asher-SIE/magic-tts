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
    private static var initStarted = false

    /// 轻量扫描 (毫秒级, 同步): speechVoices 枚举只依赖它;
    /// 重初始化 (dlopen 引擎库, 秒级) 后台预热 — 系统查询语音列表不能被它卡住.
    private static func ensureBridge() {
        bridgeQueue.sync {
            guard !scanned else { return }
            let root = MagicPaths.enginesRoot.path
            let ready = mtts_global_scan(root)
            NSLog("[MagicTTS] bridge scan: \(ready) 个引擎物料就绪")
            scanned = true
        }
        if !initStarted {
            initStarted = true
            DispatchQueue.global(qos: .userInitiated).async {
                let loaded = mtts_global_init(nil)
                NSLog("[MagicTTS] bridge init: \(loaded) 个引擎就绪 (后台预热完成)")
                /* 预热完成后主动推送一次语音表更新 (宿主与扩展侧均可触发) */
                AVSpeechSynthesisProviderVoice.updateSpeechVoices()
            }
        }
    }

    // MARK: - 请求/流状态 (请求线程写, 渲染线程读)

    private let stateLock = NSLock()
    private var stream: OpaquePointer?      // mtts_stream* (当前)
    private var retired: [OpaquePointer] = []   // 已取消待释放 (渲染线程释放)
    private var streamRate: Int32 = 24000   // 当前流引擎采样率
    private var gen: UInt64 = 0             // 请求代际 (渲染侧据此复位缓冲)
    private var pending = false             // 请求已接受, 流未就绪 (start 毫秒级)
    private var cancelled = true

    // 渲染线程私有 (仅 render block 触碰)
    private var srcBuf: [Float] = []        // 引擎采样率 Float32 累积缓冲
    private var srcPos: Double = 0          // 消费位置 (srcBuf 索引空间, 小数步进)
    private var curGen: UInt64 = 0
    private var curRate: Double = 24000
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
        if let s = stream { mtts_stream_cancel(s); mtts_stream_free(s) }
        for s in retired { mtts_stream_free(s) }
    }

    // MARK: - Speech Synthesis Provider (核心三件套)

    /// 声明语音: 每个引擎的每个发音人一条 (identifier = com.magictts.<engine>.<voicekey>).
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
                    voices.append(AVSpeechSynthesisProviderVoice(
                        name: String(cString: vd.pointee.name),
                        identifier: String(cString: vd.pointee.identifier),
                        primaryLanguages: [locale],
                        supportedLanguages: [locale]))
                }
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

    /// 合成请求 — 系统送来 SSML + voice. 流式启动 (毫秒级), 渲染块拉取.
    public override func synthesizeSpeechRequest(_ request: AVSpeechSynthesisProviderRequest) {
        let parsed = SpeechRequestParser.parse(request.ssmlRepresentation)
        let (engineKey, voiceKey) = Self.parseVoiceId(request.voice.identifier)

        NSLog("[MagicTTS] synthesize engine=\(engineKey) voice=\(voiceKey ?? "(默认)") rate=\(parsed.rate) pitch=\(parsed.pitch) text=\(parsed.text.prefix(32))")

        // 旧流退休 (cancel 立即; 释放留给渲染线程, 避免与在途 read 竞争)
        stateLock.lock()
        if let old = stream { mtts_stream_cancel(old); retired.append(old) }
        stream = nil
        gen &+= 1
        pending = true
        cancelled = false
        stateLock.unlock()

        var sr: Int32 = 0
        guard let st = mtts_stream_start(engineKey, voiceKey, parsed.text,
                                         Int32(parsed.rate), Int32(parsed.pitch), &sr) else {
            NSLog("[MagicTTS] stream_start 失败 (engine=\(engineKey) voice=\(voiceKey ?? "-"))")
            stateLock.lock()
            pending = false
            cancelled = true
            stateLock.unlock()
            return
        }

        stateLock.lock()
        stream = st
        streamRate = sr
        pending = false
        stateLock.unlock()
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

    /// 停止.
    public override func cancelSpeechRequest() {
        NSLog("[MagicTTS] cancel")
        stateLock.lock()
        cancelled = true
        pending = false
        if let s = stream { mtts_stream_cancel(s) }
        stateLock.unlock()
    }

    // MARK: - 渲染 (流式: 按需拉取 + 线性重采样, 结束置 Complete 标志)

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
            var st = self.stream
            var isCancelled = self.cancelled
            var isPending = self.pending
            let g = self.gen
            let sr = self.streamRate
            self.stateLock.unlock()
            for s in retiredNow { mtts_stream_free(s) }

            /* 请求在途而流未就绪 → 有界等待 (stream_start 毫秒级, 首次含 .vl/会话建立) */
            var waitedMs = 0
            while !isCancelled && st == nil && isPending && waitedMs < 15_000 {
                usleep(20_000)
                waitedMs += 20
                self.stateLock.lock()
                st = self.stream
                isCancelled = self.cancelled
                isPending = self.pending
                self.stateLock.unlock()
            }

            guard !isCancelled, let st else {
                for i in 0..<Int(frameCount) { out[i] = 0 }
                actionFlags.pointee.insert(.offlineUnitRenderAction_Complete)
                return noErr
            }

            // 新请求 → 复位渲染侧状态
            if g != self.curGen {
                self.curGen = g
                self.srcBuf.removeAll(keepingCapacity: true)
                self.srcPos = 0
                self.curRate = Double(sr)
                self.dbg.removeAll(keepingCapacity: true)
                self.dbgRate = Int(sr)
            }

            let step = self.curRate / self.outputBus.format.sampleRate
            let needed = Int(ceil(Double(frameCount) * step)) + 2

            // 拉取源样本 (阻塞至有数据/流结束; MS 首块 ~0.8s)
            var scratch = [Int16](repeating: 0, count: max(needed, 1))
            var total = 0
            var streamDone = false
            while total < needed {
                var done: Int32 = 0
                let got = mtts_stream_read(st, &scratch[total], Int32(needed - total), &done)
                total += Int(got)
                if done != 0 || got == 0 { streamDone = true; break }
            }
            if total > 0 {
                self.srcBuf.reserveCapacity(self.srcBuf.count + total)
                for i in 0..<total { self.srcBuf.append(Float(scratch[i]) / 32768.0) }
                self.dbg.append(contentsOf: self.srcBuf.suffix(total))
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
                    if streamDone { hitEnd = true }
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
                self.writeDebugWav(self.dbg, self.dbgRate)
            }
            return noErr
        }
    }

    /// 调试转储: 渲染结束时把引擎侧输出写为 <组容器>/Engines/.debug/last_<n>.wav (轮转 8 个).
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
