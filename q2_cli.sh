#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")"
exec ./venv/bin/python llm/q2/q2_cli.py "$@"
