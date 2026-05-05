#!/bin/bash

# 정팀장 CLI 실행 자동화 스크립트 (q1.sh)
# M4 Max 128GB 환경 최적화

# 프로젝트 루트 경로 계산 (llm/q1 기준 두 단계 위)
SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
PROJECT_ROOT="$(dirname "$(dirname "$SCRIPT_DIR")")"
VENV_PATH="$PROJECT_ROOT/venv"

# 1. 가상환경 확인 및 활성화
if [ -d "$VENV_PATH" ]; then
    source "$VENV_PATH/bin/activate"
else
    echo -e "\033[31m[ERROR] 가상환경(venv)을 찾을 수 없습니다: $VENV_PATH\033[0m"
    exit 1
fi

# 2. 정팀장(q1.py) 실행
# 현재 디렉토리를 llm/q1으로 유지하며 실행
cd "$SCRIPT_DIR"
python3 q1.py

# 3. 종료 시 가상환경 해제 (선택 사항)
deactivate
