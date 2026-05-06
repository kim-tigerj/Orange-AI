#!/bin/bash
# Continuous q1 model self-play test loop.
# Stop file: /tmp/q1_auto_stop  (touch this to stop after current iteration)

set -u
cd "$(dirname "$0")/../.."

PY=${Q1_AUTO_PY:-venv/bin/python}
STOP_FILE=${Q1_AUTO_STOP_FILE:-/tmp/q1_auto_stop}
CYCLES=${Q1_AUTO_CYCLES:-3}
SLEEP_SECONDS=${Q1_AUTO_SLEEP_SECONDS:-10}
VALIDATE_EVERY=${Q1_AUTO_VALIDATE_EVERY:-1}
MAX_ITERS=${Q1_AUTO_MAX_ITERS:-0}
ITER=0

count_json() {
  find "$1" -maxdepth 1 -name '*.json' 2>/dev/null | wc -l | tr -d ' '
}

health_snapshot() {
  curl -sS --max-time 5 http://127.0.0.1:8765/health 2>&1 || true
}

echo "===== q1 auto loop boot $(date '+%Y-%m-%d %H:%M:%S') ====="
echo "cycles_per_iter=$CYCLES validate_every=$VALIDATE_EVERY sleep_seconds=$SLEEP_SECONDS max_iters=$MAX_ITERS"

while true; do
  ITER=$((ITER+1))
  echo
  echo "===== iter $ITER start $(date '+%Y-%m-%d %H:%M:%S') ====="

  if [ -f "$STOP_FILE" ]; then
    echo "stop file detected: $STOP_FILE"
    rm -f "$STOP_FILE"
    break
  fi
  if [ "$MAX_ITERS" -gt 0 ] && [ "$ITER" -gt "$MAX_ITERS" ]; then
    echo "max iterations reached: $MAX_ITERS"
    break
  fi

  echo "--- health before ---"
  health_snapshot

  echo "--- self-play-model cycles=$CYCLES ---"
  "$PY" llm/q1/q1.py --self-play-model --cycles "$CYCLES" || true

  echo "--- process pending proposals ---"
  for f in llm/q1/proposal/pending/*.json; do
    [ -e "$f" ] || continue
    id=$(basename "$f" .json)
    echo "--- apply $id ---"
    "$PY" llm/q1/q1.py --apply-proposal "$id" || true
  done

  echo "--- export dataset ---"
  "$PY" llm/q1/q1.py --export-lora-dataset || true

  if [ "$VALIDATE_EVERY" -gt 0 ] && [ $((ITER % VALIDATE_EVERY)) -eq 0 ]; then
    echo "--- validate ---"
    "$PY" llm/q1/q1.py --validate || true
  fi

  echo "--- health after ---"
  health_snapshot
  echo "counts: pending=$(count_json llm/q1/proposal/pending) accepted=$(count_json llm/q1/proposal/accepted) rejected=$(count_json llm/q1/proposal/rejected)"
  echo "===== iter $ITER end $(date '+%Y-%m-%d %H:%M:%S') ====="
  sleep "$SLEEP_SECONDS"
done

echo "===== q1 auto loop exit $(date '+%Y-%m-%d %H:%M:%S') ====="
