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
                let systemId = systemVoices
                    .first { $0.identifier == ourId || $0.identifier.hasSuffix("." + ourId) }?
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

    /// 试听: 用系统合成器以我们的语音完整 identifier 朗读.
    /// 语音未注册时直接不发声 (发 nil voice 会静默回退系统默认语音, 反而误导).
    static func preview(_ voice: VoiceRow, text: String = "语音测试，一二三。") {
        guard let voiceId = voice.systemVoiceId,
              let v = AVSpeechSynthesisVoice(identifier: voiceId) else {
            NSLog("[MagicTTS] 试听取消: %@ 未注册到系统 (先点「刷新语音列表」)", voice.identifier)
            return
        }
        let synth = AVSpeechSynthesizer()
        let u = AVSpeechUtterance(string: text)
        u.voice = v
        u.rate = 0.5
        synth.speak(u)
        PreviewHolder.retain(synth)
    }

    private enum PreviewHolder {
        nonisolated(unsafe) static var alive: [AVSpeechSynthesizer] = []
        static func retain(_ s: AVSpeechSynthesizer) { alive.append(s) }
    }
}
