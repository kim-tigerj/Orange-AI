#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")"

BASE_URL="${Q2_AUTOTEST_SERVER:-http://127.0.0.1:3184}"

for _ in $(seq 1 30); do
  if curl -fsS --max-time 2 "$BASE_URL/health" >/dev/null; then
    break
  fi
  sleep 2
done

curl -fsS --max-time 2 "$BASE_URL/health" >/dev/null

exec ./venv/bin/python llm/q2/q2_autotest_daemon.py \
  --server "$BASE_URL" \
  --timeout "${Q2_AUTOTEST_TIMEOUT:-220}" \
  --rate "${Q2_AUTOTEST_RATE:-1}" \
  --pool-size "${Q2_AUTOTEST_POOL_SIZE:-100}" \
  --generated "${Q2_AUTOTEST_GENERATED:-59}" \
  --reference-time "${Q2_AUTOTEST_REFERENCE_TIME:-dataset}" \
  --output "${Q2_AUTOTEST_OUTPUT:-llm/q2/output/autotest_stream_latest.json}"
