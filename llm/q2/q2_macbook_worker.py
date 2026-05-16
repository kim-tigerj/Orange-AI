#!/usr/bin/env python3
from __future__ import annotations

import argparse
import os
import random
import signal
import subprocess
import sys
import time
from concurrent.futures import ThreadPoolExecutor, as_completed
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parents[2]
Q2_DIR = Path(__file__).resolve().parent
sys.path.insert(0, str(Q2_DIR))
sys.path.insert(0, str(ROOT))

from q2_autoresearch import profile_scenarios
from q2_autotest import DEFAULT_SERVER, Scenario, dataset_reference_time, request_json
from llm.q2.planner.mongo_client import insert_q2_document


STOP = False


def utc_now() -> datetime:
    return datetime.now(timezone.utc)


def stop_signal(_signum, _frame) -> None:
    global STOP
    STOP = True


def log_event(worker: str, status: str, message: str, **fields: Any) -> None:
    document = {
        "project": os.environ.get("Q2_PROJECT", "q2"),
        "created_at": utc_now(),
        "worker": worker,
        "status": status,
        "message": message,
        **fields,
    }
    try:
        insert_q2_document("macbook_worker_event", document)
    except Exception as exc:
        print(f"macbook worker event log failed: {exc}", file=sys.stderr, flush=True)
    print(f"[{worker}] {status} {message}", flush=True)


def wait_for_q2(base_url: str, timeout: float) -> None:
    while not STOP:
        try:
            if request_json("GET", f"{base_url}/health", timeout=timeout).get("ok"):
                return
        except Exception as exc:
            log_event("health", "waiting", "q2 health wait", error=str(exc))
        time.sleep(2)


def run_profiler(sample_limit: int) -> None:
    started = time.monotonic()
    cmd = [
        sys.executable,
        str(Q2_DIR / "q2_data_profiler.py"),
        "--collections",
        "node,nodeinfo,sprocess,filelist,detect,report,timeline,system",
        "--sample-limit",
        str(sample_limit),
    ]
    try:
        result = subprocess.run(
            cmd,
            cwd=str(ROOT),
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=180,
            check=False,
        )
        status = "ok" if result.returncode == 0 else "failed"
        log_event(
            "profiler",
            status,
            "data profile refresh",
            returncode=result.returncode,
            elapsed_ms=round((time.monotonic() - started) * 1000, 3),
            stdout_tail=result.stdout[-2000:],
            stderr_tail=result.stderr[-2000:],
        )
    except Exception as exc:
        log_event("profiler", "failed", "data profile refresh crashed", error=str(exc))


def recent_worker_questions(base_url: str, timeout: float, limit: int = 1000) -> set[str]:
    try:
        data = request_json("GET", f"{base_url}/api/macbook-worker/events?worker=cache_warmer&limit={limit}", timeout=timeout)
        return {
            str(row.get("question") or "").strip()
            for row in data.get("rows", [])
            if row.get("question")
        }
    except Exception as exc:
        log_event("cache_warmer", "failed", "recent worker question lookup failed", error=str(exc))
        return set()


def execute_question(base_url: str, question: str, reference_time: str, timeout: float) -> dict[str, Any]:
    payload: dict[str, Any] = {"question": question}
    if reference_time:
        payload["reference_time"] = reference_time
    started = time.monotonic()
    try:
        data = request_json("POST", f"{base_url}/api/mongo-query", payload, timeout=timeout)
        return {
            "ok": bool(data.get("ok")),
            "collection": (data.get("plan") or {}).get("collection"),
            "quality_status": data.get("quality_status"),
            "failure_reason": data.get("failure_reason"),
            "result_count": data.get("result_count"),
            "backend": data.get("backend"),
            "elapsed_ms": round((time.monotonic() - started) * 1000, 3),
        }
    except Exception as exc:
        return {
            "ok": False,
            "quality_status": "failed",
            "failure_reason": "api_error",
            "error": str(exc),
            "elapsed_ms": round((time.monotonic() - started) * 1000, 3),
        }


def warm_cache(base_url: str, seed: int, batch_size: int, reference_time: str, timeout: float) -> None:
    try:
        scenarios = profile_scenarios(base_url, min(timeout, 30.0), seed, batch_size * 4)
    except Exception as exc:
        log_event("cache_warmer", "failed", "scenario generation failed", error=str(exc))
        return
    random.Random(seed).shuffle(scenarios)
    recent_questions = recent_worker_questions(base_url, min(timeout, 10.0))
    selected: list[Scenario] = []
    skipped = 0
    for scenario in scenarios:
        if STOP or len(selected) >= batch_size:
            break
        if scenario.question in recent_questions:
            skipped += 1
            continue
        selected.append(scenario)
    if not selected:
        log_event("cache_warmer", "idle", "no fresh cache warming questions", skipped=skipped)
        return

    concurrency = max(1, int(os.environ.get("Q2_WORKER_CONCURRENCY", "4")))

    def run_one(scenario: Scenario) -> tuple[Scenario, dict[str, Any]]:
        return scenario, execute_question(base_url, scenario.question, reference_time, timeout)

    with ThreadPoolExecutor(max_workers=concurrency) as executor:
        futures = [executor.submit(run_one, scenario) for scenario in selected]
        for future in as_completed(futures):
            if STOP:
                break
            scenario, result = future.result()
        status = "ok" if result.get("ok") and result.get("quality_status") == "ok" else "needs_review"
        log_event(
            "cache_warmer",
            status,
            "warmed natural query",
            question=scenario.question,
            category=scenario.category,
            source=scenario.source,
            **result,
        )


def retest_failures(base_url: str, batch_size: int, reference_time: str, timeout: float) -> None:
    rows: list[dict[str, Any]] = []
    for status in ["needs_review", "mismatch"]:
        try:
            data = request_json("GET", f"{base_url}/api/autoresearch/results?status={status}&limit={batch_size}", timeout=timeout)
            rows.extend(data.get("rows") or [])
        except Exception as exc:
            log_event("failure_retester", "failed", "failure lookup failed", status_filter=status, error=str(exc))
    seen: set[str] = set()
    selected: list[str] = []
    for row in rows:
        if STOP or len(selected) >= batch_size:
            break
        question = str(row.get("question") or "").strip()
        if not question or question in seen:
            continue
        seen.add(question)
        selected.append(question)
    if not selected:
        log_event("failure_retester", "idle", "no failures to retest")
        return

    row_by_question = {str(row.get("question") or "").strip(): row for row in rows}
    concurrency = max(1, int(os.environ.get("Q2_WORKER_RETEST_CONCURRENCY", "3")))

    with ThreadPoolExecutor(max_workers=concurrency) as executor:
        futures = {executor.submit(execute_question, base_url, question, reference_time, timeout): question for question in selected}
        for future in as_completed(futures):
            if STOP:
                break
            question = futures[future]
            row = row_by_question.get(question, {})
            result = future.result()
        resolved = bool(result.get("ok")) and result.get("quality_status") == "ok" and int(result.get("result_count") or 0) > 0
        log_event(
            "failure_retester",
            "resolved" if resolved else "still_failing",
            "retested previous autoresearch issue",
            question=question,
            previous_status=row.get("status"),
            previous_collection=row.get("collection"),
            **result,
        )


def main() -> int:
    parser = argparse.ArgumentParser(description="Use this MacBook as q2 repetitive-work assistant")
    parser.add_argument("--server", default=os.environ.get("Q2_WORKER_SERVER", DEFAULT_SERVER))
    parser.add_argument("--timeout", type=float, default=float(os.environ.get("Q2_WORKER_TIMEOUT", "180")))
    parser.add_argument("--batch-size", type=int, default=int(os.environ.get("Q2_WORKER_BATCH_SIZE", "120")))
    parser.add_argument("--profile-interval", type=int, default=int(os.environ.get("Q2_WORKER_PROFILE_INTERVAL", "600")))
    parser.add_argument("--warm-interval", type=int, default=int(os.environ.get("Q2_WORKER_WARM_INTERVAL", "30")))
    parser.add_argument("--retest-interval", type=int, default=int(os.environ.get("Q2_WORKER_RETEST_INTERVAL", "180")))
    parser.add_argument("--sample-limit", type=int, default=int(os.environ.get("Q2_WORKER_SAMPLE_LIMIT", "1000")))
    args = parser.parse_args()

    signal.signal(signal.SIGTERM, stop_signal)
    signal.signal(signal.SIGINT, stop_signal)

    base_url = args.server.rstrip("/")
    wait_for_q2(base_url, min(args.timeout, 10.0))
    try:
        reference_time = dataset_reference_time(base_url, min(args.timeout, 30.0)) or ""
    except Exception:
        reference_time = ""

    log_event("supervisor", "started", "macbook q2 worker started", base_url=base_url, reference_time=reference_time)
    now = time.monotonic()
    last_profile = now - args.profile_interval
    last_warm = now - args.warm_interval
    last_retest = now - args.retest_interval
    seed = int(time.time())

    while not STOP:
        now = time.monotonic()
        if now - last_profile >= args.profile_interval:
            run_profiler(args.sample_limit)
            last_profile = time.monotonic()
        if now - last_retest >= args.retest_interval:
            retest_failures(base_url, max(5, args.batch_size // 4), reference_time, args.timeout)
            last_retest = time.monotonic()
        if now - last_warm >= args.warm_interval:
            seed += 1
            warm_cache(base_url, seed, args.batch_size, reference_time, args.timeout)
            last_warm = time.monotonic()
        time.sleep(2)

    log_event("supervisor", "stopped", "macbook q2 worker stopped")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
