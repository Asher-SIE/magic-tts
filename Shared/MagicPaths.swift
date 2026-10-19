import Foundation

/// 统一的路径约定 (App 与扩展共用).
///
/// 引擎物料 (1.8G) **外置**: 容器 ~/Library/Application Support/Engines/, 由
/// App「导入引擎数据」或首启自动导入 (app 旁 Engines/) 落地 — 大包不进 app
/// bundle (内嵌 1.8G 时 LaunchServices 每次启动都要校验巨包签名, 启动要数秒)。
/// 分发形态 = 单 zip (MagicTTS.app + Engines/), 见 scripts/package-zip.sh。
/// 可写状态 (方案/规则/调试) 与物料同在 appex 自有沙盒容器内, 读自己的容器
/// 不触发 TCC「访问其他 App 的数据」弹框 (2026-10-17 实测: App Group 容器 +
/// 自签身份下, 每次合成都弹框, 连真实证书签名都记不住授权; 自有容器彻底根治)。
/// 宿主 App 不沙盒化, 直接读同一绝对路径 (首次会弹一次框, GUI 进程的授权会被记住)。
enum MagicPaths {
    static let appexIdentifier = "com.magictts.voices.synth"

    /// appex 沙盒容器 Data 目录 (两侧进程同一物理位置).
    ///
    /// ⚠️ 沙盒内外 home 含义不同, 不能拼同一条相对路径 (2026-10-17 事故根因:
    /// appex 内 homeDirectoryForCurrentUser = 容器 Data, 再拼
    /// "Library/Containers/<id>/Data" 会得到容器内嵌套路径, 引擎数据永远找不到
    /// → speechVoices 空 → 不注册 + say 空音频, 而 App 试听正常, 极易误诊):
    /// - appex 进程 (Bundle.main 即扩展): home 就是容器 Data, 直接用;
    /// - App / CLI (无沙盒): home 为真实家目录, 拼 Containers 绝对路径.
    static var appexContainerData: URL {
        if Bundle.main.bundleIdentifier == appexIdentifier {
            return FileManager.default.homeDirectoryForCurrentUser
        }
        return FileManager.default.homeDirectoryForCurrentUser
            .appendingPathComponent("Library/Containers/\(appexIdentifier)/Data", isDirectory: true)
    }

    /// 可写状态根 (scheme.json / rules.json / voicemode.txt / .debug):
    /// 容器 Engines/, 沿用历史路径 — 老配置无迁移直接生效.
    static var stateRoot: URL {
        let root = appexContainerData.appendingPathComponent("Engines", isDirectory: true)
        try? FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
        return root
    }

    /// 引擎物料根 (只读 for appex; App 导入时写入):
    /// 容器 Application Support/Engines/<key>/ — 即 appex 沙盒内标准
    /// Application Support 位置, 与 stateRoot 同树, 合成宿主读取实测可靠。
    /// 此处不建目录: 未导入时 scan 走「缺物料」, App 导入时才创建。
    static var enginesRoot: URL {
        appexContainerData
            .appendingPathComponent("Library", isDirectory: true)
            .appendingPathComponent("Application Support", isDirectory: true)
            .appendingPathComponent("Engines", isDirectory: true)
    }
}
