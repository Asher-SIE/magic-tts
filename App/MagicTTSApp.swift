import SwiftUI

@main
struct MagicTTSApp: App {
    init() {
        // 启动即刷新发音人注册 (不依赖窗口/视图出现;
        // NSLog 对未签名 CLT 构建不进统一日志, 调试走文件)
        let engines = EngineRegistry.engines()
        let root = MagicPaths.enginesRoot.appendingPathComponent(".debug", isDirectory: true)
        try? FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
        let lines = engines.map { e in
            "\(e.key): loaded=\(e.loaded) voices=\(e.voices.map(\.name).joined(separator: ","))"
        }
        try? lines.joined(separator: "\n").data(using: .utf8)?
            .write(to: root.appendingPathComponent("app_engines.txt"))
        EngineRegistry.refreshVoices()
    }

    var body: some Scene {
        WindowGroup { ContentView() }
            .windowResizability(.contentSize)
    }
}
