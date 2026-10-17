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

        // <prosody rate="150%" pitch="+20%"> → 提取百分比
        if let r = firstMatch(ssml, pattern: "rate=\"([0-9]+)%\"") {
            rate = min(500, max(20, Int(r) ?? 100))
        } else if let r = firstMatch(ssml, pattern: "rate=\"([0-9.]+)\"") {
            // SSML 也允许非百分比数值 (字/秒), 最简版按比例处理
            if let v = Double(r) { rate = min(500, max(20, Int(v * 100 / 10))) }
        }
        if let p = firstMatch(ssml, pattern: "pitch=\"\\+?(-?[0-9]+)%\"") {
            let delta = Int(p) ?? 0
            pitch = min(100, max(0, 50 + delta / 2))
        }

        // 抽取纯文本: 去 <speak>/<voice>/<prosody>/<break>/... 标签, 还原实体
        var text = ssml
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

    private static func firstMatch(_ s: String, pattern: String) -> String? {
        guard let re = try? NSRegularExpression(pattern: pattern),
              let m = re.firstMatch(in: s, range: NSRange(s.startIndex..., in: s)),
              m.numberOfRanges > 1,
              let r = Range(m.range(at: 1), in: s) else { return nil }
        return String(s[r])
    }
}
