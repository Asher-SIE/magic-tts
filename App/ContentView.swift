import SwiftUI

struct VoiceListRow: View {
    let voice: VoiceRow

    var body: some View {
        HStack {
            VStack(alignment: .leading, spacing: 2) {
                Text(voice.name)
                Text(voice.key)
                    .font(.caption2).foregroundStyle(.secondary)
                    .lineLimit(1).truncationMode(.middle)
                    .textSelection(.enabled)
            }
            Spacer()
            if voice.systemVoiceId == nil {
                Text("未注册").font(.caption).foregroundStyle(.orange)
            }
            Button("播放") { EngineRegistry.preview(voice) }
        }
        .padding(.vertical, 1)
    }
}

/// 方案编辑器里可选的发音人 (全部引擎拍平)。
struct VoiceOption: Identifiable, Hashable {
    let engine: String
    let key: String        // "" = 按语言自动选
    let label: String
    var id: String { engine + "|" + key }
}

/// 双语方案卡片: 系统槽位名固定, 实际映射即时热改 (scheme.json, 请求时读取)。
struct SchemeCard: View {
    let slot: String
    @State var scheme: Scheme
    let voices: [VoiceOption]
    var onSave: (Scheme) -> Void

    @State private var editing = false

    private func sideDesc(_ c: SchemeVoiceConfig) -> String {
        if let hit = voices.first(where: { $0.engine == c.engine && $0.key == c.voice && !c.voice.isEmpty }) {
            return hit.label
        }
        return "自动 · \(c.engine) (\(c.autoLocale))"
    }

    var body: some View {
        HStack {
            VStack(alignment: .leading, spacing: 2) {
                Text("\(SchemeStore.slotNames[slot] ?? slot)\(scheme.label.isEmpty ? "" : " — \(scheme.label)")")
                Text("中文: \(sideDesc(scheme.primary))    英文: \(sideDesc(scheme.secondary))")
                    .font(.caption).foregroundStyle(.secondary)
                    .lineLimit(1).truncationMode(.middle)
                Text("语速 \(scheme.primary.rate)%/\(scheme.secondary.rate)%  音量 \(scheme.primary.volume)%/\(scheme.secondary.volume)%")
                    .font(.caption2).foregroundStyle(.tertiary)
            }
            Spacer()
            Button("试听") { EngineRegistry.previewScheme(scheme) }
            Button("编辑") { editing = true }
        }
        .padding(.vertical, 1)
        .sheet(isPresented: $editing) {
            SchemeEditor(scheme: scheme, voices: voices) { updated in
                scheme = updated
                onSave(updated)
                editing = false
            }
        }
    }
}

/// 方案编辑: 主/副发音人 + 各自语速/音高/音量 + 分发规则。
struct SchemeEditor: View {
    @State var scheme: Scheme
    let voices: [VoiceOption]
    var onSave: (Scheme) -> Void

    @State private var primarySel = ""
    @State private var secondarySel = ""
    @Environment(\.dismiss) private var dismiss

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            Text("编辑方案").font(.title3.bold())

            Form {
                TextField("备注", text: $scheme.label)

                Picker("中文发音人", selection: $primarySel) {
                    ForEach(voices) { v in Text(v.label).tag(v.id) }
                }
                Picker("英文发音人", selection: $secondarySel) {
                    ForEach(voices) { v in Text(v.label).tag(v.id) }
                }

                sideSection("中文侧参数", $scheme.primary)
                sideSection("英文侧参数", $scheme.secondary)

                Section("分发规则") {
                    Toggle("应用朗读替换规则 (全局 rules.json)", isOn: $scheme.useRules)
                    Toggle("孤立字母归中文侧 (如 “Z 分组”)", isOn: $scheme.rules.isolatedLetterToPrimary)
                    Toggle("数字归中文侧", isOn: $scheme.rules.digitsToPrimary)
                    Stepper("英文最少词数: \(scheme.rules.englishMinWords)",
                            value: $scheme.rules.englishMinWords, in: 1...3)
                }
            }
            .formStyle(.grouped)

            HStack {
                Text("方案在系统里以固定名称注册（双语方案·一/二），修改后立即生效，无需重装。")
                    .font(.caption2).foregroundStyle(.secondary)
                Spacer()
                Button("取消") { dismiss() }
                    .keyboardShortcut(.cancelAction)
                Button("保存") {
                    applyPicker(primarySel, into: &scheme.primary)
                    applyPicker(secondarySel, into: &scheme.secondary)
                    onSave(scheme)
                }
                .keyboardShortcut(.defaultAction)
            }
        }
        .padding(16)
        .frame(width: 460, height: 560)
        .onAppear {
            primarySel = selId(scheme.primary)
            secondarySel = selId(scheme.secondary)
        }
    }

    private func sideSection(_ title: String, _ side: Binding<SchemeVoiceConfig>) -> some View {
        Section(title) {
            HStack {
                Text("语速 \(side.wrappedValue.rate)%")
                Slider(value: Binding(
                    get: { Double(side.wrappedValue.rate) },
                    set: { side.wrappedValue.rate = Int($0) }), in: 20...500, step: 5)
            }
            HStack {
                Text("音高 \(side.wrappedValue.pitch)")
                Slider(value: Binding(
                    get: { Double(side.wrappedValue.pitch) },
                    set: { side.wrappedValue.pitch = Int($0) }), in: 0...100)
            }
            HStack {
                Text("音量 \(side.wrappedValue.volume)%")
                Slider(value: Binding(
                    get: { Double(side.wrappedValue.volume) },
                    set: { side.wrappedValue.volume = Int($0) }), in: 0...200, step: 5)
            }
        }
    }

    private func selId(_ c: SchemeVoiceConfig) -> String {
        c.voice.isEmpty ? c.engine + "|" : c.engine + "|" + c.voice
    }

    private func applyPicker(_ sel: String, into side: inout SchemeVoiceConfig) {
        let parts = sel.split(separator: "|", maxSplits: 1, omittingEmptySubsequences: false)
        guard parts.count == 2 else { return }
        side.engine = String(parts[0])
        side.voice = String(parts[1])
    }
}

struct ContentView: View {
    @State private var engines: [EngineStatus] = []
    @State private var schemes: [String: Scheme] = [:]
    @State private var showRules = false

    /// 全引擎发音人拍平 (方案编辑用), 每引擎的"自动"项排在该引擎发音人之前
    private var voiceOptions: [VoiceOption] {
        var out: [VoiceOption] = []
        for e in engines {
            out.append(VoiceOption(engine: e.key, key: "", label: "自动 · \(e.name)"))
            for v in e.voices {
                out.append(VoiceOption(engine: e.key, key: v.key, label: v.name))
            }
        }
        return out
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            Text("Magic TTS")
                .font(.title2.bold())

            Text("引擎物料目录: \(MagicPaths.enginesRoot.path)")
                .font(.caption).foregroundStyle(.secondary)
                .textSelection(.enabled)

            List {
                Section("语音方案 (中英自动分发)") {
                    ForEach(SchemeStore.slots, id: \.self) { slot in
                        if let scheme = schemes[slot] {
                            SchemeCard(slot: slot, scheme: scheme, voices: voiceOptions) { updated in
                                schemes[slot] = updated
                                var all = SchemeStore.loadAll()
                                all[slot] = updated
                                try? SchemeStore.save(all)
                            }
                        }
                    }
                    Text("VoiceOver/系统声音里选「双语方案·一/二」即可；中文段与英文段自动分派给两侧发音人，语速/音高/音量由 MagicTTS 托管。")
                        .font(.caption2).foregroundStyle(.secondary)
                }

                ForEach(engines) { e in
                    Section("\(e.name) · \(e.mode)") {
                        ForEach(e.voices) { v in
                            VoiceListRow(voice: v)
                        }
                        if e.voices.isEmpty {
                            Text(e.loaded ? "无可用发音人" : "引擎未就绪")
                                .font(.caption).foregroundStyle(.secondary)
                        }
                    }
                }
            }
            .frame(minHeight: 320)
            .listStyle(.inset)

            Text("每个引擎的全部发音人都会注册为系统语音；永德 = voices/ 下每个 .vl，微软引擎 = model/ 下每个含 SynthModel.license 的语音包。安装新物料后点刷新。")
                .font(.caption2).foregroundStyle(.secondary)

            HStack {
                Button("朗读替换规则…") { showRules = true }
                Button("刷新语音列表") {
                    engines = EngineRegistry.engines()
                    schemes = SchemeStore.loadAll()
                    EngineRegistry.refreshVoices()
                }
                Spacer()
                Text("系统设置 → 辅助功能 → 朗读内容 → 系统声音；或 VoiceOver 实用工具 → 语音 → 添加声音")
                    .font(.caption2).foregroundStyle(.secondary)
            }
        }
        .padding(16)
        .frame(minWidth: 760, minHeight: 460)
        .sheet(isPresented: $showRules) { RulesView() }
        .onAppear {
            engines = EngineRegistry.engines()
            schemes = SchemeStore.loadAll()
            EngineRegistry.refreshVoices()
        }
    }
}
