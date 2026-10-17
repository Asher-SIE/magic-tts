#!/bin/bash
# 无 Xcode 环境下的构建脚本: 用 Command Line Tools 的 swiftc/clang
# 直接产出 MagicTTS.app (含 MagicTTSSynth.appex), ad-hoc 签名.
# 装好 Xcode 后可改用 xcodebuild 工程.
set -euo pipefail
cd "$(dirname "$0")/.."

SDK="$(xcrun --sdk macosx --show-sdk-path)"
TARGET="arm64-apple-macos14.0"
OUT="build"
APP="$OUT/MagicTTS.app"
APPEX="$APP/Contents/PlugIns/MagicTTSSynth.appex"

rm -rf "$OUT"
mkdir -p "$APP/Contents/MacOS" "$APPEX/Contents/MacOS"

# —— C 桥预编译为 .o (swiftc 的二级 clang 不转发 -Xcc include, 故分开编) ——
mkdir -p "$OUT/obj"
for f in Bridge/*.c; do
    clang -c -std=c11 -fPIC -mmacosx-version-min=14.0 -IBridge/include "$f" -o "$OUT/obj/$(basename "${f%.c}").o"
done

# —— Audio Unit 扩展 (MagicTTSSynth): 无 main, 入口 = libextension 的 _NSExtensionMain ——
xcrun --sdk macosx swiftc -swift-version 5 \
    -sdk "$SDK" -target "$TARGET" \
    -module-name MagicTTSSynth \
    -import-objc-header Bridge/MagicBridge-Bridging-Header.h -IBridge/include \
    -parse-as-library \
    -Xlinker -e -Xlinker _NSExtensionMain -lextension \
    -emit-executable -o "$APPEX/Contents/MacOS/MagicTTSSynth" \
    Extension/*.swift Shared/*.swift "$OUT"/obj/*.o \
    -framework AVFAudio -framework AudioToolbox

# Info.plist 变量替换 (模拟 Xcode 的处理)
sed -e 's/\$(PRODUCT_MODULE_NAME)/MagicTTSSynth/g' \
    -e 's/\$(PRODUCT_BUNDLE_IDENTIFIER)/com.magictts.voices.synth/g' \
    -e 's/\$(EXECUTABLE_NAME)/MagicTTSSynth/g' \
    -e 's/\$(PRODUCT_NAME)/MagicTTSSynth/g' \
    Extension/Info.plist > "$APPEX/Contents/Info.plist"

# —— 宿主 App (MagicTTS) ——
xcrun --sdk macosx swiftc -swift-version 5 \
    -sdk "$SDK" -target "$TARGET" \
    -module-name MagicTTS \
    -import-objc-header Bridge/MagicBridge-Bridging-Header.h -IBridge/include \
    -emit-executable -o "$APP/Contents/MacOS/MagicTTS" \
    App/*.swift Shared/*.swift "$OUT"/obj/*.o \
    -framework SwiftUI -framework AVFAudio -framework AudioToolbox

cat > "$APP/Contents/Info.plist" <<'PLIST'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
	<key>CFBundleDevelopmentRegion</key>
	<string>zh_CN</string>
	<key>CFBundleDisplayName</key>
	<string>Magic TTS</string>
	<key>CFBundleExecutable</key>
	<string>MagicTTS</string>
	<key>CFBundleIdentifier</key>
	<string>com.magictts.voices</string>
	<key>CFBundleInfoDictionaryVersion</key>
	<string>6.0</string>
	<key>CFBundleName</key>
	<string>MagicTTS</string>
	<key>CFBundlePackageType</key>
	<string>APPL</string>
	<key>CFBundleShortVersionString</key>
	<string>0.2</string>
	<key>CFBundleVersion</key>
	<string>2</string>
	<key>LSMinimumSystemVersion</key>
	<string>14.0</string>
	<key>NSHighResolutionCapable</key>
	<true/>
	<key>NSPrincipalClass</key>
	<string>NSApplication</string>
</dict>
</plist>
PLIST

# —— 签名 (appex 先签, 再签 App 整体; 均带沙盒+AppGroup entitlements) ——
# 优先用钥匙串里的真实证书 (TCC 权限授权可被系统记住, 免每次弹框);
# 无证书时退回 ad-hoc (TCC 每次访问组容器都可能重新询问).
SIGN_ID="MagicToolbox Dev"
if ! security find-identity -v -p codesigning | grep -q "$SIGN_ID"; then
    SIGN_ID="-"
fi
echo "签名身份: $SIGN_ID"
codesign --force --sign "$SIGN_ID" --timestamp=none \
    --entitlements Entitlements/MagicTTSSynth.entitlements "$APPEX"
codesign --force --sign "$SIGN_ID" --timestamp=none \
    --entitlements Entitlements/MagicTTS.entitlements "$APP"

echo
echo "构建完成: $APP"
codesign -dv "$APP" 2>&1 | grep -E "Identifier|Signature" | head -2
