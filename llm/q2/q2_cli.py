#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
import sys
import urllib.error
import urllib.request
from typing import Any, Dict, Optional


DEFAULT_SERVER = "http://127.0.0.1:3184"


def request_json(method: str, url: str, payload: Optional[Dict[str, Any]] = None, timeout: float = 60.0) -> Any:
    data = None if payload is None else json.dumps(payload, ensure_ascii=False).encode("utf-8")
    req = urllib.request.Request(
        url,
        data=data,
        headers={"Content-Type": "application/json"},
        method=method,
    )
    with urllib.request.urlopen(req, timeout=timeout) as response:
        return json.loads(response.read().decode("utf-8"))


def print_json(data: Any) -> None:
    print(json.dumps(data, ensure_ascii=False, indent=2))


def main() -> int:
    parser = argparse.ArgumentParser(description="q2 API test client")
    parser.add_argument("--server", default=DEFAULT_SERVER)
    parser.add_argument("--timeout", type=float, default=60.0)
    sub = parser.add_subparsers(dest="command", required=True)
    sub.add_parser("health")
    sub.add_parser("schema")
    sub.add_parser("cache-stats")
    sub.add_parser("cache-clear")
    plan = sub.add_parser("plan")
    plan.add_argument("question")
    query = sub.add_parser("query")
    query.add_argument("question")
    args = parser.parse_args()

    base = args.server.rstrip("/")
    try:
        if args.command == "health":
            print_json(request_json("GET", f"{base}/health", timeout=args.timeout))
        elif args.command == "schema":
            print_json(request_json("GET", f"{base}/api/schema", timeout=args.timeout))
        elif args.command == "cache-stats":
            print_json(request_json("GET", f"{base}/api/cache/stats", timeout=args.timeout))
        elif args.command == "cache-clear":
            print_json(request_json("POST", f"{base}/api/cache/clear", {}, args.timeout))
        elif args.command == "plan":
            print_json(request_json("POST", f"{base}/api/mongo-plan", {"question": args.question}, args.timeout))
        elif args.command == "query":
            print_json(request_json("POST", f"{base}/api/mongo-query", {"question": args.question}, args.timeout))
        return 0
    except urllib.error.URLError as exc:
        print(f"q2_cli error: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
