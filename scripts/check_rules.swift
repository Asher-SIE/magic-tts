/* 朗读替换规则逻辑断言 (只验 RuleBook/ReadingRule 语义, 不碰音频)。
 *
 * 编译运行:
 *   xcrun swiftc -swift-version 5 scripts/check_rules.swift \
 *     Shared/ReadingRules.swift Shared/MagicPaths.swift -o /tmp/check_rules \
 *     && /tmp/check_rules
 */
import Foundation

var failures = 0
func expect(_ name: String, _ cond: Bool) {
    if cond { print("ok   \(name)") } else { failures += 1; print("FAIL \(name)") }
}

@main
enum CheckRulesMain {
    static func main() {
        let book = RuleBook([
            ReadingRule(find: "重新", replace: "虫新"),
            ReadingRule(find: "iOS", replace: "eye OS", ignoreCase: true, wholeWord: true),
            ReadingRule(find: "(\\d+(?:\\.\\d+)?)%", replace: "百分之$1", regex: true),
            ReadingRule(find: "停用词", replace: "XX", enabled: false),
            ReadingRule(find: "(", replace: "y", regex: true),   // 坏正则 → 编译失败被跳过
        ])

        expect("普通替换", book.apply("请重新连接") == "请虫新连接")
        expect("整词命中 (中文邻接)", book.apply("连接iOS设备") == "连接eye OS设备")
        expect("整词不命中词内", book.apply("iOSomething") == "iOSomething")
        expect("忽略大小写", book.apply("IOS") == "eye OS")
        expect("正则组引用", book.apply("进度50%") == "进度百分之50")
        expect("正则小数", book.apply("完成3.14%") == "完成百分之3.14")
        expect("停用规则不生效", book.apply("停用词") == "停用词")
        expect("坏正则跳过不影响其他", book.apply("进度50%") == "进度百分之50")
        expect("空文本", book.apply("") == "")
        expect("空书直通", RuleBook([]).apply("任何内容") == "任何内容")

        // 整词边界细则: 英文词内不算命中, 中文字符不算词的一部分
        let ww = RuleBook([ReadingRule(find: "live", replace: "莱夫", wholeWord: true)])
        expect("整词: 不命中 deliver", ww.apply("deliver") == "deliver")
        expect("整词: 中文旁命中", ww.apply("看live直播") == "看莱夫直播")

        // 链式: 后一条看到前一条的结果
        let chain = RuleBook([
            ReadingRule(find: "A", replace: "B"),
            ReadingRule(find: "B", replace: "C"),
        ])
        expect("链式按序应用", chain.apply("A") == "C")

        // 普通模式替换串里的 $ 被转义, 不当组引用
        let dollar = RuleBook([ReadingRule(find: "价格", replace: "$1元")])
        expect("普通模式 $ 字面输出", dollar.apply("价格") == "$1元")

        if failures > 0 { print("\n\(failures) 项失败"); exit(1) }
        print("\n全部通过")
    }
}
