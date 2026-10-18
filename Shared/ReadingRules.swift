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

    /// 扩展侧入口: 每请求调用, 文件未变化时直接返回缓存的编译结果
    static func shared() -> RuleBook {
        let date = (try? url.resourceValues(forKeys: [.contentModificationDateKey]))?
            .contentModificationDate
        lock.lock(); defer { lock.unlock() }
        if let c = cache, c.0 == date { return c.1 }
        let book = RuleBook(load())
        cache = (date, book)
        return book
    }
}
