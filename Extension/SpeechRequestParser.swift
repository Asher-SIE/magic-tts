import Foundation

/// SSML 请求解析 — 系统以 W3C SSML 下发文本, 内含 prosody (语速/音高) 与 break 等标签.
/// 对应 Android 版: 纯文本 + rate/pitch 参数 (0..500 / 0..100 刻度).
struct SpeechRequestParser {

    struct Parsed {
        var text: String
        var rate: Int    // 0..500, 100 = 常速 (Android "rate" 偏好同刻度)
        var pitch: Int   // 0..100, 50 = 常音高 (Android "pitch" 同刻度)
    }

    static func parse(_ ssml: String) -> Parsed {
        var rate = 100
        var pitch = 50

        // <prosody rate="150%" / rate="369.99997%"> → VO 送小数百分号, 统一提取
        // (正则进程级缓存 — 每请求重编译是热路径纯浪费)
        if let r = firstMatch(ssml, pattern: "rate=\"([0-9.]+)%\""), let v = Double(r) {
            rate = min(500, max(20, Int(v.rounded())))
        } else if let r = firstMatch(ssml, pattern: "rate=\"([0-9.]+)\""), let v = Double(r) {
            // SSML 也允许非百分比数值 (字/秒), 最简版按比例处理
            rate = min(500, max(20, Int((v * 10).rounded())))
        }
        if let p = firstMatch(ssml, pattern: "pitch=\"\\+?(-?[0-9]+)%\"") {
            let delta = Int(p) ?? 0
            pitch = min(100, max(0, 50 + delta / 2))
        }

        // VO 大写宣布恢复: 字符/单词导航时 VO 把大写字母小写化, 并用
        // pitch 正增量 span 标记 (宿主小写化通道的补充). 把 span 内文本
        // (标签外字符) 恢复为大写 → 切段器可按大小写触发处理; 最大 span
        // 增量并入全局 pitch (普通语音路径照常跟随请求 prosody).
        let work = recoverCapitalSpans(ssml, pitchOut: &pitch)

        // 抽取纯文本: 去 <speak>/<voice>/<prosody>/<break>/... 标签, 还原实体
        var text = work
        text = text.replacingOccurrences(of: "<[^>]+>", with: " ", options: .regularExpression)
        text = text
            .replacingOccurrences(of: "&amp;", with: "&")
            .replacingOccurrences(of: "&lt;", with: "<")
            .replacingOccurrences(of: "&gt;", with: ">")
            .replacingOccurrences(of: "&quot;", with: "\"")
            .replacingOccurrences(of: "&apos;", with: "'")
        let collapsed = text.split(whereSeparator: \.isWhitespace).joined(separator: " ")
        return Parsed(text: String(collapsed), rate: rate, pitch: pitch)
    }

    /// 嵌套的正 pitch <prosody> span (VO 资本标记恒在内层; 顶层 prosody 是
    /// 全局请求不处理): span 内标签外文本大写化, 返回改写后的 SSML;
    /// pitchOut 传入基准 pitch, 取最大 span 增量按 50+delta/2 折算后写回。
    private static func recoverCapitalSpans(_ ssml: String, pitchOut: inout Int) -> String {
        guard let open = try? NSRegularExpression(pattern: "<prosody\\b[^>]*pitch=\"\\+([0-9.]+)%\"[^>]*>"),
              let anyOpen = try? NSRegularExpression(pattern: "<prosody\\b[^>]*>"),
              let close = try? NSRegularExpression(pattern: "</prosody>") else { return ssml }
        let full = NSRange(ssml.startIndex..., in: ssml)

        // 事件流 (开/闭标签位置) → 栈配对出各 span 的区间与深度
        var events: [(pos: Int, isOpen: Bool, boost: Double?)] = []
        for m in anyOpen.matches(in: ssml, range: full) {
            let tag = (ssml as NSString).substring(with: m.range)
            var boost: Double?
            if let mm = open.firstMatch(in: tag, range: NSRange(tag.startIndex..., in: tag)),
               let r = Range(mm.range(at: 1), in: tag) {
                boost = Double(tag[r]) ?? 0
            }
            events.append((m.range.location, true, boost))
        }
        for m in close.matches(in: ssml, range: full) {
            events.append((m.range.location + m.range.length, false, nil))
        }
        events.sort { $0.pos < $1.pos }

        var stack: [(start: String.Index, boost: Double?)] = []
        var edits: [(Range<String.Index>, String)] = []
        for e in events {
            if e.isOpen {
                stack.append((ssml.index(ssml.startIndex, offsetBy: e.pos), e.boost))
            } else if !stack.isEmpty {
                let top = stack.removeLast()
                let endIdx = ssml.index(ssml.startIndex, offsetBy: e.pos)
                // 深度 ≥1 (嵌套) 且正增量: 资本标记 span
                if !stack.isEmpty, let b = top.boost, b > 0.01 {
                    let content = String(ssml[top.start..<endIdx])
                    var out = ""
                    var inTag = false
                    var inEntity = false
                    for ch in content {
                        if ch == "<" { inTag = true; out.append(ch); continue }
                        if ch == ">" { inTag = false; out.append(ch); continue }
                        if ch == "&" { inEntity = true; out.append(ch); continue }
                        if ch == ";" { inEntity = false; out.append(ch); continue }
                        out += (inTag || inEntity) ? String(ch) : String(ch).uppercased()
                    }
                    edits.append((top.start..<endIdx, out))
                    pitchOut = max(pitchOut, min(100, 50 + Int(b) / 2))
                }
            }
        }
        guard !edits.isEmpty else { return ssml }
        var out = ssml
        for (r, s) in edits.reversed() { out.replaceSubrange(r, with: s) }
        return out
    }

    private static var regexCache: [String: NSRegularExpression] = [:]
    private static let regexLock = NSLock()

    private static func firstMatch(_ s: String, pattern: String) -> String? {
        let re: NSRegularExpression? = regexLock.withLock {
            if let cached = regexCache[pattern] { return cached }
            guard let compiled = try? NSRegularExpression(pattern: pattern) else { return nil }
            regexCache[pattern] = compiled
            return compiled
        }
        guard let re,
              let m = re.firstMatch(in: s, range: NSRange(s.startIndex..., in: s)),
              m.numberOfRanges > 1,
              let r = Range(m.range(at: 1), in: s) else { return nil }
        return String(s[r])
    }
}
