import AVFoundation
import Foundation

/// AU 扩展入口 (principal class): 创建 Audio Unit 实例.
/// Info.plist: NSExtensionPrincipalClass = $(PRODUCT_MODULE_NAME).MagicAUExtensionController
final class MagicAUExtensionController: NSObject, AUAudioUnitFactory {
    func beginRequest(with context: NSExtensionContext) {
        // AU 扩展点: 系统以 createAudioUnit 驱动, beginRequest 无需处理.
    }

    func createAudioUnit(with componentDescription: AudioComponentDescription) throws -> AUAudioUnit {
        try MagicSpeechProviderAU(componentDescription: componentDescription, options: [])
    }
}
