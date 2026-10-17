import AVFoundation
import Foundation

/// Magic TTS 语音合成提供器.
///
/// 生命周期:
///   原版 onSynthesizeText   → synthesizeSpeechRequest
///   原版 audioAvailable     → internalRenderBlock 输出 Float32
///   原版 onStop             → cancelSpeechRequest
///
/// 最简实现: 整段合成到内存, 渲染块线性重采样拉流 (二期改流式).
/// 注意: 若个别 API 名与当前 SDK 出入, 以 Xcode 补全为准 (WWDC23 #10033 示例面).
public final class MagicSpeechProviderAU: AVSpeechSynthesisProviderAudioUnit {

    // MARK: - 引擎桥 (C)

    private static let bridgeQueue = DispatchQueue(label: "magictts.bridge.init")
    private static var bridgeReady = false

    private static func ensureBridge() {
        bridgeQueue.sync {
            guard !bridgeReady else { return }
            let root = MagicPaths.enginesRoot.path
            let loaded = mtts_global_init(root)
            NSLog("[MagicTTS] bridge init: \(loaded) 个引擎就绪")
            bridgeReady = true
        }
    }

    // MARK: - 渲染状态 (synth 线程生产, render 线程消费)

    private final class UtteranceBuffer {
        let pcm: [Float]        // 引擎采样率下的 Float32
        let sampleRate: Double
        let pos = Position()    // 播放位置 (源采样率空间)

        init(pcm: [Float], sampleRate: Double) {
            self.pcm = pcm
            self.sampleRate = sampleRate
        }

        final class Position {
            var value: Double = 0
        }
    }

    private let stateLock = NSLock()
    private var current: UtteranceBuffer?
    private var pending = false      // 请求已接受, 引擎合成中 (MS 引擎需数秒)
    private var cancelled = true

    private let outputBus: AUAudioUnitBus
    private var storedRenderBlock: AUInternalRenderBlock!

    // MARK: - AU 构造

    public override init(componentDescription: AudioComponentDescription, options: AudioComponentInstantiationOptions = []) throws {
        Self.ensureBridge()

        let format = AVAudioFormat(standardFormatWithSampleRate: 24000, channels: 1)!
        outputBus = try AUAudioUnitBus(format: format)

        try super.init(componentDescription: componentDescription, options: options)

        storedRenderBlock = makeRenderBlock()
    }

    public override var outputBusses: AUAudioUnitBusArray {
        AUAudioUnitBusArray(audioUnit: self, busType: .output, busses: [outputBus])
    }

    // MARK: - Speech Synthesis Provider (核心三件套)

    /// 声明语音.
    public override var speechVoices: [AVSpeechSynthesisProviderVoice] {
        get {
            var voices: [AVSpeechSynthesisProviderVoice] = []
            let n = mtts_engine_count()
            for i in 0..<n {
                guard let d = mtts_engine_at(Int32(i)) else { continue }
                let desc = d.pointee
                voices.append(AVSpeechSynthesisProviderVoice(
                    name: String(cString: desc.voice_name),
                    identifier: String(cString: desc.identifier),
                    primaryLanguages: [String(cString: desc.locale)],
                    supportedLanguages: [String(cString: desc.locale)]))
            }
            return voices
        }
        set {}   // ObjC 属性可仅覆写 getter; 系统不写此属性
    }

    /// 合成请求 — 系统送来 SSML + voice. 后台线程跑引擎 (MS 需数秒),
    /// 渲染块以 pending 状态等待 (离线语音 AU 模式).
    public override func synthesizeSpeechRequest(_ request: AVSpeechSynthesisProviderRequest) {
        let parsed = SpeechRequestParser.parse(request.ssmlRepresentation)
        let voiceId = request.voice.identifier
        let engineKey = String(voiceId.split(separator: ".").last ?? "yongde")

        NSLog("[MagicTTS] synthesize engine=\(engineKey) rate=\(parsed.rate) pitch=\(parsed.pitch) text=\(parsed.text.prefix(32))")

        stateLock.lock()
        cancelled = false
        pending = true
        current = nil
        stateLock.unlock()

        DispatchQueue.global(qos: .userInitiated).async { [weak self] in
            var pcm: UnsafeMutablePointer<Int16>? = nil
            var samples: Int32 = 0
            var sampleRate: Int32 = 0
            let rc = mtts_synthesize(engineKey, parsed.text,
                                     Int32(parsed.rate), Int32(parsed.pitch),
                                     &pcm, &samples, &sampleRate)

            guard rc == MTTS_OK, let p = pcm, samples > 0 else {
                NSLog("[MagicTTS] 合成失败 rc=\(rc)")
                self?.stateLock.lock()
                self?.pending = false
                self?.current = nil
                self?.cancelled = true
                self?.stateLock.unlock()
                return
            }

            // int16 → Float32 (引擎采样率)
            var floats = [Float](repeating: 0, count: Int(samples))
            for i in 0..<Int(samples) { floats[i] = Float(p[i]) / 32768.0 }
            mtts_free(p)

            self?.stateLock.lock()
            self?.current = UtteranceBuffer(pcm: floats, sampleRate: Double(sampleRate))
            self?.pending = false
            self?.stateLock.unlock()

            self?.writeDebugWav(engineKey, floats, Int(sampleRate))
        }
    }

    /// 调试转储: 每次合成把引擎实际输出写为 <组容器>/Engines/.debug/last_<key>.wav.
    /// 证明 AU 扩展确被系统调用, 并可与宿主收到的音频比对 (合线程写, 不在渲染线程).
    private func writeDebugWav(_ key: String, _ pcm: [Float], _ sr: Int) {
        let dir = MagicPaths.enginesRoot.appendingPathComponent(".debug", isDirectory: true)
        try? FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        let url = dir.appendingPathComponent("last_\(key).wav")

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
        NSLog("[MagicTTS] dump: last_\(key).wav \(samples.count) 采样 @\(sr)Hz")
    }

    /// 停止.
    public override func cancelSpeechRequest() {
        NSLog("[MagicTTS] cancel")
        stateLock.lock()
        cancelled = true
        pending = false
        current = nil
        stateLock.unlock()
    }

    // MARK: - 渲染 (离线语音 AU: 按需等待/静音填充, 完成时置 Complete 标志)

    private func makeRenderBlock() -> AUInternalRenderBlock {
        { [weak self] actionFlags, _, frameCount, _, audioBufferList, _, _ in
            guard let self else { return kAudioUnitErr_FailedInitialization }

            // 合成进行中 → 有界等待 (MS 引擎冷启动可达数秒)
            var waitedMs = 0
            var buf: UtteranceBuffer?
            var isCancelled = false
            while true {
                self.stateLock.lock()
                buf = self.current
                isCancelled = self.cancelled
                let waiting = self.pending
                self.stateLock.unlock()
                if buf != nil || isCancelled || !waiting { break }
                if waitedMs >= 15_000 { break }   // 尽力而为, 交静音
                usleep(20_000)
                waitedMs += 20
            }

            let abl = UnsafeMutableAudioBufferListPointer(audioBufferList)
            guard let out = abl[0].mData?.assumingMemoryBound(to: Float.self) else {
                return kAudioUnitErr_InvalidParameter
            }
            abl[0].mDataByteSize = UInt32(Int(frameCount) * MemoryLayout<Float>.size)

            guard !isCancelled, let buf else {
                // 无请求/已取消/失败 → 通知宿主本轮结束
                for i in 0..<Int(frameCount) { out[i] = 0 }
                actionFlags.pointee.insert(.offlineUnitRenderAction_Complete)
                return noErr
            }

            let step = buf.sampleRate / self.outputBus.format.sampleRate
            var hitEnd = false
            for i in 0..<Int(frameCount) {
                let src = buf.pos.value
                let i0 = Int(src)
                guard i0 + 1 < buf.pcm.count else { out[i] = 0; hitEnd = true; continue }
                let frac = Float(src - Double(i0))
                out[i] = buf.pcm[i0] * (1 - frac) + buf.pcm[i0 + 1] * frac
                buf.pos.value = src + step
            }
            if hitEnd {
                actionFlags.pointee.insert(.offlineUnitRenderAction_Complete)
            }
            return noErr
        }
    }

    public override var internalRenderBlock: AUInternalRenderBlock { storedRenderBlock }
}
