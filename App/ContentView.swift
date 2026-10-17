import SwiftUI

struct ContentView: View {
    @State private var engines: [EngineStatus] = []

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            Text("Magic TTS")
                .font(.title2.bold())

            Text("引擎物料目录: \(MagicPaths.enginesRoot.path)")
                .font(.caption).foregroundStyle(.secondary)
                .textSelection(.enabled)

            Table(engines) {
                TableColumn("语音名") { Text($0.name) }
                TableColumn("引擎") { Text($0.key) }
                TableColumn("状态") { e in
                    Text(e.mode)
                        .foregroundStyle(e.loaded ? .green : .orange)
                }
                TableColumn("试听") { e in
                    Button("播放") { EngineRegistry.preview(e) }
                }
            }
            .frame(minHeight: 200)

            Text("引擎数据由 scripts/populate-engines.sh 安装到 App Group 容器；状态为「缺少物料」时先运行安装脚本，再点刷新。")
                .font(.caption2).foregroundStyle(.secondary)

            HStack {
                Button("刷新语音列表") {
                    engines = EngineRegistry.engines()
                    EngineRegistry.refreshVoices()
                }
                Spacer()
                Text("语音已注册到系统: 系统设置 → 辅助功能 → 朗读内容 → 系统声音；或 VoiceOver 实用工具 → 语音 → 添加声音")
                    .font(.caption2).foregroundStyle(.secondary)
            }
        }
        .padding(16)
        .frame(minWidth: 640, minHeight: 320)
        .onAppear { engines = EngineRegistry.engines() }
    }
}
