import AVFoundation
import Foundation

/// 发音人状态模型 — 引擎 × 发音人 (C 桥 mtts_voice_*).
struct VoiceRow: Identifiable {
    let engineKey: String
    let engineName: String
    let key: String                  // 发音人选择键 (ydvoice01 / 包目录名)
    let name: String                 // 显示名 "永德·语音1"
    let identifier: String           // 我们声明的语音 id: com.magictts.<engine>.<key>
    let systemVoiceId: String?       // 系统注册后的完整 id (带 appex bundle 前缀); 未注册为 nil
    var id: String { identifier }
}

struct EngineStatus: Identifiable {
    let key: String
    let name: String
    let dylib: String
    let loaded: Bool     // 引擎就绪 (库+数据)
    let hasFiles: Bool   // Engines/<key>/ 目录非空
    let voices: [VoiceRow]

    var id: String { key }
    var mode: String { loaded ? "就绪 (\(voices.count) 发音人)" : (hasFiles ? "物料不全" : "缺少物料") }
}

enum EngineRegistry {

    /// 扫描引擎与发音人 (轻量, 毫秒级; 重初始化由 appex 端按需/预热).
    static func engines() -> [EngineStatus] {
        let fm = FileManager.default
        let root = MagicPaths.enginesRoot
        try? fm.createDirectory(at: root, withIntermediateDirectories: true)

        _ = mtts_global_scan(root.path)

        /* 系统注册的语音 identifier 会被加上 appex bundle 前缀
         * (com.magictts.voices.synth.<我们的id>), 试听必须用完整 id,
         * 否则 AVSpeechSynthesisVoice(identifier:) 返回 nil → 静默回退默认语音. */
        let systemVoices = AVSpeechSynthesisVoice.speechVoices()

        var out: [EngineStatus] = []
        let n = mtts_engine_count()
        for i in 0..<n {
            guard let d = mtts_engine_at(i) else { continue }
            let desc = d.pointee
            let key = String(cString: desc.key)
            let dir = root.appendingPathComponent(key, isDirectory: true)
            let files = (try? fm.contentsOfDirectory(atPath: dir.path)) ?? []

            var rows: [VoiceRow] = []
            let vc = mtts_voice_count(key)
            for v in 0..<vc {
                guard let vd = mtts_voice_at(key, v) else { continue }
                let ourId = String(cString: vd.pointee.identifier)
                /* 系统目录里可能是兜底注册的引擎级 id (枚举上下文读不到包目录),
                 * 逐级匹配: 完整 id → 引擎级 id. */
                let engineLevelId = "com.magictts." + key
                let systemId = systemVoices
                    .first { $0.identifier == ourId
                          || $0.identifier.hasSuffix("." + ourId)
                          || $0.identifier.hasSuffix("." + engineLevelId) }?
                    .identifier
                rows.append(VoiceRow(
                    engineKey: key,
                    engineName: String(cString: desc.voice_name),
                    key: String(cString: vd.pointee.key),
                    name: String(cString: vd.pointee.name),
                    identifier: ourId,
                    systemVoiceId: systemId))
            }
            if rows.contains(where: { $0.systemVoiceId == nil }) {
                NSLog("[MagicTTS] %@ 有发音人未注册到系统 (点「刷新语音列表」)", key)
            }
            out.append(EngineStatus(
                key: key,
                name: String(cString: desc.voice_name),
                dylib: String(cString: desc.dylib_name),
                loaded: mtts_engine_loaded(key) == 1,
                hasFiles: !files.isEmpty,
                voices: rows))
        }
        return out
    }

    /// 通知系统语音列表刷新 (安装/移除物料后调用).
    static func refreshVoices() {
        AVSpeechSynthesisProviderVoice.updateSpeechVoices()
    }

    /// 试听: 直接走 C 桥合成 + AVAudioPlayer 播放 — 不依赖系统注册状态
    /// (2026-10-17: 系统语音注册管道在反复重装后可能僵死, 注销重登才恢复;
    ///  试听是自家 UI, 没理由绕系统).
    static func preview(_ voice: VoiceRow, text: String = "语音测试，一二三。") {
        var sr: Int32 = 24000
        guard let st = mtts_stream_start(voice.engineKey, voice.key,
                                         text, 100, 50, &sr) else {
            NSLog("[MagicTTS] 试听取消: 引擎合成失败 (%@)", voice.identifier)
            return
        }
        DispatchQueue.global(qos: .userInitiated).async {
            defer { mtts_stream_free(st) }
            var samples: [Int16] = []
            samples.reserveCapacity(48000)
            let chunk = 4096
            var buf = [Int16](repeating: 0, count: chunk)
            while samples.count < 48000 * 30 {
                var done: Int32 = 0
                let got = mtts_stream_read(st, &buf, Int32(chunk), &done)
                if got > 0 { samples.append(contentsOf: buf[0..<Int(got)]) }
                if done != 0 { break }
            }
            guard samples.count > 0 else { return }
            let fmt = AVAudioFormat(commonFormat: .pcmFormatFloat32,
                                    sampleRate: Double(sr),
                                    channels: 1, interleaved: false)!
            guard let pb = AVAudioPCMBuffer(pcmFormat: fmt, frameCapacity: AVAudioFrameCount(samples.count)) else { return }
            pb.frameLength = AVAudioFrameCount(samples.count)
            let ch = pb.floatChannelData![0]
            for i in 0..<samples.count { ch[i] = Float(samples[i]) / 32768.0 }
            do {
                let player = try AVAudioPlayer(contentsOf: PreviewHolder.writeWav(samples, rate: sr))
                player.volume = 1.0
                player.play()
                PreviewHolder.retain(player)
            } catch {
                NSLog("[MagicTTS] 试听播放失败: %@", error.localizedDescription)
            }
        }
    }

    /// 方案试听样例: 覆盖中文、英文短语、混夹缩写、孤立字母、数字。
    static let schemePreviewText = "方案试听：中文与 English mixed reading，NVDA 发布会 at 3 点，字母 Z 分组，圆周率 3.14。"

    /// 方案试听: 与 appex 渲染同一条分发链 (切段 → 逐段桥合成 → 重采样 24k 拼接,
    /// 各段带方案音量增益; 单语音模式归主侧重并, 孤立大写字母段带音高增量)
    /// → AVAudioPlayer 播放。
    static func previewScheme(_ scheme: Scheme) {
        // 与 appex 同链: 先过朗读替换规则 (方案开关), 再切段
        let text = scheme.useRules ? RuleStore.shared().apply(schemePreviewText) : schemePreviewText
        var segs = TextSegmenter.segment(text, options: scheme.rules)
        if !scheme.useSecondary {
            segs = TextSegmenter.coalesce(segs.map {
                TextSegment(text: $0.text, target: .primary, isolatedCapital: $0.isolatedCapital)
            })
        }
        DispatchQueue.global(qos: .userInitiated).async {
            _ = mtts_global_scan(MagicPaths.enginesRoot.path)
            let outRate: Double = 24000
            // resolve 依赖 scan 结果, 须在 scan 后 (与 appex 顺序一致)
            let p = SchemeStore.resolve(scheme.primary)
            let s = scheme.useSecondary ? SchemeStore.resolve(scheme.secondary) : p
            var pcm: [Float] = []
            for seg in segs {
                let isPrimary = !scheme.useSecondary || seg.target == .primary
                let side = isPrimary ? scheme.primary : scheme.secondary
                let r = isPrimary ? p : s
                var pitch = side.pitch
                if seg.isolatedCapital { pitch = min(100, pitch + LetterCaseStyle.pitchDelta) }
                var sr: Int32 = 24000
                guard let st = mtts_stream_start(r.engine, r.voice, seg.text,
                                                 Int32(side.rate), Int32(pitch), &sr) else {
                    NSLog("[MagicTTS] 方案试听: 第 %d 段合成失败 (%@)", pcm.count, r.engine)
                    continue
                }
                defer { mtts_stream_free(st) }
                var segSamples: [Int16] = []
                let chunk = 4096
                var buf = [Int16](repeating: 0, count: chunk)
                while segSamples.count < 48000 * 30 {
                    var done: Int32 = 0
                    let got = mtts_stream_read(st, &buf, Int32(chunk), &done)
                    if got > 0 { segSamples.append(contentsOf: buf[0..<Int(got)]) }
                    if done != 0 { break }
                }
                guard !segSamples.isEmpty else { continue }
                // 重采样到 24k + 增益
                let stepIn = Double(sr) / outRate
                let outCount = Int(Double(segSamples.count) / stepIn)
                let gain = Float(side.volume) / 100.0
                for i in 0..<outCount {
                    let p = Double(i) * stepIn
                    let i0 = Int(p)
                    let frac = Float(p - Double(i0))
                    let a = i0 < segSamples.count ? Float(segSamples[i0]) : 0
                    let b = i0 + 1 < segSamples.count ? Float(segSamples[i0 + 1]) : a
                    pcm.append((a + (b - a) * frac) / 32768.0 * gain)
                }
            }
            guard pcm.count > 0 else { return }
            var pcm16 = pcm.map { Int16(max(-32768, min(32767, Int($0 * 32767)))) }
            do {
                let player = try AVAudioPlayer(contentsOf: PreviewHolder.writeWav(pcm16, rate: 24000))
                player.volume = 1.0
                player.play()
                PreviewHolder.retain(player)
            } catch {
                NSLog("[MagicTTS] 方案试听播放失败: %@", error.localizedDescription)
            }
        }
    }

    private enum PreviewHolder {
        nonisolated(unsafe) static var alive: [AVAudioPlayer] = []
        static func retain(_ p: AVAudioPlayer) { alive.append(p); trim() }
        static func trim() {
            if alive.count > 4 { alive.removeFirst(alive.count - 4) }
        }
        static func writeWav(_ samples: [Int16], rate: Int32) -> URL {
            let url = FileManager.default.temporaryDirectory
                .appendingPathComponent("mtts_preview.wav")
            var data = Data()
            func le16(_ v: Int16) -> Data { withUnsafeBytes(of: v.littleEndian) { Data($0) } }
            func le32(_ v: Int32) -> Data { withUnsafeBytes(of: v.littleEndian) { Data($0) } }
            let n = Int32(samples.count)
            data.append(contentsOf: [UInt8]("RIFF".utf8)); data.append(le32(36 + n * 2))
            data.append(contentsOf: [UInt8]("WAVE".utf8))
            data.append(contentsOf: [UInt8]("fmt ".utf8)); data.append(le32(16))
            data.append(le16(1)); data.append(le16(1))
            data.append(le32(rate)); data.append(le32(rate * 2))
            data.append(le16(2)); data.append(le16(16))
            data.append(contentsOf: [UInt8]("data".utf8)); data.append(le32(n * 2))
            samples.withUnsafeBytes { data.append(contentsOf: $0) }
            try? data.write(to: url)
            return url
        }
    }
}
