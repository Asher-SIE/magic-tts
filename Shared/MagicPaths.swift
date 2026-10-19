import Foundation

/// 统一的路径约定 (App 与扩展共用).
///
/// 引擎物料 (只读) 随 app 包体分发: <MagicTTS.app>/Contents/Resources/Engines/,
/// 仓库内 resources/engines/ 为备份/开发源, 构建时整目录拷入包体 — 分发一个
/// .app 即自包含, 无需装后填充。可写状态 (方案/规则/调试) 留在 appex 自己的
/// 沙盒容器 (~/Library/Containers/com.magictts.voices.synth/Data/Engines/):
/// 每句合成都由 appex 读状态, 读自己的容器不触发 TCC「访问其他 App 的数据」
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

    /// 可写状态根 (scheme.json / rules.json / voicemode.txt / .debug):
    /// 容器 Engines/, 沿用拆分前旧路径 — 老配置无迁移直接生效.
    static var stateRoot: URL {
        let root = appexContainerData.appendingPathComponent("Engines", isDirectory: true)
        try? FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
        return root
    }

    /// 引擎物料根 (只读): appex 包体 MagicTTSSynth.appex/Contents/Resources/Engines.
    /// 装在 appex 而非宿主 App 内 — 合成宿主 (系统守护进程 in-process 加载 AU) 的
    /// 沙盒只放行 appex 自身 bundle 树, 读不了宿主 App 目录 (2026-10-19 实测:
    /// 物料在 App/Contents/Resources 时 scan 仅 kona 就绪 → say 空音频).
    /// 定位用**类所在 bundle** 而非 Bundle.main — Bundle.main 在 in-process 宿主里
    /// 是宿主进程自身:
    /// - appex 进程 / 守护进程内: BundleLocator 编入 appex → 即物料所在;
    /// - 宿主 App: bundle 为 MagicTTS.app → 拼 PlugIns/ 下 appex 路径.
    final class BundleLocator {}

    static var enginesRoot: URL {
        let bundle = Bundle(for: BundleLocator.self).bundleURL
        let inThisBundle = bundle
            .appendingPathComponent("Contents/Resources/Engines", isDirectory: true)
        let inAppexOfApp = bundle
            .appendingPathComponent("Contents/PlugIns/MagicTTSSynth.appex/Contents/Resources/Engines",
                                    isDirectory: true)
        if FileManager.default.fileExists(atPath: inThisBundle.path) {
            return inThisBundle
        }
        if FileManager.default.fileExists(atPath: inAppexOfApp.path) {
            return inAppexOfApp
        }
        return inThisBundle   // 探测失败也返回首选 (scan 打「缺物料」日志)
    }
}
