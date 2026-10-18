import SwiftUI

/// 朗读替换规则编辑: 列表 (启用/编辑/删除/排序) + 单条编辑 sheet (参数与替换预览)。
/// 规则全局一份 (rules.json), 每次改动即时保存生效; 双语方案可在方案编辑里整体关闭。
struct RulesView: View {
    @State private var rules: [ReadingRule] = RuleStore.load()
    @State private var editing: ReadingRule?
    @State private var isNew = false

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            Text("朗读替换规则").font(.title3.bold())
            Text("朗读前按序做文本替换，可修正多音字、英文缩写读法、数字符号等。普通语音全局生效；双语方案可在方案编辑里关闭。多音字「率」等常见误读已内置修正，始终在自定义规则之后兜底。")
                .font(.caption2).foregroundStyle(.secondary)

            List {
                if rules.isEmpty {
                    Text("还没有规则。点「添加」新建，或「添加示例」看规则能做什么。")
                        .font(.caption).foregroundStyle(.secondary)
                }
                ForEach(rules) { rule in
                    ruleRow(rule)
                }
                .onDelete { delete(at: $0) }
                .onMove { rules.move(fromOffsets: $0, toOffset: $1); save() }
            }
            .frame(minHeight: 220)
            .listStyle(.inset)

            HStack {
                Button("添加") {
                    isNew = true
                    editing = ReadingRule(find: "", replace: "")
                }
                Button("添加示例") {
                    rules += ReadingRule.examples.map { r in
                        var copy = r; copy.id = UUID(); return copy
                    }
                    save()
                }
                Spacer()
                Text("修改即时生效，无需重装扩展。")
                    .font(.caption2).foregroundStyle(.secondary)
            }
        }
        .padding(16)
        .frame(width: 560, height: 460)
        .sheet(item: $editing) { rule in
            RuleEditView(rule: rule, isNew: isNew) { saved in
                if let i = rules.firstIndex(where: { $0.id == saved.id }) {
                    rules[i] = saved
                } else {
                    rules.append(saved)
                }
                save()
            }
        }
    }

    private func save() { RuleStore.save(rules) }

    private func delete(at offsets: IndexSet) {
        rules.remove(atOffsets: offsets)
        save()
    }

    /// 一行摘要: "查找 → 替换（标志）" + 备注/错误
    private func summary(_ r: ReadingRule) -> String {
        let to = r.replace.isEmpty ? "（删除）" : r.replace
        var flags: [String] = []
        if r.regex { flags.append("正则") }
        if r.ignoreCase { flags.append("忽略大小写") }
        if r.wholeWord && !r.regex { flags.append("整词") }
        let f = flags.joined(separator: "、")
        return "\(r.find) → \(to)\(f.isEmpty ? "" : "（\(f)）")"
    }

    private func ruleRow(_ rule: ReadingRule) -> some View {
        HStack {
            Button {
                isNew = false
                editing = rule
            } label: {
                VStack(alignment: .leading, spacing: 2) {
                    Text(summary(rule))
                        .foregroundStyle(rule.enabled ? .primary : .secondary)
                    if !rule.note.isEmpty {
                        Text(rule.note).font(.caption).foregroundStyle(.secondary)
                    }
                    if case .failure(let e) = rule.compiled() {
                        Text(e.message).font(.caption).foregroundStyle(.red)
                    }
                }
            }
            .buttonStyle(.plain)
            .contextMenu {
                Button("删除") {
                    delete(at: IndexSet([rules.firstIndex { $0.id == rule.id } ?? 0]))
                }
            }
            Spacer()
            Toggle("启用", isOn: Binding(
                get: { rule.enabled },
                set: { on in
                    if let i = rules.firstIndex(where: { $0.id == rule.id }) {
                        rules[i].enabled = on
                        save()
                    }
                }))
            .labelsHidden()
        }
        .padding(.vertical, 1)
    }
}

/// 单条规则编辑: 查找/替换/选项 + 示例文本实时替换预览。
struct RuleEditView: View {
    @State var rule: ReadingRule
    let isNew: Bool
    var onSave: (ReadingRule) -> Void
    @Environment(\.dismiss) private var dismiss
    @State private var sample = ""

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            Text(isNew ? "新规则" : "编辑规则").font(.title3.bold())

            Form {
                Section("查找与替换") {
                    TextField("查找", text: $rule.find)
                    TextField("替换为 (留空 = 删除)", text: $rule.replace)
                    if case .failure(let e) = rule.compiled() {
                        Text(e.message).foregroundStyle(.red).font(.caption)
                    }
                }
                Section("选项") {
                    Toggle("正则表达式 (替换串可用 $1 引用捕获组)", isOn: $rule.regex)
                    Toggle("忽略大小写", isOn: $rule.ignoreCase)
                    if !rule.regex {
                        Toggle("只匹配完整英文单词/数字", isOn: $rule.wholeWord)
                    }
                    Toggle("启用", isOn: $rule.enabled)
                    TextField("备注", text: $rule.note)
                }
                Section("预览") {
                    TextField("示例文本", text: $sample, axis: .vertical)
                        .lineLimit(2...4)
                    if !sample.isEmpty {
                        let one: [ReadingRule] = {
                            var r = rule; r.enabled = true; return [r]
                        }()
                        Text("结果：\(RuleBook(one).apply(sample))")
                            .font(.callout).textSelection(.enabled)
                    }
                }
            }
            .formStyle(.grouped)

            HStack {
                Spacer()
                Button("取消") { dismiss() }.keyboardShortcut(.cancelAction)
                Button("保存") {
                    onSave(rule)
                    dismiss()
                }
                .disabled(rule.find.isEmpty)
                .keyboardShortcut(.defaultAction)
            }
        }
        .padding(16)
        .frame(width: 480, height: 500)
        .onAppear {
            if sample.isEmpty { sample = rule.regex ? "" : rule.find }
        }
    }
}
