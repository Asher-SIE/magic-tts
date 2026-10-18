import Foundation

// MARK: - 分发目标

enum SegmentTarget {
    case primary    // 主发音人 (默认中文)
    case secondary  // 副发音人 (默认英文)
}

// MARK: - 规则参数 (随 scheme.json 持久化)

/// 大小写字母处理 — 作用于"孤立的单个大写字母":
/// 音高变体不分侧 (读给哪侧就在哪侧变调); 前缀变体与语言绑定, 仅主侧朗读时改写。
enum LetterCaseStyle: String, Codable {
    case none       // 原样朗读
    case prefixZh   // 前缀 "大写": Z → "大写 Z" (仅主侧)
    case prefixEn   // 前缀 "Capital": Z → "Capital Z" (仅主侧)
    case pitch      // 提高音高 (段带 isolatedCapital 标, 渲染级音高增量, 不分侧)

    /// pitch 变体的音高增量 (桥刻度 0...100, 50 = 常音高)
    static let pitchDelta = 15
}

/// 分发规则开关。特殊情况 (网址、型号、缩写、方言混排 …) 优先在这里加开关,
/// 或在 TextSegmenter.route 的判定链里加条目 — 不要把规则散到调用方。
struct SegmentRuleOptions: Codable, Equatable {
    /// 连续英文至少几个词才交给副发音人 (1 = 所有英文短语都切给英文侧)
    var englishMinWords: Int = 1
    /// 孤立单字母 (如 "Z 分组" 的 Z) 交给主发音人, 避免英文音色拼读突兀
    var isolatedLetterToPrimary: Bool = true
    /// 数字一律交给主发音人; false = 左右都被英文夹住时归副发音人
    var digitsToPrimary: Bool = true
    /// 孤立大写字母读给主侧时的辅助方式 (见 LetterCaseStyle)
    var letterCase: LetterCaseStyle = .none

    // 自定义解码: 手改 scheme.json 缺字段时回退默认值, 不整体报废
    init() {}
    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        englishMinWords = try c.decodeIfPresent(Int.self, forKey: .englishMinWords) ?? 1
        isolatedLetterToPrimary = try c.decodeIfPresent(Bool.self, forKey: .isolatedLetterToPrimary) ?? true
        digitsToPrimary = try c.decodeIfPresent(Bool.self, forKey: .digitsToPrimary) ?? true
        letterCase = try c.decodeIfPresent(LetterCaseStyle.self, forKey: .letterCase) ?? .none
    }
}

struct TextSegment: Equatable {
    let text: String
    let target: SegmentTarget
    /// 音高变体的孤立大写字母段: 独立成段、不与同侧邻段合并 (渲染层对该段
    /// 附加音高增量; 同 (engine,voice) 相邻段的启动须串行)
    var isolatedCapital: Bool = false
}

/// 文本分发规则层 — 策略全部集中于此, 便于按特殊情况逐条增改:
///   1. normalize()  — 预处理 (替换/清洗, 目前仅去首尾空白)
///   2. splitRuns()  — 按字符类别切成原子 run (CJK / 拉丁 / 数字 / 中性)
///   3. route()      — 拉丁聚短语判词数、孤立字母、数字归属 → 主/副/待并入;
///                     大小写字母处理 (前缀改写 / 音高变体打标) 也在此步
///   4. attach()     — 中性字符 (空格/标点) 并入前一 run
///   5. merge()      — 相邻同侧合并为输出段 (音高变体段保持独立)
/// 音高变体会产生同侧相邻段 — 渲染层对同 (engine,voice) 段须串行启动。
enum TextSegmenter {

    static func segment(_ raw: String, options: SegmentRuleOptions) -> [TextSegment] {
        let text = normalize(raw)
        guard !text.isEmpty else { return [] }
        let runs = splitRuns(text)
        let routed = route(runs, options: options)
        let attached = attach(routed)
        return merge(attached)
    }

    // MARK: 1. 预处理

    static func normalize(_ s: String) -> String {
        s.trimmingCharacters(in: .whitespacesAndNewlines)
    }

    // MARK: 2. 原子 run

    enum RunClass { case cjk, latin, digit, neutral }

    struct Run {
        let text: String
        let cls: RunClass
    }

    /// 字符类别。可在此加特判 (假名单类、控制字符过滤、专有名词白名单 …)。
    static func classify(_ s: UnicodeScalar) -> RunClass {
        switch s.value {
        case 0x41...0x5A, 0x61...0x7A: return .latin        // ASCII 字母
        case 0x30...0x39: return .digit                     // ASCII 数字
        case 0xFF10...0xFF19: return .digit                 // 全角数字
        case 0xFF01...0xFF5E, 0x3000...0x303F, 0xFE30...0xFE4F:
            return .neutral                                  // 全角符号/中文标点
        default: break
        }
        if s.properties.isIdeographic { return .cjk }
        switch s.value {
        case 0x3040...0x30FF, 0x31F0...0x31FF,              // 假名
             0xAC00...0xD7AF, 0x1100...0x11FF:              // 谚文
            return .cjk
        default: break
        }
        if s.properties.isAlphabetic { return .latin }       // 带音标拉丁等其他字母
        if s.properties.numericType == .decimal { return .digit }
        return .neutral
    }

    static func splitRuns(_ text: String) -> [Run] {
        var out: [Run] = []
        var cur = ""
        var curCls: RunClass? = nil
        for ch in text.unicodeScalars {
            let cls = classify(ch)
            if cls == curCls { cur.unicodeScalars.append(ch); continue }
            if !cur.isEmpty { out.append(Run(text: cur, cls: curCls!)) }
            cur = String(ch)
            curCls = cls
        }
        if !cur.isEmpty { out.append(Run(text: cur, cls: curCls!)) }
        return out
    }

    // MARK: 3. 路由

    struct Routed {
        let text: String
        let cls: RunClass
        let target: SegmentTarget?   // nil = 数字/中性, 数字下一轮定侧
        var isolatedCapital: Bool = false
    }

    /// 拉丁短语内部允许的中性连接符 (Node.js / don't / state-of-the-art)
    static let joiners: Set<Character> = ["-", "'", "’", ".", "·"]

    /// item = 一个拉丁短语 (latin + 连接符 + latin …) 或一个非拉丁 run。
    /// 短语聚合后, 拉丁 item 的相邻 item 必非拉丁 → "孤立" 只看自身形态。
    struct Item {
        let text: String
        let cls: RunClass
        let latinWords: Int
    }

    static func route(_ runs: [Run], options: SegmentRuleOptions) -> [Routed] {
        var items: [Item] = []
        var i = 0
        while i < runs.count {
            let r = runs[i]
            if r.cls != .latin {
                items.append(Item(text: r.text, cls: r.cls, latinWords: 0))
                i += 1
                continue
            }
            var text = r.text
            var words = 1
            var j = i + 1
            while j < runs.count {
                if runs[j].cls == .latin {
                    text += runs[j].text
                    words += 1
                    j += 1
                } else if runs[j].cls == .neutral,
                          j + 1 < runs.count, runs[j + 1].cls == .latin,
                          !runs[j].text.isEmpty,
                          runs[j].text.allSatisfy({ joiners.contains($0) }) {
                    text += runs[j].text
                    j += 1
                } else { break }
            }
            items.append(Item(text: text, cls: .latin, latinWords: words))
            i = j
        }

        // 第一轮: CJK/拉丁定侧 (不依赖邻居)
        var targets: [SegmentTarget?] = Array(repeating: nil, count: items.count)
        for k in items.indices {
            switch items[k].cls {
            case .cjk:
                targets[k] = .primary
            case .latin:
                let isolated = items[k].latinWords == 1
                    && items[k].text.unicodeScalars.count == 1
                if options.isolatedLetterToPrimary && isolated {
                    targets[k] = .primary
                } else {
                    targets[k] = items[k].latinWords >= options.englishMinWords ? .secondary : .primary
                }
            case .digit, .neutral:
                break
            }
        }
        // 第二轮: 数字定侧 (看两侧已定侧内容)
        for k in items.indices where items[k].cls == .digit {
            if options.digitsToPrimary {
                targets[k] = .primary
            } else {
                let prev = targets[..<k].reversed().first { $0 != nil } ?? nil
                let next = targets[(k + 1)...].first { $0 != nil } ?? nil
                targets[k] = (prev == .secondary && next == .secondary) ? .secondary : .primary
            }
        }
        /* 大小写字母处理: 孤立的单个大写字母 (ASCII A-Z) 时生效 —
         * 音高变体不分侧 (读给哪侧就在哪侧变调, VoiceOver 式资本提示);
         * 前缀变体与措辞语言绑定, 仅主侧朗读时改写文本 */
        return items.enumerated().map { i, item -> Routed in
            var text = item.text
            var cap = false
            if item.cls == .latin, item.latinWords == 1, item.text.unicodeScalars.count == 1,
               let ch = item.text.unicodeScalars.first, (0x41...0x5A).contains(ch.value) {
                if options.letterCase == .pitch {
                    cap = true
                } else if targets[i] == .primary {
                    switch options.letterCase {
                    case .none, .pitch: break
                    case .prefixZh: text = "大写 " + text
                    case .prefixEn: text = "Capital " + text
                    }
                }
            }
            return Routed(text: text, cls: item.cls, target: targets[i], isolatedCapital: cap)
        }
    }

    // MARK: 4. 中性并入

    static func attach(_ routed: [Routed]) -> [Routed] {
        var out: [Routed] = []
        for r in routed {
            if r.target != nil {
                out.append(r)
            } else if var last = out.last, !last.isolatedCapital {
                last = Routed(text: last.text + r.text, cls: last.cls, target: last.target,
                              isolatedCapital: last.isolatedCapital)
                out[out.count - 1] = last
            }
            // 音高变体的孤立字母段不吸收后随中性字符 (至多空白/标点, 丢弃),
            // 保持"单字母段"纯净; 开头未定侧的中性同理丢弃 (normalize 已去首尾)
        }
        return out
    }

    // MARK: 5. 合并

    static func merge(_ routed: [Routed]) -> [TextSegment] {
        var out: [TextSegment] = []
        for r in routed {
            guard let t = r.target, !r.text.isEmpty else { continue }
            if r.isolatedCapital {
                out.append(TextSegment(text: r.text, target: t, isolatedCapital: true))
                continue
            }
            if let last = out.last, last.target == t, !last.isolatedCapital {
                out[out.count - 1] = TextSegment(text: last.text + r.text, target: t)
            } else {
                out.append(TextSegment(text: r.text, target: t))
            }
        }
        return out
    }

    /// 同侧强制后重并 (单语音模式 useSecondary=false 时): 相邻段合回一段;
    /// 音高变体的孤立字母段仍保持独立。
    static func coalesce(_ segs: [TextSegment]) -> [TextSegment] {
        var out: [TextSegment] = []
        for s in segs {
            if let last = out.last, last.target == s.target,
               !last.isolatedCapital, !s.isolatedCapital {
                out[out.count - 1] = TextSegment(text: last.text + s.text, target: s.target)
            } else {
                out.append(s)
            }
        }
        return out
    }
}
