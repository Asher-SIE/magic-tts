/* SpeechRequestParser 断言: VO 资本 span 恢复 + 既有解析回归.
 * 用例取自 2026-10-18 latency 探针抓到的真实 VO 请求.
 *
 * 编译:
 *   xcrun swiftc -swift-version 5 scripts/check_parser.swift \
 *     Extension/SpeechRequestParser.swift -o /tmp/check_parser
 */
import Foundation

var g_fail = 0
func expect(_ name: String, _ cond: Bool, _ detail: String = "") {
    print(cond ? "通过  \(name)" : "失败  \(name)  \(detail)")
    if !cond { g_fail += 1 }
}

@main
enum CheckParserMain {
    static func main() {
        let wrap = { (inner: String) -> String in
            "<speak><prosody pitch=\"+0.0%\" rate=\"369.99997%\" volume=\"-26.020601dB\">"
            + "<lang xml:lang=\"cmn\"><voice name=\"\">\(inner)</voice></lang></prosody></speak>"
        }

        // 1. VO 大写字母导航 (探针实录): 字母小写 + pitch 正增量 span → 恢复大写
        let p1 = SpeechRequestParser.parse(wrap(
            "<prosody pitch=\"+50.0%\"><say-as interpret-as=\"characters\">c</say-as></prosody>"))
        expect("资本span恢复大写", p1.text == "C", "\"\(p1.text)\"")
        expect("span增量并入pitch", p1.pitch == 75, "\(p1.pitch)")

        // 2. VO 小写字母导航 (探针实录): 无 span → 原样
        let p2 = SpeechRequestParser.parse(wrap(
            "<say-as interpret-as=\"characters\">c</say-as>"))
        expect("小写不动", p2.text == "c" && p2.pitch == 50, "\"\(p2.text)\" pitch=\(p2.pitch)")

        // 3. 普通文本 (探针实录): 全局 prosody +0% 无 span
        let p3 = SpeechRequestParser.parse(wrap("编辑文本"))
        expect("普通文本", p3.text == "编辑文本" && p3.pitch == 50 && p3.rate == 370,
               "\(p3.text) \(p3.pitch) \(p3.rate)")

        // 4. 顶层正 pitch (全局请求, 非资本标记): 不大写化, 但 pitch 跟随
        let p4 = SpeechRequestParser.parse(
            "<speak><prosody pitch=\"+20%\" rate=\"100%\">hello</prosody></speak>")
        expect("顶层pitch跟随", p4.text == "hello" && p4.pitch == 60, "\"\(p4.text)\" \(p4.pitch)")

        // 5. say 样式: 果 z 味 (宿主已小写化, 无 span) → 原样
        let p5 = SpeechRequestParser.parse(wrap("果 z 味"))
        expect("宿主小写无span不动", p5.text == "果 z 味" && p5.pitch == 50, "\"\(p5.text)\"")

        // 6. span 内多字符 (词导航): 全部大写化
        let p6 = SpeechRequestParser.parse(wrap(
            "<prosody pitch=\"+50.0%\"><say-as interpret-as=\"characters\">abc</say-as></prosody>"))
        expect("span多字符全大写", p6.text == "ABC", "\"\(p6.text)\"")

        // 7. span 内实体/中文混排: 标签与实体不动, 拉丁大写
        let p7 = SpeechRequestParser.parse(wrap(
            "<prosody pitch=\"+50.0%\">a&amp;b字</prosody>"))
        expect("span内混排", p7.text == "A&B字", "\"\(p7.text)\"")

        // 8. 兄弟 span + 无 span 文本交错
        let p8 = SpeechRequestParser.parse(wrap(
            "x<prosody pitch=\"+30.0%\">y</prosody>z"))
        expect("兄弟span交错", p8.text == "x Y z", "\"\(p8.text)\" \(p8.pitch)")

        if g_fail == 0 {
            print("全部通过")
        } else {
            print("\(g_fail) 项失败")
            exit(1)
        }
    }
}
