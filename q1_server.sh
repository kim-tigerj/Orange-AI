#!/bin/bash
# Orange q1 model server launcher
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PYTHON_EXEC="$SCRIPT_DIR/venv/bin/python3"
SERVER_PATH="$SCRIPT_DIR/llm/q1/q1_server.py"

if [ ! -f "$PYTHON_EXEC" ]; then
    echo "에러: venv를 찾을 수 없습니다. 설정이 필요합니다." >&2
    exit 1
fi

if [ ! -f "$SERVER_PATH" ]; then
    echo "에러: q1_server.py를 찾을 수 없습니다: $SERVER_PATH" >&2
    exit 1
fi

HAS_DASHBOARD=0
for arg in "$@"; do
    if [ "$arg" = "--dashboard" ] || [[ "$arg" == --dashboard=* ]]; then
        HAS_DASHBOARD=1
        break
    fi
done

if [ "$HAS_DASHBOARD" -eq 0 ]; then
    set -- --dashboard always "$@"
fi

exec "$PYTHON_EXEC" "$SERVER_PATH" "$@"
