#!/bin/bash
# 安装引擎数据到 Magic TTS 的 App Group 容器.
# 用法: ./populate-engines.sh [MS引擎数据源目录] [目标Engines目录]
#   yongde       ← 项目内 resources/yongde (自包含, 无需外部素材)
#   naturalvoice/onecore ← MS引擎数据源 (默认旧版组容器一次性迁移;
#                          从零重建用 fetch-ms-sdk.sh + fetch-voice-models.sh)
#   目标默认: ~/Library/Group Containers/com.magictts.shared/Engines
set -euo pipefail

PROJ="$(cd "$(dirname "$0")/.." && pwd)"
SRC_MS="${1:-$HOME/Library/Group Containers/com.sgr.grtts.shared/Engines}"
DST="${2:-$HOME/Library/Group Containers/com.magictts.shared/Engines}"

mkdir -p "$DST"

# —— 永德: 项目资源 (5 个 .vl 音色 + 词典 + 引擎 .so 参考) ——
if [ -d "$PROJ/resources/yongde/voices" ]; then
    rm -rf "$DST/yongde"
    cp -Rc "$PROJ/resources/yongde" "$DST/yongde"
    echo "已安装 yongde (来自项目 resources/): $(du -sh "$DST/yongde" | cut -f1)"
else
    echo "错误: 缺少 $PROJ/resources/yongde/voices"; exit 1
fi

# —— 微软嵌入式 ×2 ——
for key in naturalvoice onecore; do
    if [ -d "$SRC_MS/$key" ]; then
        rm -rf "$DST/$key"
        cp -Rc "$SRC_MS/$key" "$DST/$key"
        echo "已安装 $key (来自 $SRC_MS): $(du -sh "$DST/$key" | cut -f1)"
    else
        echo "跳过 $key: $SRC_MS/$key 不存在 (从零重建: fetch-ms-sdk.sh + fetch-voice-models.sh)"
    fi
done

echo
echo "完成: $DST"
