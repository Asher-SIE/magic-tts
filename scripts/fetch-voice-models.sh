#!/bin/bash
# 从模型分发服务器登录并下载微软嵌入式语音模型 (账户与端点还原自原版 APK).
# 还原自 APK: n2/c.smali (登录 /api/auth/login, 字段 username/password/deviceId,
# 响应含 token) + n2/v1.smali (下载 GET /grtts-auth/api/download/<path>?deviceId=
# &Bearer 认证, SHA-256 校验).
#
# 用法:
#   fetch-gr-models.sh <用户名> <密码>            # 登录并列出语音目录
#   fetch-gr-models.sh <用户名> <密码> <path...>  # 下载指定 relativePath 到临时目录
set -euo pipefail

USER="${1:?用法: $0 <用户名> <密码> [relativePath...]}"
PASS="${2:?缺少密码}"
shift 2

AUTH_BASE="https://app.gr.run:18443/grtts-auth"
DL_BASE="https://app.gr.run:8084/grtts-auth/api/download"
DEVICE="${GR_DEVICE:-macos-$(ioreg -rd1 -c IOPlatformExpertDevice | awk '/IOPlatformSerialNumber/{gsub(/"/,"",$3); print tolower($3)}' | cut -c1-20)}"
TOKEN=""

login() {
    local body resp
    body=$(python3 -c "import json,sys; print(json.dumps({'username':sys.argv[1],'password':sys.argv[2],'deviceId':sys.argv[3]}))" "$USER" "$PASS" "$DEVICE")
    resp=$(curl -sf -m 20 -X POST "$AUTH_BASE/api/auth/login" -H "Content-Type: application/json" -d "$body") \
        || { echo "登录失败 (检查用户名/密码/服务器状态)"; exit 1; }
    TOKEN=$(printf '%s' "$resp" | python3 -c "import json,sys; print(json.load(sys.stdin).get('token',''))")
    [ -n "$TOKEN" ] || { echo "响应无 token: $resp"; exit 1; }
    echo "登录成功 (deviceId=$DEVICE)"
}

catalog() {
    echo "=== 语音目录 ==="
    curl -sf -m 30 "$DL_BASE/catalog?deviceId=$DEVICE" -H "Authorization: Bearer $TOKEN" \
        | python3 -m json.tool
}

download() {
    local rel="$1" out="/tmp/gr-dl/$1"
    mkdir -p "$(dirname "$out")"
    echo "下载 $rel ..."
    curl -sf -m 600 "$DL_BASE/$rel?deviceId=$DEVICE" -H "Authorization: Bearer $TOKEN" -o "$out"
    ls -la "$out"
}

login
if [ $# -eq 0 ]; then
    catalog
    echo
    echo "把要下载的 relativePath 作为参数重跑本脚本即可, 例如:"
    echo "  $0 <用户名> <密码> naturalvoice/model/xxx.bin"
else
    for p in "$@"; do download "$p"; done
    echo "文件已下到 /tmp/gr-dl/, 拷入 Engines/<key>/model/ 后重启 App"
fi
