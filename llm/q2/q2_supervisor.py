#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
import time
from datetime import datetime, timedelta, timezone
from pathlib import Path
from typing import Any
from urllib.error import URLError
from urllib.request import Request, urlopen

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))

from llm.q2.planner.mongo_client import find_q2_documents, insert_q2_document


PROJECT = os.environ.get("Q2_PROJECT", "q2")
DEFAULT_SERVER = os.environ.get("Q2_SUPERVISOR_SERVER", "http://127.0.0.1:3184")
OUTPUT_DIR = ROOT / "llm" / "q2" / "output"


def utc_now() -> datetime:
    return datetime.now(timezone.utc)


def json_default(value: Any) -> str:
    if isinstance(value, datetime):
        return value.isoformat()
    return str(value)


def run_command(args: list[str], timeout: int = 60) -> dict[str, Any]:
    started = time.monotonic()
    try:
        completed = subprocess.run(
            args,
            cwd=str(ROOT),
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=timeout,
            check=False,
        )
        return {
            "ok": completed.returncode == 0,
            "returncode": completed.returncode,
            "elapsed_ms": round((time.monotonic() - started) * 1000, 3),
            "stdout": (completed.stdout or "")[-4000:],
            "stderr": (completed.stderr or "")[-4000:],
        }
    except subprocess.TimeoutExpired:
        return {
            "ok": False,
            "returncode": -1,
            "elapsed_ms": round((time.monotonic() - started) * 1000, 3),
            "stdout": "",
            "stderr": f"timeout after {timeout}s",
        }


def api_json(method: str, url: str, payload: dict[str, Any] | None = None, timeout: float = 5.0) -> dict[str, Any]:
    data = None
    headers = {}
    if payload is not None:
        data = json.dumps(payload).encode("utf-8")
        headers["Content-Type"] = "application/json"
    request = Request(url, data=data, headers=headers, method=method)
    with urlopen(request, timeout=timeout) as response:
        return json.loads(response.read().decode("utf-8"))


def launchctl_kick(label: str) -> dict[str, Any]:
    return run_command(["launchctl", "kickstart", "-k", f"gui/{os.getuid()}/{label}"], timeout=30)


def launchctl_state(label: str) -> dict[str, Any]:
    result = run_command(["launchctl", "print", f"gui/{os.getuid()}/{label}"], timeout=15)
    state = "unknown"
    for line in result.get("stdout", "").splitlines():
        text = line.strip()
        if text.startswith("state = "):
            state = text.removeprefix("state = ").strip()
            break
    result["state"] = state
    return result


def log_event(status: str, message: str, **fields: Any) -> None:
    document = {
        "project": PROJECT,
        "created_at": utc_now(),
        "worker": "supervisor",
        "status": status,
        "message": message,
        **fields,
    }
    try:
        insert_q2_document("macbook_worker_event", document)
    except Exception as exc:
        print(f"q2 supervisor db log failed: {exc}", file=sys.stderr, flush=True)
    print(json.dumps(document, ensure_ascii=False, default=json_default), flush=True)


def health(base_url: str) -> dict[str, Any]:
    try:
        return api_json("GET", f"{base_url}/health", timeout=3.0)
    except (OSError, URLError, TimeoutError) as exc:
        return {"ok": False, "error": str(exc)}


def autotest_status(base_url: str) -> dict[str, Any]:
    try:
        return api_json("GET", f"{base_url}/api/autotest/status", timeout=5.0)
    except Exception as exc:
        return {"ok": False, "error": str(exc)}


def recent_problem_count(hours: int) -> int:
    since = utc_now() - timedelta(hours=hours)
    rows = find_q2_documents(
        "llm_query",
        {
            "project": PROJECT,
            "created_at": {"$gte": since},
            "$or": [
                {"ok": False},
                {"quality_status": "needs_review"},
                {"expected_match": False},
            ],
        },
        limit=200,
        sort=[("created_at", -1)],
    )
    return len(rows)


def recent_slow_sprocess_count(hours: int, threshold_ms: float) -> int:
    since = utc_now() - timedelta(hours=hours)
    rows = find_q2_documents(
        "llm_query",
        {
            "project": PROJECT,
            "created_at": {"$gte": since},
            "collection": "sprocess",
            "execution_ms": {"$gte": threshold_ms},
        },
        limit=100,
        sort=[("created_at", -1)],
    )
    return len(rows)


def latest_improver_running(min_gap_minutes: int) -> bool:
    since = utc_now() - timedelta(minutes=min_gap_minutes)
    rows = find_q2_documents(
        "improvement_run",
        {"project": PROJECT, "created_at": {"$gte": since}},
        limit=5,
        sort=[("created_at", -1)],
    )
    return bool(rows)


def run_profiler() -> dict[str, Any]:
    return run_command(["./venv/bin/python", "llm/q2/q2_data_profiler.py", "--sample-limit", "500"], timeout=240)


def run_sprocess_summary() -> dict[str, Any]:
    return run_command(["./q2_sprocess_summary.sh"], timeout=300)


def run_improver() -> dict[str, Any]:
    return run_command(["./q2_improver.sh"], timeout=int(os.environ.get("Q2_SUPERVISOR_IMPROVER_TIMEOUT", "1200")))


def one_cycle(base_url: str, state: dict[str, Any], args: argparse.Namespace) -> None:
    actions: list[dict[str, Any]] = []

    for label in [
        "com.orangelabs.q2.mongo-tunnel",
        "com.orangelabs.q2",
        "com.orangelabs.q2.autotest",
        "com.orangelabs.q2.macbook-worker",
    ]:
        service = launchctl_state(label)
        if service.get("state") not in {"running", "not running"}:
            actions.append({"action": "service_state_unknown", "label": label, "state": service.get("state")})
        if label in {"com.orangelabs.q2.mongo-tunnel", "com.orangelabs.q2", "com.orangelabs.q2.autotest", "com.orangelabs.q2.macbook-worker"} and service.get("state") != "running":
            kick = launchctl_kick(label)
            actions.append({"action": "restart_service", "label": label, "result": kick})

    q2_health = health(base_url)
    if not q2_health.get("ok"):
        kick = launchctl_kick("com.orangelabs.q2")
        actions.append({"action": "restart_q2_health_failed", "health": q2_health, "result": kick})
        time.sleep(3)
        q2_health = health(base_url)

    status = autotest_status(base_url) if q2_health.get("ok") else {"ok": False}
    current = str(status.get("current_question") or "")
    if "pool exhausted" in current or "question pool exhausted" in current:
        result = launchctl_kick("com.orangelabs.q2.autotest")
        actions.append({"action": "restart_autotest_pool_exhausted", "current": current, "result": result})

    now = time.monotonic()
    if now - state.get("last_profile", 0.0) >= args.profile_interval:
        result = run_profiler()
        state["last_profile"] = time.monotonic()
        actions.append({"action": "refresh_data_profile", "result": result})

    slow_count = recent_slow_sprocess_count(args.lookback_hours, args.slow_sprocess_ms) if q2_health.get("ok") else 0
    if slow_count >= args.slow_sprocess_min and now - state.get("last_sprocess_summary", 0.0) >= args.sprocess_summary_interval:
        result = run_sprocess_summary()
        state["last_sprocess_summary"] = time.monotonic()
        actions.append({"action": "refresh_sprocess_summary", "slow_count": slow_count, "result": result})

    problem_count = recent_problem_count(args.lookback_hours) if q2_health.get("ok") else 0
    if (
        problem_count >= args.improver_min_problems
        and now - state.get("last_improver", 0.0) >= args.improver_interval
        and not latest_improver_running(args.improver_gap_minutes)
    ):
        result = run_improver()
        state["last_improver"] = time.monotonic()
        actions.append({"action": "run_improver", "problem_count": problem_count, "result": result})

    if actions:
        log_event(
            "acted",
            "q2 supervisor cycle applied actions",
            health=q2_health,
            autotest={"ok": status.get("ok"), "current_question": current, "completed": status.get("completed")},
            problem_count=problem_count,
            slow_sprocess_count=slow_count,
            actions=actions,
        )
    else:
        log_event(
            "ok",
            "q2 supervisor cycle no action needed",
            health=q2_health,
            autotest={"ok": status.get("ok"), "current_question": current, "completed": status.get("completed")},
            problem_count=problem_count,
            slow_sprocess_count=slow_count,
        )


def main() -> int:
    parser = argparse.ArgumentParser(description="q2 autonomous supervisor")
    parser.add_argument("--server", default=DEFAULT_SERVER)
    parser.add_argument("--interval", type=int, default=int(os.environ.get("Q2_SUPERVISOR_INTERVAL", "300")))
    parser.add_argument("--once", action="store_true")
    parser.add_argument("--profile-interval", type=int, default=int(os.environ.get("Q2_SUPERVISOR_PROFILE_INTERVAL", "3600")))
    parser.add_argument("--sprocess-summary-interval", type=int, default=int(os.environ.get("Q2_SUPERVISOR_SPROCESS_SUMMARY_INTERVAL", "3600")))
    parser.add_argument("--improver-interval", type=int, default=int(os.environ.get("Q2_SUPERVISOR_IMPROVER_INTERVAL", "3600")))
    parser.add_argument("--improver-gap-minutes", type=int, default=int(os.environ.get("Q2_SUPERVISOR_IMPROVER_GAP_MINUTES", "45")))
    parser.add_argument("--improver-min-problems", type=int, default=int(os.environ.get("Q2_SUPERVISOR_IMPROVER_MIN_PROBLEMS", "3")))
    parser.add_argument("--lookback-hours", type=int, default=int(os.environ.get("Q2_SUPERVISOR_LOOKBACK_HOURS", "2")))
    parser.add_argument("--slow-sprocess-ms", type=float, default=float(os.environ.get("Q2_SUPERVISOR_SLOW_SPROCESS_MS", "3000")))
    parser.add_argument("--slow-sprocess-min", type=int, default=int(os.environ.get("Q2_SUPERVISOR_SLOW_SPROCESS_MIN", "2")))
    args = parser.parse_args()

    OUTPUT_DIR.mkdir(parents=True, exist_ok=True)
    started_at = time.monotonic()
    state = {
        "last_profile": started_at,
        "last_sprocess_summary": started_at,
        "last_improver": started_at,
    }
    log_event("started", "q2 supervisor started", interval=args.interval, once=args.once)
    while True:
        try:
            one_cycle(args.server.rstrip("/"), state, args)
        except Exception as exc:
            log_event("failed", "q2 supervisor cycle crashed", error=str(exc))
        if args.once:
            break
        time.sleep(max(10, args.interval))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
