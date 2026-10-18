import Foundation

/// 方案一侧的发音人与参数。所有参数由 MagicTTS 托管
/// (系统请求里本就没有音量; 方案语音连语速/音高也一并忽略请求值)。
struct SchemeVoiceConfig: Codable, Equatable {
    var engine: String         // 引擎键 (yongde / naturalvoice / onecore)
    var voice: String          // 精确发音人键; "" = 按 autoLocale 自动选
    var autoLocale: String     // voice 为空时: 引擎内首个此语言发音人, 无则默认包
    var rate: Int = 100        // 20...500, 100 = 常速 (桥刻度)
    var pitch: Int = 50        // 0...100, 50 = 常音高
    var volume: Int = 100      // 0...200 (%), 渲染级逐样本增益

    init(engine: String, voice: String = "", autoLocale: String,
         rate: Int = 100, pitch: Int = 50, volume: Int = 100) {
        self.engine = engine
        self.voice = voice
        self.autoLocale = autoLocale
        self.rate = rate
        self.pitch = pitch
        self.volume = volume
    }

    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        engine = try c.decode(String.self, forKey: .engine)
        voice = try c.decodeIfPresent(String.self, forKey: .voice) ?? ""
        autoLocale = try c.decodeIfPresent(String.self, forKey: .autoLocale) ?? "zh-CN"
        rate = try c.decodeIfPresent(Int.self, forKey: .rate) ?? 100
        pitch = try c.decodeIfPresent(Int.self, forKey: .pitch) ?? 50
        volume = try c.decodeIfPresent(Int.self, forKey: .volume) ?? 100
    }
}

/// 双语方案: 主发音人读中文、副发音人读英文, 规则见 TextSegmenter。
struct Scheme: Codable, Equatable {
    var label: String = ""         // 备注; 系统目录里的槽位名固定 (见 SchemeStore.slotNames)
    var primary: SchemeVoiceConfig
    var secondary: SchemeVoiceConfig
    var rules: SegmentRuleOptions = SegmentRuleOptions()

    init(primary: SchemeVoiceConfig, secondary: SchemeVoiceConfig,
         label: String = "", rules: SegmentRuleOptions = SegmentRuleOptions()) {
        self.label = label
        self.primary = primary
        self.secondary = secondary
        self.rules = rules
    }

    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        label = try c.decodeIfPresent(String.self, forKey: .label) ?? ""
        primary = try c.decode(SchemeVoiceConfig.self, forKey: .primary)
        secondary = try c.decode(SchemeVoiceConfig.self, forKey: .secondary)
        rules = try c.decodeIfPresent(SegmentRuleOptions.self, forKey: .rules) ?? SegmentRuleOptions()
    }
}

/// 方案存取与解析。
///
/// 槽位 (双语方案·一/二) 在 speechVoices 里**固定注册** — 语音目录对未变化的
/// app 不重查, 固定槽位只需注册一次; 槽位→实际配置的映射存 scheme.json,
/// **每次请求时读取** → 改配置即时生效, 无需重装 app。
enum SchemeStore {
    static let slots = ["dual1", "dual2"]
    static let slotNames = ["dual1": "双语方案·一", "dual2": "双语方案·二"]

    static var url: URL {
        MagicPaths.enginesRoot.appendingPathComponent("scheme.json")
    }

    static func defaults() -> [String: Scheme] {
        [
            /* dual1 默认 = Kona (系统内置 Eloquence/ViaVoice): 同一发音人 Eddy
             * 的中文+英文两侧 — 复刻 VVTTS 的 0=中文/1=英文 双音库模式,
             * 且零物料依赖 (见 NOTES-vvtts.md 第十三节)。 */
            "dual1": Scheme(
                primary: SchemeVoiceConfig(engine: "kona", voice: "eddy-zh", autoLocale: "zh-CN"),
                secondary: SchemeVoiceConfig(engine: "kona", voice: "eddy-en", autoLocale: "en-US"),
                label: "Eddy 中文 + 英文 (Kona)"),
            "dual2": Scheme(
                primary: SchemeVoiceConfig(engine: "yongde", autoLocale: "zh-CN"),
                secondary: SchemeVoiceConfig(engine: "onecore", autoLocale: "en-US"),
                label: "永德 + 英文 (OneCore)"),
        ]
    }

    /// 逐槽读取, 坏槽位/缺文件回退默认 — 单个手改错误不会拖垮全部方案。
    static func loadAll() -> [String: Scheme] {
        guard let data = try? Data(contentsOf: url),
              let raw = try? JSONDecoder().decode([String: Scheme].self, from: data) else {
            return defaults()
        }
        var out = defaults()
        for slot in slots where raw[slot] != nil { out[slot] = raw[slot]! }
        return out
    }

    static func save(_ all: [String: Scheme]) throws {
        let enc = JSONEncoder()
        enc.outputFormatting = [.prettyPrinted, .sortedKeys]
        try enc.encode(all).write(to: url, options: [.atomic])
    }

    /// 发音人解析: 精确键 → 引擎内首个 autoLocale 匹配 → 引擎默认包。
    /// 无论实际装了哪些包都能给出可用答案 (默认包兜底)。
    static func resolve(_ c: SchemeVoiceConfig) -> (engine: String, voice: String?) {
        if !c.voice.isEmpty {
            let vc = mtts_voice_count(c.engine)
            for i in 0..<vc {
                guard let d = mtts_voice_at(c.engine, i) else { continue }
                if String(cString: d.pointee.key) == c.voice { return (c.engine, c.voice) }
            }
        }
        let vc = mtts_voice_count(c.engine)
        for i in 0..<vc {
            guard let d = mtts_voice_at(c.engine, i) else { continue }
            if String(cString: d.pointee.locale) == c.autoLocale {
                return (c.engine, String(cString: d.pointee.key))
            }
        }
        return (c.engine, nil)
    }
}
