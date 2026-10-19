import SwiftUI

@main
struct MagicTTSApp: App {
    init() {
        // 启动即刷新发音人注册 (不依赖窗口/视图出现;
        // NSLog 对未签名 CLT 构建不进统一日志, 调试走文件)
        // 调试转储异步写: 容器路径的 IO 不进启动关键路径
        // (2026-10-19 实测 open 启动上下文偶发阻塞在容器写入上, 拖死整个启动)
        DispatchQueue.global(qos: .utility).async { Self.dumpEngines() }
        EngineRegistry.refreshVoices()
        // 首启自动导入: app 旁有 Engines/ 且尚未装物料 → 后台导入 (1.8G 不阻塞启动)
        DispatchQueue.global(qos: .userInitiated).async {
            guard let imported = EngineImporter.autoImportIfNeeded() else { return }
            Self.dumpEngines(importedNote: "已导入: \(imported.joined(separator: ", "))")
            DispatchQueue.main.async {
                EngineRegistry.refreshVoices()
                NotificationCenter.default.post(name: EngineImporter.importedNotification, object: nil)
            }
        }
    }

    /// 引擎扫描结果落 .debug/app_engines.txt (跨上下文排查用)
    private static func dumpEngines(importedNote: String? = nil) {
        let engines = EngineRegistry.engines()
        let root = MagicPaths.stateRoot.appendingPathComponent(".debug", isDirectory: true)
        try? FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
        var lines = ["root: \(MagicPaths.enginesRoot.path)"]
        if let note = importedNote { lines.append(note) }
        lines += engines.map { e in
            "\(e.key): loaded=\(e.loaded) voices=\(e.voices.map(\.name).joined(separator: ","))"
        }
        try? lines.joined(separator: "\n").data(using: .utf8)?
            .write(to: root.appendingPathComponent("app_engines.txt"))
    }

    var body: some Scene {
        WindowGroup { ContentView() }
            .windowResizability(.contentSize)
    }
}
