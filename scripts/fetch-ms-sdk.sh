#!/bin/bash
# 下载微软官方 macOS Speech SDK (版本与 APK 内 Android SDK 一致: 1.48.2)
# 并把 3 个 dylib 装进两个 MS 引擎目录 (naturalvoice + onecore).
# 来源 NuGet (公开, 无需账号):
#   Microsoft.CognitiveServices.Speech                       → core
#   Microsoft.CognitiveServices.Speech.Extension.Embedded.TTS → embedded.tts
#   Microsoft.CognitiveServices.Speech.Extension.ONNX.Runtime → onnxruntime
set -euo pipefail
cd "$(dirname "$0")/.."

VER=1.48.2
ENG="${1:-$HOME/Library/Group Containers/com.magictts.shared/Engines}"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

fetch() { # id outdir
    local id="$1" out="$2"
    echo "下载 $id@$VER ..."
    curl -sfL -o "$TMP/$id.nupkg" "https://www.nuget.org/api/v2/package/$id/$VER"
    unzip -o -q "$TMP/$id.nupkg" -d "$TMP/$id" "runtimes/osx-arm64/native/*"
    cp -v "$TMP/$id/runtimes/osx-arm64/native/"*.dylib "$out/"
}

for key in naturalvoice onecore; do
    DST="$ENG/$key"
    mkdir -p "$DST"
    fetch Microsoft.CognitiveServices.Speech "$DST"
    fetch Microsoft.CognitiveServices.Speech.Extension.Embedded.TTS "$DST"
    fetch Microsoft.CognitiveServices.Speech.Extension.ONNX.Runtime "$DST"
    mkdir -p "$DST/model"    # 嵌入式模型后续从 app.gr.run 下载放入
    echo "→ $DST 就绪"
done

echo
echo "完成. 下一步: 嵌入式模型放入 $ENG/naturalvoice/model/ 与 $ENG/onecore/model/"
echo "(下载需 gr.run 账号, 见 HANDOFF.md 第 3 步)"
