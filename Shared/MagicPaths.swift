import Foundation

/// 统一的路径约定 (App 与扩展共用).
///
/// 引擎数据放 **appex 自己的沙盒容器** (~/Library/Containers/com.magictts.voices.synth/):
/// 每句合成都由 appex 读引擎数据, 读自己的容器不触发 TCC「访问其他 App 的数据」
/// 弹框 (2026-10-17 实测: App Group 容器 + 自签身份下, 每次合成都弹框,
/// 连真实证书签名都记不住授权; 自有容器彻底根治).
/// 宿主 App 不沙盒化, 直接读同一绝对路径 (首次会弹一次框, GUI 进程的授权会被记住).
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

    /// 引擎目录: 每引擎一个子目录, 内放引擎库 + 数据.
    /// <appex容器>/Engines/<key>/
    static var enginesRoot: URL {
        let root = appexContainerData.appendingPathComponent("Engines", isDirectory: true)
        try? FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
        return root
    }
}
