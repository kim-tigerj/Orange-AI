#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
import sys
import urllib.request
from pathlib import Path
from typing import Any, Dict, List


DEFAULT_SERVER = "http://127.0.0.1:3184"
DEFAULT_CASES = Path(__file__).resolve().parent / "eval_prompts.jsonl"


def post_json(url: str, payload: Dict[str, Any], timeout: float) -> Dict[str, Any]:
    req = urllib.request.Request(
        url,
        data=json.dumps(payload, ensure_ascii=False).encode("utf-8"),
        headers={"Content-Type": "application/json"},
        method="POST",
    )
    with urllib.request.urlopen(req, timeout=timeout) as response:
        return json.loads(response.read().decode("utf-8"))


def load_cases(path: Path) -> List[Dict[str, Any]]:
    cases: List[Dict[str, Any]] = []
    for line in path.read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if line:
            cases.append(json.loads(line))
    return cases


def contains_all(actual: List[str], expected: List[str]) -> bool:
    return set(expected).issubset(set(actual))


def metric_fields(plan: Dict[str, Any]) -> List[str]:
    return [metric["field"] for metric in plan.get("metrics", [])]


def filter_fields(plan: Dict[str, Any]) -> List[str]:
    return [item["field"] for item in plan.get("filters", [])]


def main() -> int:
    parser = argparse.ArgumentParser(description="q2 planner evaluation runner")
    parser.add_argument("--server", default=DEFAULT_SERVER)
    parser.add_argument("--cases", type=Path, default=DEFAULT_CASES)
    parser.add_argument("--timeout", type=float, default=90.0)
    parser.add_argument("--execute", action="store_true")
    args = parser.parse_args()

    endpoint = "/api/mongo-query" if args.execute else "/api/mongo-plan"
    failures = 0
    for case in load_cases(args.cases):
        data = post_json(f"{args.server.rstrip('/')}{endpoint}", {"question": case["request"]}, args.timeout)
        plan = data.get("plan") or {}
        ok = bool(data.get("ok"))
        checks = [
            ok,
            plan.get("collection") == case["expected_collection"],
            contains_all(plan.get("group_by", []), case.get("expected_group_by", [])),
            contains_all(metric_fields(plan), case.get("expected_metrics", [])),
            contains_all(filter_fields(plan), case.get("expected_filters", [])),
        ]
        passed = all(checks)
        failures += 0 if passed else 1
        status = "PASS" if passed else "FAIL"
        print(f"{status} {case['id']} backend={data.get('backend')} cache={data.get('cache_hit')}")
        if not passed:
            print(json.dumps({"expected": case, "actual": plan, "errors": data.get("errors")}, ensure_ascii=False, indent=2))

    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
