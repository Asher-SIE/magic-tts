/* TextSegmenter 断言: 大小写字母处理 (前缀/音高变体) + 字母串拼读 + 单语音 coalesce.
 *
 * 编译:
 *   xcrun swiftc -swift-version 5 scripts/check_segment.swift \
 *     Shared/TextSegmenter.swift -o /tmp/check_segment
 */
import Foundation

var g_fail = 0
func expect(_ name: String, _ cond: Bool, _ detail: String = "") {
    print(cond ? "通过  \(name)" : "失败  \(name)  \(detail)")
    if !cond { g_fail += 1 }
}

@main
enum CheckSegmentMain {
    static func main() {
        var opt = SegmentRuleOptions()
        opt.isolatedLetterToPrimary = true   // 1-5: 字母归主侧场景

        // 1. 无处理: 孤立大写 Z 归主侧并合并成单段 (既有行为不变)
        opt.letterCase = .none
        let s1 = TextSegmenter.segment("字母 Z 分组", options: opt)
        expect("无处理单段", s1 == [TextSegment(text: "字母 Z 分组", target: .primary)],
               "\(s1)")

        // 2. 前缀「大写」: 文本改写, 仍合并单段
        opt.letterCase = .prefixZh
        let s2 = TextSegmenter.segment("字母 Z 分组", options: opt)
        expect("前缀大写", s2 == [TextSegment(text: "字母 大写 Z 分组", target: .primary)],
               "\(s2)")

        // 3. 前缀「Capital」
        opt.letterCase = .prefixEn
        let s3 = TextSegmenter.segment("字母 Z 分组", options: opt)
        expect("前缀Capital", s3 == [TextSegment(text: "字母 Capital Z 分组", target: .primary)],
               "\(s3)")

        // 4. 音高变体: 孤立字母独立成段并打标, 邻段不并
        opt.letterCase = .pitch
        let s4 = TextSegmenter.segment("字母 Z 分组", options: opt)
        expect("音高变体三段",
               s4.count == 3
               && s4[0] == TextSegment(text: "字母 ", target: .primary)
               && s4[1] == TextSegment(text: "Z", target: .primary, isolatedCapital: true)
               && s4[2] == TextSegment(text: "分组", target: .primary),
               "\(s4)")

        // 5. 小写字母不受处理
        let s5 = TextSegmenter.segment("字母 z 分组", options: opt)
        expect("小写不受处理", s5 == [TextSegment(text: "字母 z 分组", target: .primary)],
               "\(s5)")

        // 6. 字母归副侧时前缀同样生效 (措辞由用户按朗读侧语言选)
        opt.letterCase = .prefixZh
        opt.isolatedLetterToPrimary = false
        let s6 = TextSegmenter.segment("字母 Z 分组", options: opt)
        expect("副侧也前缀",
               s6.count == 3 && s6[1] == TextSegment(text: "大写 Z ", target: .secondary)
               && !s6[1].isolatedCapital,
               "\(s6)")

        // 6b. 音高变体不分侧: 字母归副侧也打标
        opt.letterCase = .pitch
        let s6b = TextSegmenter.segment("字母 Z 分组", options: opt)
        expect("音高副侧也生效",
               s6b.count == 3 && s6b[1] == TextSegment(text: "Z", target: .secondary, isolatedCapital: true),
               "\(s6b)")

        // 7. 多字母词不算孤立字母
        opt.letterCase = .prefixEn
        opt.isolatedLetterToPrimary = true
        let s7 = TextSegmenter.segment("文件 ABC 处理", options: opt)
        expect("多字母不处理",
               s7.count == 3 && s7[1].text == "ABC " && s7[1].target == .secondary,
               "\(s7)")

        // 8. 连接符聚合 (A.txt) 不算孤立字母
        let s8 = TextSegmenter.segment("文件 A.txt 处理", options: opt)
        expect("连接符不处理",
               s8.count == 3 && s8[1].text == "A.txt " && s8[1].target == .secondary,
               "\(s8)")

        // 9. 两个相邻孤立大写字母 (音高): 各自成段不并
        opt.letterCase = .pitch
        let s9 = TextSegmenter.segment("Z A", options: opt)
        expect("相邻两字母独立",
               s9.count == 2
               && s9[0] == TextSegment(text: "Z", target: .primary, isolatedCapital: true)
               && s9[1] == TextSegment(text: "A", target: .primary, isolatedCapital: true),
               "\(s9)")

        // 10. coalesce: 单语音模式下同侧重并, 音高段保持独立
        let s10src = TextSegmenter.segment("中文 English 中文 字母 Z", options: opt)
        let s10 = TextSegmenter.coalesce(s10src.map {
            TextSegment(text: $0.text, target: .primary, isolatedCapital: $0.isolatedCapital)
        })
        expect("coalesce重并",
               s10.count == 2
               && s10[0] == TextSegment(text: "中文 English 中文 字母 ", target: .primary)
               && s10[1] == TextSegment(text: "Z", target: .primary, isolatedCapital: true),
               "\(s10)")

        // 11. 普通混排 (无孤立字母) coalesce 成单段
        let s11src = TextSegmenter.segment("中文 English 中文", options: opt)
        let s11 = TextSegmenter.coalesce(s11src.map { TextSegment(text: $0.text, target: .primary) })
        expect("coalesce全并", s11 == [TextSegment(text: "中文 English 中文", target: .primary)],
               "\(s11)")

        // —— 编程标识符拆词 (normalize 内置) ——
        // 13. 拆词矩阵: 驼峰/下划线/连字符/数字边界
        let m: [(String, String)] = [
            ("AppleConnectMenu", "Apple Connect Menu"),
            ("user_name", "user name"),
            ("my-cool-var", "my cool var"),
            ("HTTPServer", "HTTP Server"),
            ("iOS12", "i OS 12"),
            ("mp3player", "mp 3 player"),
            ("state-of-the-art", "state of the art"),
            ("__init__", " init "),            ("NVDA", "NVDA"),
            ("A.txt", "A.txt"),
            ("2024-2025", "2024-2025"),
            ("-5", "-5"),
            ("你好世界", "你好世界"),
        ]
        for (src, want) in m {
            let got = TextSegmenter.splitIdentifiers(src)
            expect("拆词 \(src)", got == want, "得到 \"\(got)\"")
        }

        // 14. 拆词进切段: "AppleConnectMenu 状态菜单" → 英文词组 + 中文两段
        opt.letterCase = .none
        let s14 = TextSegmenter.segment("AppleConnectMenu 状态菜单", options: opt)
        expect("拆词切段",
               s14 == [TextSegment(text: "Apple Connect Menu ", target: .secondary),
                       TextSegment(text: "状态菜单", target: .primary)],
               "\(s14)")

        // 15. 空文本
        expect("空文本", TextSegmenter.segment("", options: opt).isEmpty)

        if g_fail == 0 {
            print("全部通过")
        } else {
            print("\(g_fail) 项失败")
            exit(1)
        }
    }
}
