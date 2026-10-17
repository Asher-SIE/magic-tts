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

struct ContentView: View {
    @State private var engines: [EngineStatus] = []

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            Text("Magic TTS")
                .font(.title2.bold())

            Text("引擎物料目录: \(MagicPaths.enginesRoot.path)")
                .font(.caption).foregroundStyle(.secondary)
                .textSelection(.enabled)

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
            .frame(minHeight: 260)
            .listStyle(.inset)

            Text("每个引擎的全部发音人都会注册为系统语音；永德 = voices/ 下每个 .vl，微软引擎 = model/ 下每个含 SynthModel.license 的语音包。安装新物料后点刷新。")
                .font(.caption2).foregroundStyle(.secondary)

            HStack {
                Button("刷新语音列表") {
                    engines = EngineRegistry.engines()
                    EngineRegistry.refreshVoices()
                }
                Spacer()
                Text("系统设置 → 辅助功能 → 朗读内容 → 系统声音；或 VoiceOver 实用工具 → 语音 → 添加声音")
                    .font(.caption2).foregroundStyle(.secondary)
            }
        }
        .padding(16)
        .frame(minWidth: 720, minHeight: 380)
        .onAppear {
            engines = EngineRegistry.engines()
            EngineRegistry.refreshVoices()
        }
    }
}
