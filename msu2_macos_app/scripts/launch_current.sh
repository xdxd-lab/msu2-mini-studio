#!/bin/bash
set -euo pipefail
APP_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUNDLE="$APP_ROOT/build/MSU2 Mini Studio.app"
if [[ ! -x "$BUNDLE/Contents/MacOS/MSU2MiniStudio" ]]; then
    printf '找不到目前版本：%s\n' "$BUNDLE" >&2
    exit 1
fi
printf '開啟目前版本：%s\n' "$BUNDLE"
exec /usr/bin/open -n -a "$BUNDLE" --args "$@"
