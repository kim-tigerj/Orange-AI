#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
import time
import uuid
from collections import Counter, defaultdict
from datetime import datetime, timedelta, timezone
from pathlib import Path
from typing import Any


ROOT = Path(__file__).resolve().parents[2]
OUTPUT_DIR = ROOT / "llm" / "q2" / "output"
VERSION_PATH = ROOT / "llm" / "q2" / "VERSION"
sys.path.insert(0, str(ROOT))

from llm.q2.planner.mongo_client import find_q2_documents, insert_q2_document, upsert_q2_document


def utc_now() -> datetime:
    return datetime.now(timezone.utc)


def json_default(value: Any) -> str:
    if isinstance(value, datetime):
        return value.isoformat()
    return str(value)


def run_command(args: list[str], timeout: int, capture: bool = True) -> dict[str, Any]:
    started = time.monotonic()
    try:
        completed = subprocess.run(
            args,
            cwd=str(ROOT),
            text=True,
            capture_output=capture,
            timeout=timeout,
            check=False,
        )
        return {
            "ok": completed.returncode == 0,
            "returncode": completed.returncode,
            "elapsed_ms": round((time.monotonic() - started) * 1000, 3),
            "stdout": (completed.stdout or "")[-8000:] if capture else "",
            "stderr": (completed.stderr or "")[-8000:] if capture else "",
        }
    except subprocess.TimeoutExpired as exc:
        return {
            "ok": False,
            "returncode": -1,
            "elapsed_ms": round((time.monotonic() - started) * 1000, 3),
            "stdout": (exc.stdout or "")[-8000:] if isinstance(exc.stdout, str) else "",
            "stderr": f"timeout after {timeout}s",
        }


def recent_needs_review(limit: int, hours: int) -> list[dict[str, Any]]:
    since = utc_now() - timedelta(hours=hours)
    rows = find_q2_documents(
        "llm_query",
        {
            "project": os.environ.get("Q2_PROJECT", "q2"),
            "$or": [
                {"quality_status": "needs_review"},
                {"expected_match": False},
                {"ok": False},
            ],
            "created_at": {"$gte": since},
        },
        limit=limit,
        sort=[("created_at", -1)],
    )
    if rows:
        return rows
    return find_q2_documents(
        "llm_query",
        {
            "project": os.environ.get("Q2_PROJECT", "q2"),
            "$or": [
                {"quality_status": "needs_review"},
                {"expected_match": False},
                {"ok": False},
            ],
        },
        limit=limit,
        sort=[("created_at", -1)],
    )


def recent_improvement_runs(limit: int = 5) -> list[dict[str, Any]]:
    return find_q2_documents(
        "improvement_run",
        {"project": os.environ.get("Q2_PROJECT", "q2")},
        limit=limit,
        sort=[("created_at", -1)],
    )


def pending_improvement_requests(limit: int = 20) -> list[dict[str, Any]]:
    return find_q2_documents(
        "improvement_request",
        {"project": os.environ.get("Q2_PROJECT", "q2"), "status": "pending"},
        limit=limit,
        sort=[("created_at", 1)],
    )


def run_data_profiler() -> dict[str, Any]:
    env = os.environ.copy()
    return run_command(
        [
            "./venv/bin/python",
            "llm/q2/q2_data_profiler.py",
            "--sample-limit",
            "300",
        ],
        timeout=180,
    )


def latest_data_profile() -> dict[str, Any]:
    rows = find_q2_documents(
        "data_profile",
        {"project": os.environ.get("Q2_PROJECT", "q2")},
        limit=20,
        sort=[("created_at", -1)],
    )
    summary = next((row for row in rows if row.get("collection") == "_summary"), None)
    profiles = {row.get("collection"): row.get("profile") for row in rows if row.get("collection") and row.get("collection") != "_summary"}
    nodeinfo = profiles.get("nodeinfo") or {}
    compact_sections = []
    for section in nodeinfo.get("sections", [])[:20]:
        compact_sections.append(
            {
                "name": section.get("name"),
                "node_count": section.get("node_count"),
                "data_keys": section.get("data_keys", [])[:30],
                "top_products": section.get("top_products", [])[:8],
                "top_values": section.get("top_values", [])[:8],
                "top_os": section.get("top_os", [])[:8],
                "top_builds": section.get("top_builds", [])[:8],
                "top_hotfixes": section.get("top_hotfixes", [])[:8],
            }
        )
    return {
        "summary": summary,
        "collections": {
            name: {
                "count": profile.get("count"),
                "time_ranges": profile.get("time_ranges"),
                "top_keys": profile.get("top_keys", [])[:20],
            }
            for name, profile in profiles.items()
            if isinstance(profile, dict) and name != "nodeinfo"
        },
        "nodeinfo_sections": compact_sections,
    }


def summarize_rows(rows: list[dict[str, Any]]) -> dict[str, Any]:
    by_reason = Counter(row.get("failure_reason") or row.get("quality_status") or "unknown" for row in rows)
    by_collection = Counter(row.get("collection") or "none" for row in rows)
    grouped: dict[str, list[dict[str, Any]]] = defaultdict(list)
    for row in rows:
        key = f"{row.get('failure_reason') or row.get('quality_status') or 'unknown'}:{row.get('collection') or 'none'}"
        grouped[key].append(row)
    groups = []
    for key, items in sorted(grouped.items(), key=lambda item: len(item[1]), reverse=True):
        groups.append(
            {
                "key": key,
                "count": len(items),
                "examples": [
                    {
                        "question": item.get("question"),
                        "collection": item.get("collection"),
                        "backend": item.get("backend"),
                        "result_count": item.get("result_count"),
                        "failure_reason": item.get("failure_reason"),
                        "candidate_fix": item.get("candidate_fix"),
                        "errors": item.get("errors"),
                    }
                    for item in items[:5]
                ],
            }
        )
    return {
        "total": len(rows),
        "by_reason": dict(by_reason),
        "by_collection": dict(by_collection),
        "groups": groups[:8],
    }


def build_codex_prompt(
    run_id: str,
    summary: dict[str, Any],
    rows: list[dict[str, Any]],
    previous_runs: list[dict[str, Any]],
    requests: list[dict[str, Any]],
    data_profile: dict[str, Any],
) -> str:
    examples = [
        {
            "question": row.get("question"),
            "collection": row.get("collection"),
            "backend": row.get("backend"),
            "quality_status": row.get("quality_status"),
            "failure_reason": row.get("failure_reason"),
            "candidate_fix": row.get("candidate_fix"),
            "result_count": row.get("result_count"),
            "errors": row.get("errors"),
            "plan": row.get("plan"),
        }
        for row in rows[:25]
    ]
    prior = [
        {
            "run_id": row.get("run_id"),
            "status": row.get("status"),
            "summary": row.get("summary"),
            "verification": row.get("verification"),
            "error": row.get("error"),
        }
        for row in previous_runs[:3]
    ]
    request_summary = [
        {
            "request_id": row.get("request_id"),
            "candidate_id": row.get("candidate_id"),
            "question": row.get("question"),
            "instruction": row.get("instruction"),
            "created_at": row.get("created_at"),
        }
        for row in requests
    ]
    return f"""
You are the q2 autonomous improver for Orange-AI.

Goal:
- Inspect recent q2 natural-language Mongo query needs_review/failure evidence.
- Inspect pending human improvement requests from the web UI.
- Make the smallest useful code or prompt change under llm/q2.
- Prefer deterministic template/planner/display/quality fixes over vague prompt edits.
- The q2 autotest question generator is in scope. If questions are repetitive, low-value, or too similar, improve llm/q2/q2_autotest.py or llm/q2/q2_autotest_daemon.py.
- If the autotest stream reports "fresh question pool exhausted" or the recent questions are too similar, expand the question bank using endpoint management / Tanium / IT asset management patterns: company-wide installed program list, find nodes where a named program is installed, software inventory, product/version distribution, publisher distribution, patch readiness, vulnerable/old versions, antivirus/security product versions, unmanaged/offline devices, hardware inventory, OS build, publisher/company metadata, signature metadata, and per-node software state.
- Korean IT administrator questions should include concrete products commonly seen on Windows endpoints such as nProtect, V3, AhnLab, 알약, INISAFE, MagicLine, TouchEn, Office, Teams, Chrome, Edge, Firefox, Adobe, Zoom, Java, .NET, and 한컴.
- Autotest improvements are valid q2 improvements. After changing the question generator, q2 autotest must be restarted so the new question pool is used immediately.
- Do not touch model files, venv, public product data, or unrelated code.
- Keep changes narrow and testable.

Important domain rules:
- public MongoDB is product/source data and must remain read-only.
- ai MongoDB is for q2 logs/improvement data.
- system is company-wide aggregated node performance.
- node/nodeinfo are per-node inventory/spec data.
- Installed program inventory is in nodeinfo records where name == "UNINSTALL".
- nodeinfo has many per-node categories. Current known name values include AGENT, CPU, DISK, DISKDRIVE, FAN, MEMORY, MONITOR, NETWORK, NETWORKADAPTER, OS, PRINTER, SYSTEM, UNINSTALL, UPDATE, VACCINE, and VIDEOCARD.
- Use nodeinfo UPDATE for Windows hotfix/patch inventory, VACCINE for antivirus/security product state, AGENT for Orange agent version/path/build, NETWORK/NETWORKADAPTER for IP/MAC/adapter inventory, PRINTER for printer inventory, CPU/MEMORY/DISK/DISKDRIVE/VIDEOCARD/SYSTEM/OS for hardware and OS inventory.
- File inventory, file version metadata, paths, signatures, and newly created/installed files are in filelist.
- sprocess is per-process performance; CPU, Memory, IO, Handle, pscore averages must divide by CounterCount when appropriate.
- Web displayed timestamps must be local time to seconds only.
- Node ids should be enriched to human-readable node labels when possible.

Run id: {run_id}

Recent needs_review summary:
{json.dumps(summary, ensure_ascii=False, indent=2, default=json_default)}

Recent examples:
{json.dumps(examples, ensure_ascii=False, indent=2, default=json_default)}

Previous improver runs:
{json.dumps(prior, ensure_ascii=False, indent=2, default=json_default)}

Pending human improvement requests:
{json.dumps(request_summary, ensure_ascii=False, indent=2, default=json_default)}

Latest source data profile:
{json.dumps(data_profile, ensure_ascii=False, indent=2, default=json_default)}

Required behavior:
1. Edit files directly if a clear low-risk improvement exists.
2. Ground new questions/templates in the data profile. Do not invent product/category fields that are not present.
3. If no safe improvement exists, create or update a concise note in llm/q2/docs/improver_notes.md explaining why and what evidence is needed.
4. Do not ask the user questions.
5. Do not run long services.
6. Keep the final answer short: changed files, reason, tests run or recommended.
"""


def run_codex_improvement(prompt: str, timeout: int) -> dict[str, Any]:
    output_path = OUTPUT_DIR / f"improver_codex_{uuid.uuid4().hex[:8]}.txt"
    args = [
        "codex",
        "-a",
        "never",
        "--sandbox",
        "danger-full-access",
        "-C",
        str(ROOT),
        "exec",
        "--output-last-message",
        str(output_path),
        prompt,
    ]
    result = run_command(args, timeout=timeout)
    try:
        result["last_message"] = output_path.read_text(encoding="utf-8")[-12000:]
    except OSError:
        result["last_message"] = ""
    output_path.unlink(missing_ok=True)
    return result


def verify() -> dict[str, Any]:
    checks = []
    checks.append({
        "name": "py_compile",
        **run_command(
            [
                "./venv/bin/python",
                "-m",
                "py_compile",
                "llm/q2/q2_api.py",
                "llm/q2/q2_autotest.py",
                "llm/q2/q2_autotest_daemon.py",
                "llm/q2/q2_improver.py",
            ],
            timeout=60,
        ),
    })
    checks.append({
        "name": "web_js",
        **run_command(["node", "--check", "llm/q2/web/q2.js"], timeout=30),
    })
    checks.append({
        "name": "q2_autotest_smoke",
        **run_command(
            [
                "./venv/bin/python",
                "llm/q2/q2_autotest.py",
                "--server",
                "http://127.0.0.1:3184",
                "--timeout",
                "90",
                "--limit",
                "8",
                "--generated",
                "0",
                "--reference-time",
                "dataset",
            ],
            timeout=180,
        ),
    })
    return {"ok": all(item["ok"] for item in checks), "checks": checks}


def restart_q2() -> dict[str, Any]:
    user_id = str(os.getuid())
    restart = run_command(["launchctl", "kickstart", "-k", f"gui/{user_id}/com.orangelabs.q2"], timeout=30)
    health = {"ok": False, "stdout": "", "stderr": "health not checked"}
    deadline = time.monotonic() + 45
    while time.monotonic() < deadline:
        health = run_command(["curl", "-fsS", "--max-time", "2", "http://127.0.0.1:3184/health"], timeout=5)
        if health["ok"]:
            break
        time.sleep(1)
    return {"ok": restart["ok"] and health["ok"], "restart": restart, "health": health}


def restart_q2_autotest() -> dict[str, Any]:
    user_id = str(os.getuid())
    return run_command(["launchctl", "kickstart", "-k", f"gui/{user_id}/com.orangelabs.q2.autotest"], timeout=30)


def restart_services() -> dict[str, Any]:
    q2 = restart_q2()
    autotest = restart_q2_autotest()
    return {"ok": q2.get("ok") and autotest.get("ok"), "q2": q2, "autotest": autotest}


def current_q2_version() -> str:
    try:
        value = VERSION_PATH.read_text(encoding="utf-8").strip()
    except OSError:
        value = "0.0.1.1"
    return value or "0.0.1.1"


def bump_self_improvement_version() -> dict[str, Any]:
    before = current_q2_version()
    parts = before.split(".")
    if len(parts) != 4 or not all(part.isdigit() for part in parts):
        parts = ["0", "0", "1", "1"]
    else:
        parts[:3] = ["0", "0", "1"]
    parts[3] = str(int(parts[3]) + 1)
    after = ".".join(parts)
    VERSION_PATH.write_text(after + "\n", encoding="utf-8")
    return {"before": before, "after": after}


def git_diff_stat() -> str:
    result = run_command(["git", "diff", "--stat", "--", "llm/q2", "q2_api.sh", "q2_autotest_stream.sh"], timeout=20)
    return result.get("stdout", "")


def save_run(document: dict[str, Any]) -> None:
    clean = dict(document)
    clean.pop("_id", None)
    upsert_q2_document("improvement_run", {"run_id": clean["run_id"]}, clean)


def mark_requests_processed(requests: list[dict[str, Any]], run_id: str, status: str, response: str) -> None:
    for item in requests:
        request_id = item.get("request_id")
        if not request_id:
            continue
        update = dict(item)
        update.pop("_id", None)
        update.update(
            {
                "status": "processed" if status == "applied" else "attempted",
                "processed_run_id": run_id,
                "processed_at": utc_now(),
                "response": response[-4000:],
            }
        )
        upsert_q2_document("improvement_request", {"request_id": request_id}, update)


def main() -> int:
    parser = argparse.ArgumentParser(description="q2 hourly autonomous improver")
    parser.add_argument("--limit", type=int, default=80)
    parser.add_argument("--hours", type=int, default=6)
    parser.add_argument("--codex-timeout", type=int, default=900)
    parser.add_argument("--dry-run", action="store_true")
    args = parser.parse_args()

    OUTPUT_DIR.mkdir(parents=True, exist_ok=True)
    run_id = uuid.uuid4().hex[:12]
    started = utc_now()
    profile_refresh = run_data_profiler()
    data_profile = latest_data_profile()
    rows = recent_needs_review(args.limit, args.hours)
    human_requests = pending_improvement_requests()
    summary = summarize_rows(rows)
    document: dict[str, Any] = {
        "run_id": run_id,
        "project": os.environ.get("Q2_PROJECT", "q2"),
        "created_at": started,
        "status": "started",
        "summary": summary,
        "profile_refresh": profile_refresh,
        "input_count": len(rows),
        "human_request_count": len(human_requests),
        "dry_run": args.dry_run,
    }
    insert_q2_document("improvement_run", document)

    if not rows and not human_requests:
        document.update({"status": "skipped", "error": "no needs_review/failure rows found", "finished_at": utc_now()})
        save_run(document)
        print(json.dumps(document, ensure_ascii=False, default=json_default))
        return 0

    prompt = build_codex_prompt(run_id, summary, rows, recent_improvement_runs(), human_requests, data_profile)
    if args.dry_run:
        (OUTPUT_DIR / f"improver_prompt_{run_id}.txt").write_text(prompt, encoding="utf-8")
        document.update({"status": "dry_run", "prompt_path": str(OUTPUT_DIR / f"improver_prompt_{run_id}.txt"), "finished_at": utc_now()})
        save_run(document)
        print(json.dumps(document, ensure_ascii=False, default=json_default))
        return 0

    codex_result = run_codex_improvement(prompt, timeout=args.codex_timeout)
    verification = verify()
    status = "verified" if codex_result["ok"] and verification["ok"] else "failed"
    version_bump = bump_self_improvement_version() if status == "verified" else None
    restart = restart_services() if status == "verified" else {"ok": False, "skipped": "verification failed"}
    status = "applied" if status == "verified" and restart["ok"] else "failed"
    codex_response = codex_result.get("last_message") or codex_result.get("stdout") or codex_result.get("stderr") or ""
    mark_requests_processed(human_requests, run_id, status, codex_response)
    document.update(
        {
            "status": status,
            "finished_at": utc_now(),
            "codex": codex_result,
            "verification": verification,
            "version_bump": version_bump,
            "restart": restart,
            "diff_stat": git_diff_stat(),
        }
    )
    save_run(document)
    print(json.dumps({"run_id": run_id, "status": status, "summary": summary, "diff_stat": document["diff_stat"]}, ensure_ascii=False, default=json_default))
    return 0 if status == "applied" else 1


if __name__ == "__main__":
    raise SystemExit(main())
