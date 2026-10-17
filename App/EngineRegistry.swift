import AVFoundation
import Foundation

/// 引擎状态模型 — 读取 C 桥的注册表 + 扫描 Engines 目录物料.
struct EngineStatus: Identifiable {
    let key: String
    let name: String
    let identifier: String          // 我们声明的语音 id: com.magictts.<key>
    let systemVoiceId: String?      // 系统注册后的完整 id (带 appex bundle 前缀); 未注册为 nil
    let dylib: String
    let loaded: Bool     // 引擎就绪 (库+数据)
    let hasFiles: Bool   // Engines/<key>/ 目录非空

    var id: String { key }
    var mode: String { loaded ? "就绪" : (hasFiles ? "物料不全" : "缺少物料") }
}

enum EngineRegistry {

    /// 初始化 C 桥并枚举引擎状态.
    static func engines() -> [EngineStatus] {
        let fm = FileManager.default
        let root = MagicPaths.enginesRoot
        try? fm.createDirectory(at: root, withIntermediateDirectories: true)

        _ = mtts_global_init(root.path)

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
            let ourId = String(cString: desc.identifier)
            let dir = root.appendingPathComponent(key, isDirectory: true)
            let files = (try? fm.contentsOfDirectory(atPath: dir.path)) ?? []
            let systemId = systemVoices
                .first { $0.identifier == ourId || $0.identifier.hasSuffix("." + ourId) }?
                .identifier
            NSLog("[MagicTTS] engine %@: %ld 个物料文件, %@, 系统语音=%@", key, files.count,
                  files.isEmpty ? "缺少物料" : "物料不全", systemId ?? "未注册!")
            out.append(EngineStatus(
                key: key,
                name: String(cString: desc.voice_name),
                identifier: ourId,
                systemVoiceId: systemId,
                dylib: String(cString: desc.dylib_name),
                loaded: mtts_engine_loaded(key) == 1,
                hasFiles: !files.isEmpty))
        }
        return out
    }

    /// 通知系统语音列表刷新 (安装/移除物料后调用).
    static func refreshVoices() {
        AVSpeechSynthesisProviderVoice.updateSpeechVoices()
    }

    /// 试听: 用系统合成器以我们的语音完整 identifier 朗读.
    /// 语音未注册时直接不发声 (发 nil voice 会静默回退系统默认语音, 反而误导).
    static func preview(_ engine: EngineStatus, text: String = "语音测试，一二三。") {
        guard let voiceId = engine.systemVoiceId,
              let voice = AVSpeechSynthesisVoice(identifier: voiceId) else {
            NSLog("[MagicTTS] 试听取消: %@ 未注册到系统 (先点「刷新语音列表」)", engine.identifier)
            return
        }
        let synth = AVSpeechSynthesizer()
        let u = AVSpeechUtterance(string: text)
        u.voice = voice
        u.rate = 0.5
        synth.speak(u)
        PreviewHolder.retain(synth)
    }

    private enum PreviewHolder {
        nonisolated(unsafe) static var alive: [AVSpeechSynthesizer] = []
        static func retain(_ s: AVSpeechSynthesizer) { alive.append(s) }
    }
}
