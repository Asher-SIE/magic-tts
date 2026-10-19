import Foundation

/// 朗读替换规则: 朗读前把「查找」替换为「替换为」, 修正多音字、英文缩写读法、
/// 数字符号等。普通文本按字面匹配 (可选整词/忽略大小写); 正则模式用 ICU 语法,
/// 替换串里可用 $1/$2 引用捕获组。
struct ReadingRule: Codable, Identifiable, Equatable {
    var id = UUID()
    var enabled = true
    var find: String
    var replace: String
    var regex = false
    var ignoreCase = false
    /// 仅普通文本模式: 只匹配完整英文单词/数字, 相邻中文字符不算词的一部分
    /// ("live" 不会命中 "deliver", 但会命中 "看live直播")
    var wholeWord = false
    var note = ""

    init(find: String, replace: String, regex: Bool = false, ignoreCase: Bool = false,
         wholeWord: Bool = false, note: String = "", enabled: Bool = true) {
        self.find = find
        self.replace = replace
        self.regex = regex
        self.ignoreCase = ignoreCase
        self.wholeWord = wholeWord
        self.note = note
        self.enabled = enabled
    }

    enum CodingKeys: String, CodingKey {
        case id, enabled, find, replace, regex, ignoreCase, wholeWord, note
    }

    init(from decoder: Decoder) throws {
        // 手改 rules.json 容错: 缺字段回退默认
        let c = try decoder.container(keyedBy: CodingKeys.self)
        id = try c.decodeIfPresent(UUID.self, forKey: .id) ?? UUID()
        enabled = try c.decodeIfPresent(Bool.self, forKey: .enabled) ?? true
        find = try c.decode(String.self, forKey: .find)
        replace = try c.decodeIfPresent(String.self, forKey: .replace) ?? ""
        regex = try c.decodeIfPresent(Bool.self, forKey: .regex) ?? false
        ignoreCase = try c.decodeIfPresent(Bool.self, forKey: .ignoreCase) ?? false
        wholeWord = try c.decodeIfPresent(Bool.self, forKey: .wholeWord) ?? false
        note = try c.decodeIfPresent(String.self, forKey: .note) ?? ""
    }

    enum RuleError: Error, Equatable {
        case emptyFind
        case badPattern

        var message: String {
            switch self {
            case .emptyFind: return "查找内容为空"
            case .badPattern: return "正则表达式无效"
            }
        }
    }

    /// 编译结果; 失败给原因 (编辑页展示, 朗读时跳过该条)
    func compiled() -> Result<NSRegularExpression, RuleError> {
        guard !find.isEmpty else { return .failure(.emptyFind) }
        var pattern = regex ? find : NSRegularExpression.escapedPattern(for: find)
        if !regex && wholeWord {
            pattern = "(?<![A-Za-z0-9])" + pattern + "(?![A-Za-z0-9])"
        }
        guard let re = try? NSRegularExpression(
            pattern: pattern, options: ignoreCase ? [.caseInsensitive] : []) else {
            return .failure(.badPattern)
        }
        return .success(re)
    }

    /// 替换模板: 正则模式原样 (可用 $1), 普通文本转义 $ 防误当组引用
    var template: String {
        regex ? replace : NSRegularExpression.escapedTemplate(for: replace)
    }

    /// 示例规则 (默认停用): 展示普通/整词/正则三种用法
    static let examples: [ReadingRule] = [
        ReadingRule(find: "重新", replace: "虫新",
                    note: "多音字: 让「重」读 chóng", enabled: false),
        ReadingRule(find: "iOS", replace: "eye OS", ignoreCase: true, wholeWord: true,
                    note: "英文缩写整词读法", enabled: false),
        ReadingRule(find: "(\\d+(?:\\.\\d+)?)%", replace: "百分之$1", regex: true,
                    note: "正则: 50% 读「百分之50」", enabled: false),
    ]

    /// 内置默认替换: 在用户规则之后兜底应用 (见 RuleStore.shared)。两类: 同音改写
    /// 修正引擎误读的多音字 (引擎读对时听感不变); 符号读法把引擎丢弃的连字符按
    /// 语境改写为汉字。始终生效、不进 rules.json。连字符按序判定, 前条不改写的
    /// 留给后条: 日期 → 年份区间(至) → 数值区间(至) → 减法(减) → 负号(负)。
    static let builtin: [ReadingRule] = [
        // 「率」lǜ 义词 (圆周率/效率/概率…) 引擎易误读 shuài, 改写同音「律」;
        // 读 shuài 的排除: 坦/轻/直/草/表/大 + 率, 以及 率先/领/真/性/尔/土 开头
        ReadingRule(find: "(?<![坦轻直草表大])率(?!先|领|真|性|尔|土)", replace: "律", regex: true,
                    note: "多音字: 「率」读 lǜ 的词改写同音「律」; 坦率/率先/率领 等读 shuài 的不动"),
        // 日期连写读「年月日」; 先于减法规则, 免得 2026-10-19 被读成「减」
        ReadingRule(find: "(?<![0-9.])(\\d{4})[-−－](\\d{1,2})[-−－](\\d{1,2})(?![0-9.])",
                    replace: "$1年$2月$3日", regex: true,
                    note: "符号读法: 2026-10-19 读「2026年10月19日」(引擎丢弃连字符)"),
        // 年份区间读「至」; 后接续算符是算式, 留给减法规则
        ReadingRule(find: "(?<![0-9.])(\\d{4})[-−－](\\d{4})(?![0-9.=＝+×÷*/-])",
                    replace: "$1至$2", regex: true,
                    note: "符号读法: 2024-2025 读「2024至2025」; 后接 = × ÷ 等续算符时按减法处理"),
        // 数值区间读「至」: 后接中文量词或百分号 (3-5天/第2-4章/10%-20%);
        // 后接 加减乘除等差得/的差 是算式行文, 留给减法规则
        ReadingRule(find: "([0-9%％])\\s*[-−－]\\s*(\\d+)(?=[\\u4e00-\\u9fff%％])(?![加减乘除等差得]|的差)",
                    replace: "$1至$2", regex: true,
                    note: "符号读法: 后接量词的数字区间读「至」 (3-5天/10%-20%); 算式行文 (10-3等于7) 不动"),
        // 减法: 数字/右括号后的连字符读「减」, 含两侧带空格的 10 - 3
        ReadingRule(find: "([0-9)）])\\s*[-−－]\\s*(?=\\d)",
                    replace: "$1减", regex: true,
                    note: "符号读法: 运算场景 10-3=7、(4×2)-3 读「减」"),
        // 负号: 其余紧贴数字的连字符读「负」 (行首 -5度、中文后 气温-5度、算符后 3×(-5));
        // 字母后不动 (COVID-19/B-52), 带空格的「- 5」是列表项也不动
        ReadingRule(find: "(?<![0-9A-Za-z%％)）])[-−－](?=\\d)",
                    replace: "负", regex: true,
                    note: "符号读法: 读数场景的负号 -5度/3×(-5) 读「负」; 字母后 (COVID-19) 与空格列表项不动"),
    ]
}

/// 启用且编译通过的规则按序就绪; 依次应用, 后一条看到前一条的结果。
struct RuleBook {
    private let compiled: [(NSRegularExpression, String)]

    init(_ rules: [ReadingRule]) {
        compiled = rules.filter(\.enabled).compactMap { r in
            guard case .success(let re) = r.compiled() else { return nil }
            return (re, r.template)
        }
    }

    var isEmpty: Bool { compiled.isEmpty }

    func apply(_ text: String) -> String {
        var t = text
        for (re, template) in compiled {
            t = re.stringByReplacingMatches(in: t, range: NSRange(t.startIndex..., in: t),
                                            withTemplate: template)
        }
        return t
    }
}

/// 规则存取: App 写 rules.json, 扩展每请求读。与 scheme.json 同目录同模式:
/// shared() 按文件修改时间缓存, App 改完规则即时生效, 无需重装扩展。
enum RuleStore {
    static var url: URL { MagicPaths.enginesRoot.appendingPathComponent("rules.json") }

    static func load() -> [ReadingRule] {
        guard let data = try? Data(contentsOf: url),
              let rules = try? JSONDecoder().decode([ReadingRule].self, from: data) else { return [] }
        return rules
    }

    @discardableResult
    static func save(_ rules: [ReadingRule]) -> Bool {
        let enc = JSONEncoder()
        enc.outputFormatting = [.prettyPrinted, .sortedKeys]
        guard let data = try? enc.encode(rules) else { return false }
        return (try? data.write(to: url, options: [.atomic])) != nil
    }

    private static let lock = NSLock()
    private static var cache: (Date?, RuleBook)?

    /// 扩展侧入口: 每请求调用, 文件未变化时直接返回缓存的编译结果。
    /// 用户规则先应用, 内置默认替换 (ReadingRule.builtin) 在其结果上兜底:
    /// 用户按原文匹配优先, 用户规则产出的文本也会被兜底修正。
    static func shared() -> RuleBook {
        let date = (try? url.resourceValues(forKeys: [.contentModificationDateKey]))?
            .contentModificationDate
        lock.lock(); defer { lock.unlock() }
        if let c = cache, c.0 == date { return c.1 }
        let book = RuleBook(load() + ReadingRule.builtin)
        cache = (date, book)
        return book
    }
}
