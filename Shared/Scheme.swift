import Foundation

/// 主/副侧的语言角色。当前固定 中文主 / 英文副;
/// 将来接系统语言判断 (英文系统 → 主 en-US / 副 zh-CN) 时只改这里。
enum SchemeSides {
    static let defaultPrimaryLocale = "zh-CN"
    static let defaultSecondaryLocale = "en-US"
}

/// 方案一侧的发音人与参数。所有参数由 MagicTTS 托管
/// (系统请求里本就没有音量; 方案语音连语速/音高也一并忽略请求值)。
struct SchemeVoiceConfig: Codable, Equatable {
    var engine: String         // 引擎键 (yongde / naturalvoice / onecore)
    var voice: String          // 精确发音人键; "" = 按 autoLocale 自动选
    var autoLocale: String     // voice 为空时: 引擎内首个此语言发音人, 无则默认包
    var rate: Int = 100        // 20...500, 100 = 常速 (桥刻度)
    var pitch: Int = 50        // 0...100, 50 = 常音高
    var volume: Int = 100      // 0...200 (%), 渲染级逐样本增益
    var eqLow: Int = 0         // -12...12 dB, 三段音色均衡 (0 = 不滤波), 见 Eq3Band
    var eqMid: Int = 0
    var eqHigh: Int = 0

    init(engine: String, voice: String = "", autoLocale: String,
         rate: Int = 100, pitch: Int = 50, volume: Int = 100,
         eqLow: Int = 0, eqMid: Int = 0, eqHigh: Int = 0) {
        self.engine = engine
        self.voice = voice
        self.autoLocale = autoLocale
        self.rate = rate
        self.pitch = pitch
        self.volume = volume
        self.eqLow = eqLow
        self.eqMid = eqMid
        self.eqHigh = eqHigh
    }

    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        engine = try c.decode(String.self, forKey: .engine)
        voice = try c.decodeIfPresent(String.self, forKey: .voice) ?? ""
        autoLocale = try c.decodeIfPresent(String.self, forKey: .autoLocale) ?? "zh-CN"
        rate = try c.decodeIfPresent(Int.self, forKey: .rate) ?? 100
        pitch = try c.decodeIfPresent(Int.self, forKey: .pitch) ?? 50
        volume = try c.decodeIfPresent(Int.self, forKey: .volume) ?? 100
        eqLow = try c.decodeIfPresent(Int.self, forKey: .eqLow) ?? 0
        eqMid = try c.decodeIfPresent(Int.self, forKey: .eqMid) ?? 0
        eqHigh = try c.decodeIfPresent(Int.self, forKey: .eqHigh) ?? 0
    }
}

/// 双语方案: 主发音人读中文、副发音人读英文, 规则见 TextSegmenter。
/// useSecondary=false 时单语音朗读 (全部文本归主侧, 切段后重并)。
struct Scheme: Codable, Equatable {
    var label: String = ""         // 方案名称; 系统目录里的槽位名固定 (见 SchemeStore.slotNames)
    var primary: SchemeVoiceConfig
    var secondary: SchemeVoiceConfig
    var rules: SegmentRuleOptions = SegmentRuleOptions()
    var useRules: Bool = true      // 是否应用全局朗读替换规则 (rules.json)
    var useSecondary: Bool = true  // 启用第二语音 (false = 仅主引擎朗读)

    init(primary: SchemeVoiceConfig, secondary: SchemeVoiceConfig,
         label: String = "", rules: SegmentRuleOptions = SegmentRuleOptions(),
         useRules: Bool = true, useSecondary: Bool = true) {
        self.label = label
        self.primary = primary
        self.secondary = secondary
        self.rules = rules
        self.useRules = useRules
        self.useSecondary = useSecondary
    }

    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        label = try c.decodeIfPresent(String.self, forKey: .label) ?? ""
        primary = try c.decode(SchemeVoiceConfig.self, forKey: .primary)
        secondary = try c.decode(SchemeVoiceConfig.self, forKey: .secondary)
        rules = try c.decodeIfPresent(SegmentRuleOptions.self, forKey: .rules) ?? SegmentRuleOptions()
        useRules = try c.decodeIfPresent(Bool.self, forKey: .useRules) ?? true
        useSecondary = try c.decodeIfPresent(Bool.self, forKey: .useSecondary) ?? true
    }
}

/// 方案存取与解析。
///
/// 槽位 id 池固定 (dual1..dual8, 系统语音 identifier 稳定), 但**方案是否创建
/// = 是否出现在 scheme.json 里**: UI 增删改直接增删 json 键, 增/删后调
/// updateSpeechVoices 让系统目录重查。槽位→实际配置的映射**每次请求时读取**
/// → 改配置即时生效, 无需重装 app。
enum SchemeStore {
    static let slots = ["dual1", "dual2", "dual3", "dual4", "dual5", "dual6", "dual7", "dual8"]
    static let slotNames = [
        "dual1": "语音方案·一", "dual2": "语音方案·二", "dual3": "语音方案·三", "dual4": "语音方案·四",
        "dual5": "语音方案·五", "dual6": "语音方案·六", "dual7": "语音方案·七", "dual8": "语音方案·八",
    ]

    static var url: URL {
        MagicPaths.enginesRoot.appendingPathComponent("scheme.json")
    }

    static func defaults() -> [String: Scheme] {
        [
            /* dual1 默认 = Kona (系统内置 Eloquence/ViaVoice): 同一发音人 Eddy
             * 的中文+英文两侧 — 复刻 VVTTS 的 0=中文/1=英文 双音库模式,
             * 且零物料依赖 (见 NOTES-vvtts.md 第十三节)。 */
            "dual1": Scheme(
                primary: SchemeVoiceConfig(engine: "kona", voice: "eddy-zh", autoLocale: SchemeSides.defaultPrimaryLocale),
                secondary: SchemeVoiceConfig(engine: "kona", voice: "eddy-en", autoLocale: SchemeSides.defaultSecondaryLocale),
                label: "Eddy 中文 + 英文 (Kona)"),
            "dual2": Scheme(
                primary: SchemeVoiceConfig(engine: "yongde", autoLocale: SchemeSides.defaultPrimaryLocale),
                secondary: SchemeVoiceConfig(engine: "onecore", autoLocale: SchemeSides.defaultSecondaryLocale),
                label: "永德 + 英文 (OneCore)"),
        ]
    }

    /// 槽位无配置时的兜底方案 (系统目录残留已删槽位的请求 → 仍有声可读)。
    static func fallback() -> Scheme {
        defaults()["dual1"]!
    }

    /// 逐槽读取: json 里出现的槽位才算已创建 (删除 = 从 json 消失)。
    /// 缺文件/坏文件 → 种子默认两条; 单个槽位解码坏 → 该槽视为未创建。
    static func loadAll() -> [String: Scheme] {
        guard let data = try? Data(contentsOf: url),
              let raw = try? JSONDecoder().decode([String: Scheme].self, from: data) else {
            return defaults()
        }
        var out: [String: Scheme] = [:]
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
