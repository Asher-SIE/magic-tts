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

        // 内置默认替换: 「率」读 lǜ 的词改写同音「律」, 读 shuài 的不动
        let bi = RuleBook(ReadingRule.builtin)
        expect("内置: 圆周率改写", bi.apply("圆周率是3.14") == "圆周律是3.14")
        expect("内置: 效率概率频率", bi.apply("效率和概率、频率") == "效律和概律、频律")
        expect("内置: 坦率直率不动", bi.apply("坦率 直率") == "坦率 直率")
        expect("内置: 率先率领率真率性不动", bi.apply("率先 率领 率真 率性") == "率先 率领 率真 率性")
        expect("内置: 表率草率轻率不动", bi.apply("表率 草率 轻率") == "表率 草率 轻率")

        // 内置: 连字符读法 — 日期/区间(至)/减法(减)/负号(负), 语境按序判定
        expect("负号: 行首", bi.apply("-5度") == "负5度")
        expect("负号: 中文后", bi.apply("气温-5度") == "气温负5度")
        expect("负号: 全角－", bi.apply("气温－5度") == "气温负5度")
        expect("负号: 算符/括号后", bi.apply("=(-3)+5, 3×(-5)") == "=(负3)+5, 3×(负5)")
        expect("负号: 字母后不动", bi.apply("COVID-19, B-52") == "COVID-19, B-52")
        expect("负号: 空格列表项不动", bi.apply("- 5个要点") == "- 5个要点")
        expect("减法: 等式", bi.apply("10-3=7") == "10减3=7")
        expect("减法: 两侧空格", bi.apply("10 - 3") == "10减3")
        expect("减法: 右括号后", bi.apply("(4×2)-3") == "(4×2)减3")
        expect("减法: 中文算式行文", bi.apply("10-3等于7, 5-2的差") == "10减3等于7, 5减2的差")
        expect("区间: 量词", bi.apply("3-5天, 第2-4章") == "3至5天, 第2至4章")
        expect("区间: 百分比", bi.apply("10%-20%") == "10%至20%")
        expect("区间: 年份", bi.apply("2024-2025年") == "2024至2025年")
        expect("日期: 年月日", bi.apply("2026-10-19") == "2026年10月19日")
        expect("日期: 单位月日", bi.apply("2026-1-5") == "2026年1月5日")
        expect("负减混合", bi.apply("气温-5度, 2026-10-19, 10-3=7") == "气温负5度, 2026年10月19日, 10减3=7")

        // 用户规则先应用, 内置在其结果上兜底 (用户产出的 ×率 也被修正)
        let userThenBuiltin = RuleBook([ReadingRule(find: "π", replace: "圆周率")] + ReadingRule.builtin)
        expect("内置在用户规则后兜底", userThenBuiltin.apply("π≈3.14") == "圆周律≈3.14")

        if failures > 0 { print("\n\(failures) 项失败"); exit(1) }
        print("\n全部通过")
    }
}
