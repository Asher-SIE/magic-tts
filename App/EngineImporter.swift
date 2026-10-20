import Foundation

/// 引擎物料导入: 从 zip 包或已解压目录导入到容器 Application Support/Engines。
/// 分发形态 = 单 zip (MagicTTS.app + Engines/, scripts/package-zip.sh):
/// 解压后 app 旁即 Engines/ → 首启自动导入; 也可在 App 内手动选 zip / 目录。
/// 拷贝走 /bin/cp -Rc (APFS clonefile, 1.8G 秒级)。
enum EngineImporter {

    /// 导入完成通知 (main 线程发出) — UI 收到后重扫引擎列表
    static let importedNotification = Notification.Name("MagicTTS.enginesImported")

    /// 已安装引擎目录名 (enginesRoot 下一级目录, 排除隐藏项)
    static func installedEngines() -> [String] {
        let fm = FileManager.default
        guard let names = try? fm.contentsOfDirectory(atPath: MagicPaths.enginesRoot.path) else {
            return []
        }
        return names.filter { !$0.hasPrefix(".") && $0 != "__MACOSX" }
    }

    /// 首启自动导入: 未装任何物料且 app 旁有 Engines/ → 导入; 返回引擎名, 无动作返回 nil
    @discardableResult
    static func autoImportIfNeeded() -> [String]? {
        guard installedEngines().isEmpty else { return nil }
        let sibling = Bundle.main.bundleURL
            .deletingLastPathComponent().appendingPathComponent("Engines", isDirectory: true)
        guard FileManager.default.fileExists(atPath: sibling.path) else { return nil }
        guard let got = try? importEngines(from: sibling), !got.isEmpty else { return nil }
        return got
    }

    /// 导入物料: src 为引擎目录 (yongde/… 一级) 或含 Engines/ 的 zip/解压目录;
    /// 逐引擎替换式拷入 (同名先删); 返回导入的引擎名。
    static func importEngines(from src: URL) throws -> [String] {
        let srcDir = src.hasDirectoryPath ? src : try unzip(src)
        let enginesDir = try locateEnginesRoot(in: srcDir)
        return try copyEngines(from: enginesDir)
    }

    /// 在解压产物中定位引擎一级目录: 本身即引擎目录 / 其下 Engines/ / 再深一层
    /// (兼容 zip 里多包了一层的情况)。
    private static func locateEnginesRoot(in dir: URL) throws -> URL {
        if hasEngineChild(dir) { return dir }
        let engines = dir.appendingPathComponent("Engines", isDirectory: true)
        if hasEngineChild(engines) { return engines }
        let fm = FileManager.default
        if let subs = try? fm.contentsOfDirectory(atPath: dir.path) {
            for s in subs where !s.hasPrefix(".") {
                let inner = dir.appendingPathComponent(s, isDirectory: true)
                let candidate = inner.appendingPathComponent("Engines", isDirectory: true)
                if hasEngineChild(candidate) { return candidate }
            }
        }
        throw ImportError("未找到引擎物料 (需含 yongde/… 或 Engines/ 目录): \(dir.path)")
    }

    /// 目录下是否有引擎物料子目录 (yongde: voices/; MS: model/ + dylib;
    /// aisound: Resource.irf)
    private static func hasEngineChild(_ dir: URL) -> Bool {
        let fm = FileManager.default
        guard let names = try? fm.contentsOfDirectory(atPath: dir.path) else { return false }
        return names.contains { name in
            guard !name.hasPrefix(".") else { return false }
            let child = dir.appendingPathComponent(name, isDirectory: true)
            var isDir: ObjCBool = false
            guard fm.fileExists(atPath: child.path, isDirectory: &isDir), isDir.boolValue else {
                return false
            }
            let files = (try? fm.contentsOfDirectory(atPath: child.path)) ?? []
            return files.contains("voices") || files.contains("model")
                || files.contains("Resource.irf")
                || files.contains { $0.hasSuffix(".dylib") }
        }
    }

    /// 逐引擎替换式拷入 enginesRoot
    private static func copyEngines(from dir: URL) throws -> [String] {
        let fm = FileManager.default
        let dst = MagicPaths.enginesRoot
        try fm.createDirectory(at: dst, withIntermediateDirectories: true)
        guard let names = try? fm.contentsOfDirectory(atPath: dir.path),
              !names.isEmpty else {
            throw ImportError("引擎目录为空: \(dir.path)")
        }
        var imported: [String] = []
        for name in names {
            guard !name.hasPrefix(".") && name != "__MACOSX" else { continue }
            let src = dir.appendingPathComponent(name, isDirectory: true)
            var isDir: ObjCBool = false
            guard fm.fileExists(atPath: src.path, isDirectory: &isDir), isDir.boolValue else { continue }
            let target = dst.appendingPathComponent(name, isDirectory: true)
            try? fm.removeItem(at: target)
            try run("/bin/cp", ["-Rc", src.path, target.path])
            imported.append(name)
        }
        guard !imported.isEmpty else {
            throw ImportError("目录中无引擎子目录: \(dir.path)")
        }
        return imported
    }

    /// ditto 解压 zip 到临时目录, 返回解压目录
    private static func unzip(_ zip: URL) throws -> URL {
        let dst = FileManager.default.temporaryDirectory
            .appendingPathComponent("magictts-import-\(UUID().uuidString)", isDirectory: true)
        try FileManager.default.createDirectory(at: dst, withIntermediateDirectories: true)
        try run("/usr/bin/ditto", ["-x", "-k", zip.path, dst.path])
        return dst
    }

    private static func run(_ executable: String, _ arguments: [String]) throws {
        let p = Process()
        p.executableURL = URL(fileURLWithPath: executable)
        p.arguments = arguments
        try p.run()
        p.waitUntilExit()
        guard p.terminationStatus == 0 else {
            throw ImportError("\(executable) 退出码 \(p.terminationStatus): \(arguments)")
        }
    }
}

struct ImportError: LocalizedError {
    let message: String
    init(_ message: String) { self.message = message }
    var errorDescription: String? { message }
}
