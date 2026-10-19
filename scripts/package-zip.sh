#!/bin/bash
# 分发打包: MagicTTS.app (轻量, 无内嵌物料) + resources/engines → 单 zip.
# 收件人解压后直接运行 MagicTTS.app — 首启检测到旁边 Engines/ 自动导入
# (容器 Application Support/Engines); 也可在 App 内「导入引擎数据」手动选
# 本 zip 或解压出的 Engines 目录。
set -euo pipefail
cd "$(dirname "$0")/.."

if [ ! -d "resources/engines" ]; then
    echo "错误: 缺少 resources/engines (备份/开发源, 见 README)"; exit 1
fi

./scripts/build-clt.sh

STAGE="$(mktemp -d)/dist"
mkdir -p "$STAGE"
cp -Rc build/MagicTTS.app "$STAGE/MagicTTS.app"
cp -Rc resources/engines "$STAGE/Engines"

OUT="build/MagicTTS-dist.zip"
rm -f "$OUT"
( cd "$STAGE" && /usr/bin/zip -q -r -y "$OLDPWD/$OUT" MagicTTS.app Engines )
rm -rf "$(dirname "$STAGE")"

echo
echo "打包完成: $OUT ($(du -h "$OUT" | cut -f1))"
echo "解压后直接运行 MagicTTS.app 即自动导入旁边 Engines/"
