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

/// 方案列表行: 名称 + 两侧发音人/参数摘要。
struct SchemeRow: View {
    let slot: String
    let scheme: Scheme
    let voices: [VoiceOption]
    var selected: Bool = false
    var onTap: () -> Void = {}
    var onDoubleTap: () -> Void = {}

    private func sideDesc(_ c: SchemeVoiceConfig) -> String {
        if let hit = voices.first(where: { $0.engine == c.engine && $0.key == c.voice && !c.voice.isEmpty }) {
            return hit.label
        }
        return "自动 · \(c.engine) (\(c.autoLocale))"
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 2) {
            Text(scheme.label.isEmpty ? (SchemeStore.slotNames[slot] ?? slot) : scheme.label)
            if scheme.useSecondary {
                Text("中文: \(sideDesc(scheme.primary))    英文: \(sideDesc(scheme.secondary))")
                    .font(.caption).foregroundStyle(.secondary)
                    .lineLimit(1).truncationMode(.middle)
                Text("语速 \(scheme.primary.rate)%/\(scheme.secondary.rate)%  音量 \(scheme.primary.volume)%/\(scheme.secondary.volume)%")
                    .font(.caption2).foregroundStyle(.tertiary)
            } else {
                Text("引擎: \(sideDesc(scheme.primary))")
                    .font(.caption).foregroundStyle(.secondary)
                    .lineLimit(1).truncationMode(.middle)
                Text("语速 \(scheme.primary.rate)%  音量 \(scheme.primary.volume)%")
                    .font(.caption2).foregroundStyle(.tertiary)
            }
        }
        .padding(.vertical, 6).padding(.horizontal, 8)
        .frame(maxWidth: .infinity, alignment: .leading)
        .background(selected ? Color.accentColor.opacity(0.15) : Color.clear,
                    in: RoundedRectangle(cornerRadius: 6))
        .contentShape(Rectangle())
        .onTapGesture(count: 2, perform: onDoubleTap)
        .onTapGesture(count: 1, perform: onTap)
    }
}

/// 编辑器打开状态: 槽位 + 方案草稿 (新增时预分配空闲槽位, 保存才落盘)。
struct SchemeEditDraft: Identifiable {
    let slot: String
    let scheme: Scheme
    let isNew: Bool
    var id: String { slot }
}

/// 方案编辑: 名称 / 主要引擎与参数 (含大小写字母处理) / 第二语音开关与引擎 /
/// 分发规则; 试听随时按面板当前参数预览。
struct SchemeEditor: View {
    let draft: SchemeEditDraft
    let voices: [VoiceOption]
    var onSave: (String, Scheme) -> Void

    @State private var scheme: Scheme
    @State private var primarySel = ""
    @State private var secondarySel = ""
    @Environment(\.dismiss) private var dismiss

    init(draft: SchemeEditDraft, voices: [VoiceOption],
         onSave: @escaping (String, Scheme) -> Void) {
        self.draft = draft
        self.voices = voices
        self.onSave = onSave
        _scheme = State(initialValue: draft.scheme)
    }

    /// 第二语音选择器里"无"选项的 tag (= 单语音方案, 全部文本归主侧)。
    private static let noneSel = "none"

    /// 面板当前状态 (选择器已套用) — 试听与保存都用它
    private var previewing: Scheme {
        var s = scheme
        applyPicker(primarySel, into: &s.primary)
        s.useSecondary = secondarySel != Self.noneSel
        if s.useSecondary {
            applyPicker(secondarySel, into: &s.secondary)
        }
        return s
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            Text(draft.isNew ? "新增方案" : "编辑方案").font(.title3.bold())

            Form {
                Section("方案") {
                    TextField("方案名称", text: $scheme.label)
                    Toggle("应用朗读替换规则 (全局 rules.json)", isOn: $scheme.useRules)
                }

                Section("主要引擎") {
                    Picker("发音人", selection: $primarySel) {
                        ForEach(voices) { v in Text(v.label).tag(v.id) }
                    }
                    sideParams($scheme.primary)
                    Picker("大小写字母处理", selection: $scheme.rules.letterCase) {
                        Text("无").tag(LetterCaseStyle.none)
                        Text("前缀「大写」").tag(LetterCaseStyle.prefixZh)
                        Text("前缀「Capital」").tag(LetterCaseStyle.prefixEn)
                        Text("提高音高").tag(LetterCaseStyle.pitch)
                    }
                }

                Section("第二语音") {
                    Picker("发音人", selection: $secondarySel) {
                        Text("无 (仅主引擎朗读)").tag(Self.noneSel)
                        ForEach(voices) { v in Text(v.label).tag(v.id) }
                    }
                    if previewing.useSecondary {
                        sideParams($scheme.secondary)
                    }
                }

                if previewing.useSecondary {
                    Section("分发规则") {
                        Toggle("孤立字母归中文侧 (如 “Z 分组”)", isOn: $scheme.rules.isolatedLetterToPrimary)
                        Toggle("数字归中文侧", isOn: $scheme.rules.digitsToPrimary)
                        Stepper("英文最少词数: \(scheme.rules.englishMinWords)",
                                value: $scheme.rules.englishMinWords, in: 1...3)
                    }
                }
            }
            .formStyle(.grouped)

            HStack {
                Button("试听") { EngineRegistry.previewScheme(previewing) }
                Spacer()
                Button("取消") { dismiss() }
                    .keyboardShortcut(.cancelAction)
                Button("保存") {
                    let s = previewing
                    onSave(draft.slot, s)
                    dismiss()
                }
                .keyboardShortcut(.defaultAction)
            }
        }
        .padding(16)
        .frame(width: 480, height: 640)
        .onAppear {
            primarySel = selId(scheme.primary)
            secondarySel = scheme.useSecondary ? selId(scheme.secondary) : Self.noneSel
        }
    }

    private func sideParams(_ side: Binding<SchemeVoiceConfig>) -> some View {
        Group {
            HStack {
                Text("语速 \(side.wrappedValue.rate)%")
                Slider(value: Binding(
                    get: { Double(side.wrappedValue.rate) },
                    set: { side.wrappedValue.rate = Int($0) }), in: 20...500, step: 1)
            }
            HStack {
                Text("音高 \(side.wrappedValue.pitch)")
                Slider(value: Binding(
                    get: { Double(side.wrappedValue.pitch) },
                    set: { side.wrappedValue.pitch = Int($0) }), in: 0...100, step: 1)
            }
            HStack {
                Text("音量 \(side.wrappedValue.volume)%")
                Slider(value: Binding(
                    get: { Double(side.wrappedValue.volume) },
                    set: { side.wrappedValue.volume = Int($0) }), in: 0...200, step: 1)
            }
            eqRow("低频", side.eqLow)
            eqRow("中频", side.eqMid)
            eqRow("高频", side.eqHigh)
        }
    }

    /// 三段音色均衡行: 标签 + 带符号 dB 值 + -12...12 dB 滑块 (0 = 不滤波)。
    private func eqRow(_ label: String, _ value: Binding<Int>) -> some View {
        HStack {
            Text("\(label) \(value.wrappedValue >= 0 ? "+" : "")\(value.wrappedValue)dB")
            Slider(value: Binding(
                get: { Double(value.wrappedValue) },
                set: { value.wrappedValue = Int($0) }), in: -12...12, step: 1)
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
    @State private var selectedSlot: String?
    @State private var showRules = false
    @State private var editing: SchemeEditDraft?
    @State private var confirmDelete = false
    @State private var importing = false
    @State private var importError: String?

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

    private var orderedSlots: [String] {
        SchemeStore.slots.filter { schemes[$0] != nil }
    }

    private var freeSlot: String? {
        SchemeStore.slots.first { schemes[$0] == nil }
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            Text("Magic TTS")
                .font(.title2.bold())

            schemePanel

            HStack {
                Button("朗读替换规则…") { showRules = true }
                Button("刷新语音列表") { reload() }
                Button(importing ? "导入中…" : "导入引擎数据…") { pickEnginesArchive() }
                    .disabled(importing)
                Spacer()
            }

            List {
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
            .frame(minHeight: 200)
            .listStyle(.inset)
        }
        .padding(16)
        .frame(minWidth: 760, minHeight: 480)
        .sheet(isPresented: $showRules) { RulesView() }
        .sheet(item: $editing) { draft in
            SchemeEditor(draft: draft, voices: voiceOptions) { slot, scheme in
                saveScheme(scheme, slot: slot)
            }
        }
        .confirmationDialog("删除所选方案？", isPresented: $confirmDelete, titleVisibility: .visible) {
            Button("删除", role: .destructive) { deleteSelected() }
            Button("取消", role: .cancel) {}
        }
        .alert("导入引擎数据失败",
               isPresented: Binding(get: { importError != nil },
                                    set: { if !$0 { importError = nil } })) {
            Button("好", role: .cancel) {}
        } message: {
            Text(importError ?? "")
        }
        .onAppear { reload() }
        .onReceive(NotificationCenter.default.publisher(for: EngineImporter.importedNotification)) { _ in
            reload()
        }
    }

    /// 选 zip 包或已解压目录导入引擎物料 (大包拷贝需数秒~分钟, 后台执行)
    private func pickEnginesArchive() {
        let panel = NSOpenPanel()
        panel.canChooseFiles = true
        panel.canChooseDirectories = true
        panel.allowsMultipleSelection = false
        panel.message = "选择分发包里的 Engines 目录或整包 zip"
        if panel.runModal() == .OK, let url = panel.url {
            importEngines(from: url)
        }
    }

    private func importEngines(from url: URL) {
        importing = true
        DispatchQueue.global(qos: .userInitiated).async {
            let result = Result { try EngineImporter.importEngines(from: url) }
            DispatchQueue.main.async {
                importing = false
                switch result {
                case .success:
                    reload()
                case .failure(let err):
                    importError = err.localizedDescription
                }
            }
        }
    }

    /// 上方方案区: 左列表 (单击选中, 双击编辑) + 右侧管理按钮
    private var schemePanel: some View {
        HStack(alignment: .top, spacing: 12) {
            ScrollView {
                VStack(spacing: 0) {
                    if orderedSlots.isEmpty {
                        Text("暂无方案，点「新增」创建。")
                            .font(.caption).foregroundStyle(.secondary)
                            .frame(maxWidth: .infinity, alignment: .leading)
                            .padding(8)
                    }
                    ForEach(orderedSlots, id: \.self) { slot in
                        SchemeRow(slot: slot, scheme: schemes[slot]!, voices: voiceOptions,
                                  selected: selectedSlot == slot,
                                  onTap: { selectedSlot = slot },
                                  onDoubleTap: { openEditor(slot) })
                    }
                }
            }
            .frame(minHeight: 96, maxHeight: 176)
            .overlay(RoundedRectangle(cornerRadius: 8).stroke(.quaternary, lineWidth: 1))

            VStack(spacing: 8) {
                Button("新增") {
                    guard let slot = freeSlot else { return }
                    editing = SchemeEditDraft(slot: slot, scheme: newScheme(), isNew: true)
                }
                .disabled(freeSlot == nil)
                Button("编辑") {
                    if let slot = selectedSlot { openEditor(slot) }
                }
                .disabled(selectedSlot == nil)
                Button("删除") { confirmDelete = true }
                    .disabled(selectedSlot == nil)
                Divider().frame(width: 56)
                Button("试听") {
                    if let slot = selectedSlot, let s = schemes[slot] {
                        EngineRegistry.previewScheme(s)
                    }
                }
                .disabled(selectedSlot == nil)
            }
            .frame(width: 68)
        }
    }

    private func newScheme() -> Scheme {
        Scheme(
            primary: SchemeVoiceConfig(engine: "kona", voice: "eddy-zh",
                                       autoLocale: SchemeSides.defaultPrimaryLocale),
            secondary: SchemeVoiceConfig(engine: "kona", voice: "eddy-en",
                                         autoLocale: SchemeSides.defaultSecondaryLocale))
    }

    private func openEditor(_ slot: String) {
        guard let s = schemes[slot] else { return }
        editing = SchemeEditDraft(slot: slot, scheme: s, isNew: false)
    }

    private func saveScheme(_ scheme: Scheme, slot: String) {
        var all = SchemeStore.loadAll()
        all[slot] = scheme
        do {
            try SchemeStore.save(all)
            schemes = all
            selectedSlot = slot
            EngineRegistry.refreshVoices()
        } catch {
            NSLog("[MagicTTS] 方案保存失败: %@", error.localizedDescription)
        }
    }

    private func deleteSelected() {
        guard let slot = selectedSlot else { return }
        var all = SchemeStore.loadAll()
        all.removeValue(forKey: slot)
        do {
            try SchemeStore.save(all)
        } catch {
            NSLog("[MagicTTS] 方案删除失败: %@", error.localizedDescription)
        }
        schemes = all
        selectedSlot = orderedSlots.first
        EngineRegistry.refreshVoices()
    }

    private func reload() {
        engines = EngineRegistry.engines()
        schemes = SchemeStore.loadAll()
        if let sel = selectedSlot, schemes[sel] == nil { selectedSlot = nil }
        if selectedSlot == nil { selectedSlot = orderedSlots.first }
        EngineRegistry.refreshVoices()
    }
}
