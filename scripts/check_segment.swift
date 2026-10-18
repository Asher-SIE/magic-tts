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

        // —— 字母串拼读 (ABCabc 类) ——
        // 7. 拼读拆分: none 变体下逐字母空格化, 合并单段走副侧
        opt.letterCase = .none
        let s7 = TextSegmenter.segment("ABCabc", options: opt)
        expect("拼读none单段",
               s7 == [TextSegment(text: "A B C a b c", target: .secondary)],
               "\(s7)")

        // 8. 拼读 + Capital 前缀: 大写逐字母加前缀, 合并单段
        opt.letterCase = .prefixEn
        let s8 = TextSegmenter.segment("ABCabc", options: opt)
        expect("拼读Capital",
               s8 == [TextSegment(text: "Capital A Capital B Capital C a b c", target: .secondary)],
               "\(s8)")

        // 9. 拼读 + 音高: 三个大写各成独立段, 小写合并
        opt.letterCase = .pitch
        let s9 = TextSegmenter.segment("ABCabc", options: opt)
        expect("拼读音高",
               s9.count == 4
               && s9[0] == TextSegment(text: "A", target: .secondary, isolatedCapital: true)
               && s9[1] == TextSegment(text: "B", target: .secondary, isolatedCapital: true)
               && s9[2] == TextSegment(text: "C", target: .secondary, isolatedCapital: true)
               && s9[3] == TextSegment(text: "a b c", target: .secondary),
               "\(s9)")

        // 10. 词形保护: 驼峰/词尾大写/全大写不拆
        opt.letterCase = .prefixEn
        for w in ["iPhone", "macOS", "NVDA", "GitHub", "LaTeX"] {
            let s10 = TextSegmenter.segment("用 \(w) 好", options: opt)
            expect("词形不拆\(w)",
                   s10.count == 3 && s10[1].text == "\(w) " && s10[1].target == .secondary
                   && !s10[1].isolatedCapital,
                   "\(s10)")
        }

        // 11. 拼读开关关闭: ABCabc 回到整词
        opt.spellMixedRuns = false
        let s11 = TextSegmenter.segment("ABCabc", options: opt)
        expect("拼读可关", s11 == [TextSegment(text: "ABCabc", target: .secondary)], "\(s11)")
        opt.spellMixedRuns = true

        // 12. 多字母词不算孤立字母
        opt.isolatedLetterToPrimary = true
        let s12 = TextSegmenter.segment("文件 ABC 处理", options: opt)
        expect("多字母不处理",
               s12.count == 3 && s12[1].text == "ABC " && s12[1].target == .secondary,
               "\(s12)")

        // 13. 连接符聚合 (A.txt) 不算孤立字母
        let s13 = TextSegmenter.segment("文件 A.txt 处理", options: opt)
        expect("连接符不处理",
               s13.count == 3 && s13[1].text == "A.txt " && s13[1].target == .secondary,
               "\(s13)")

        // 14. 两个相邻孤立大写字母 (音高): 各自成段不并
        opt.letterCase = .pitch
        let s14 = TextSegmenter.segment("Z A", options: opt)
        expect("相邻两字母独立",
               s14.count == 2
               && s14[0] == TextSegment(text: "Z", target: .primary, isolatedCapital: true)
               && s14[1] == TextSegment(text: "A", target: .primary, isolatedCapital: true),
               "\(s14)")

        // 15. coalesce: 单语音模式下同侧重并, 音高段保持独立
        let s15src = TextSegmenter.segment("中文 English 中文 字母 Z", options: opt)
        let s15 = TextSegmenter.coalesce(s15src.map {
            TextSegment(text: $0.text, target: .primary, isolatedCapital: $0.isolatedCapital)
        })
        expect("coalesce重并",
               s15.count == 2
               && s15[0] == TextSegment(text: "中文 English 中文 字母 ", target: .primary)
               && s15[1] == TextSegment(text: "Z", target: .primary, isolatedCapital: true),
               "\(s15)")

        // 16. 普通混排 (无孤立字母) coalesce 成单段
        let s16src = TextSegmenter.segment("中文 English 中文", options: opt)
        let s16 = TextSegmenter.coalesce(s16src.map { TextSegment(text: $0.text, target: .primary) })
        expect("coalesce全并", s16 == [TextSegment(text: "中文 English 中文", target: .primary)],
               "\(s16)")

        // 17. 空文本
        expect("空文本", TextSegmenter.segment("", options: opt).isEmpty)

        if g_fail == 0 {
            print("全部通过")
        } else {
            print("\(g_fail) 项失败")
            exit(1)
        }
    }
}
