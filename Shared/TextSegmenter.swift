import Foundation

// MARK: - 分发目标

enum SegmentTarget {
    case primary    // 主发音人 (默认中文)
    case secondary  // 副发音人 (默认英文)
}

// MARK: - 规则参数 (随 scheme.json 持久化)

/// 分发规则开关。特殊情况 (网址、型号、缩写、方言混排 …) 优先在这里加开关,
/// 或在 TextSegmenter.route 的判定链里加条目 — 不要把规则散到调用方。
struct SegmentRuleOptions: Codable, Equatable {
    /// 连续英文至少几个词才交给副发音人 (1 = 所有英文短语都切给英文侧)
    var englishMinWords: Int = 1
    /// 孤立单字母 (如 "Z 分组" 的 Z) 交给主发音人, 避免英文音色拼读突兀
    var isolatedLetterToPrimary: Bool = true
    /// 数字一律交给主发音人; false = 左右都被英文夹住时归副发音人
    var digitsToPrimary: Bool = true

    // 自定义解码: 手改 scheme.json 缺字段时回退默认值, 不整体报废
    init() {}
    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        englishMinWords = try c.decodeIfPresent(Int.self, forKey: .englishMinWords) ?? 1
        isolatedLetterToPrimary = try c.decodeIfPresent(Bool.self, forKey: .isolatedLetterToPrimary) ?? true
        digitsToPrimary = try c.decodeIfPresent(Bool.self, forKey: .digitsToPrimary) ?? true
    }
}

struct TextSegment: Equatable {
    let text: String
    let target: SegmentTarget
}

/// 文本分发规则层 — 策略全部集中于此, 便于按特殊情况逐条增改:
///   1. normalize()  — 预处理 (替换/清洗, 目前仅去首尾空白)
///   2. splitRuns()  — 按字符类别切成原子 run (CJK / 拉丁 / 数字 / 中性)
///   3. route()      — 拉丁聚短语判词数、孤立字母、数字归属 → 主/副/待并入
///   4. attach()     — 中性字符 (空格/标点) 并入前一 run
///   5. merge()      — 相邻同侧合并为输出段
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
        return items.enumerated().map { Routed(text: $1.text, cls: $1.cls, target: targets[$0]) }
    }

    // MARK: 4. 中性并入

    static func attach(_ routed: [Routed]) -> [Routed] {
        var out: [Routed] = []
        for r in routed {
            if r.target != nil {
                out.append(r)
            } else if var last = out.last {
                last = Routed(text: last.text + r.text, cls: last.cls, target: last.target)
                out[out.count - 1] = last
            }
            // 开头未定侧的中性: 丢弃 (normalize 已去首尾空白, 至多是标点)
        }
        return out
    }

    // MARK: 5. 合并

    static func merge(_ routed: [Routed]) -> [TextSegment] {
        var out: [TextSegment] = []
        for r in routed {
            guard let t = r.target, !r.text.isEmpty else { continue }
            if let last = out.last, last.target == t {
                out[out.count - 1] = TextSegment(text: last.text + r.text, target: t)
            } else {
                out.append(TextSegment(text: r.text, target: t))
            }
        }
        return out
    }
}
