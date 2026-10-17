#!/bin/bash
# 安装引擎数据到 Magic TTS 的 App Group 容器.
# 用法: ./populate-engines.sh [源Engines目录] [目标Engines目录]
#   源默认: 旧版组容器 (com.sgr.grtts.shared) — 存量数据一次性迁移
#   目标默认: ~/Library/Group Containers/com.magictts.shared/Engines
# 仅安装三引擎: yongde / naturalvoice / onecore (其余引擎已从产品移除)
set -euo pipefail

SRC="${1:-$HOME/Library/Group Containers/com.sgr.grtts.shared/Engines}"
DST="${2:-$HOME/Library/Group Containers/com.magictts.shared/Engines}"

[ -d "$SRC" ] || { echo "错误: 源目录不存在: $SRC"; exit 1; }
mkdir -p "$DST"

for key in yongde naturalvoice onecore; do
    if [ -d "$SRC/$key" ]; then
        rm -rf "$DST/$key"
        cp -Rc "$SRC/$key" "$DST/$key"
        echo "已安装 $key: $(du -sh "$DST/$key" | cut -f1)"
    else
        echo "警告: 源中缺少 $key"
    fi
done

echo
echo "完成: $DST"
echo "从零重建数据的途径: scripts/fetch-ms-sdk.sh (MS 运行库) + scripts/fetch-voice-models.sh (语音包)"
