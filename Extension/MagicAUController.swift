import AVFoundation
import Foundation

/// AU 扩展入口 (principal class): 创建 Audio Unit 实例.
/// Info.plist: NSExtensionPrincipalClass = $(PRODUCT_MODULE_NAME).MagicAUExtensionController

private func probeMark(_ name: String) {
    let dir = MagicPaths.stateRoot.appendingPathComponent(".debug", isDirectory: true)
    try? FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
    let url = dir.appendingPathComponent("entry_trace.txt")
    let line = "\(Date()) \(name)\n"
    if let old = try? String(contentsOf: url, encoding: .utf8) {
        try? (old + line).write(to: url, atomically: true, encoding: .utf8)
    } else {
        try? line.write(to: url, atomically: true, encoding: .utf8)
    }
}
final class MagicAUExtensionController: NSObject, AUAudioUnitFactory {
    func beginRequest(with context: NSExtensionContext) {
        probeMark("beginRequest")
        // AU 扩展点: 系统以 createAudioUnit 驱动, beginRequest 无需处理.
    }

    func createAudioUnit(with componentDescription: AudioComponentDescription) throws -> AUAudioUnit {
        probeMark("createAudioUnit")
        return try MagicSpeechProviderAU(componentDescription: componentDescription, options: [])
    }
}
