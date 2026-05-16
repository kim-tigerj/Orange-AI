#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")"

LOCK_DIR="llm/q2/output/q2_autotest.lock"
if ! mkdir "$LOCK_DIR" 2>/dev/null; then
  echo "q2 autotest already running"
  exit 0
fi
trap 'rmdir "$LOCK_DIR" 2>/dev/null || true' EXIT

BASE_URL="${Q2_AUTOTEST_SERVER:-http://127.0.0.1:3184}"
for _ in $(seq 1 30); do
  if curl -fsS --max-time 2 "$BASE_URL/health" >/dev/null; then
    break
  fi
  sleep 2
done

curl -fsS --max-time 2 "$BASE_URL/health" >/dev/null

exec ./venv/bin/python llm/q2/q2_autotest.py \
  --server "$BASE_URL" \
  --timeout "${Q2_AUTOTEST_TIMEOUT:-220}" \
  --generated "${Q2_AUTOTEST_GENERATED:-100}" \
  --limit "${Q2_AUTOTEST_LIMIT:-100}" \
  --reference-time "${Q2_AUTOTEST_REFERENCE_TIME:-dataset}" \
  --output "${Q2_AUTOTEST_OUTPUT:-llm/q2/output/autotest_run_latest.json}"
