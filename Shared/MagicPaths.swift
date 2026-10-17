import Foundation

/// 统一的路径约定 (App 与扩展共用).
/// macOS 26 实测: AU 扩展必须沙盒化才会被系统注册, 故 App 与扩展均走
/// App Group 容器 (ad-hoc 本地签名下 group id 无需 TeamID 前缀).
enum MagicPaths {
    static let groupIdentifier = "com.magictts.shared"

    /// 组容器: ~/Library/Group Containers/com.magictts.shared/
    /// 未沙盒的兜底路径仅为无组容器环境兼容, 正常构建都应命中组容器.
    static var supportRoot: URL {
        if let group = FileManager.default.containerURL(forSecurityApplicationGroupIdentifier: groupIdentifier) {
            return group
        }
        return FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
            .appendingPathComponent("MagicTTS", isDirectory: true)
    }

    /// 引擎目录: 每引擎一个子目录, 内放引擎库 + 数据.
    /// <组容器>/Engines/<key>/
    static var enginesRoot: URL { supportRoot.appendingPathComponent("Engines", isDirectory: true) }
}
