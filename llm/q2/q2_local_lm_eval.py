from __future__ import annotations

import os
import time
from datetime import datetime, timezone
from typing import Any

from llm.q2.planner.catalog import load_catalog
from llm.q2.planner.local_training import build_training_examples
from llm.q2.planner.mongo_client import insert_q2_document
from llm.q2.planner.ollama_backend import OllamaPlanner
from llm.q2.planner.template_planner import plan_from_template
from llm.q2.planner.validator import validate_plan


DEFAULT_QUESTIONS = [
    "최원용 장비 대수",
    "Chrome 설치 버전별 장비 수를 보여줘",
    "우리 회사에서 메모리 제일 많이 쓰는 사람은?",
    "삼성 장비를 가진 노드 찾아줘",
    "PowerShell 부하 높은 프로세스 보여줘",
    "오늘 새로 설치된 파일 목록을 보여줘",
]


def utc_now() -> datetime:
    return datetime.now(timezone.utc).replace(tzinfo=None)


def signature(plan: Any) -> dict[str, Any] | None:
    if plan is None:
        return None
    data = plan.model_dump(mode="json") if hasattr(plan, "model_dump") else dict(plan)
    return {
        "collection": data.get("collection"),
        "group_by": data.get("group_by") or [],
        "metrics": [
            {"op": item.get("op"), "field": item.get("field")}
            for item in data.get("metrics", [])
        ],
        "filters": [
            {"field": item.get("field"), "op": item.get("op"), "value": item.get("value")}
            for item in data.get("filters", [])
        ],
    }


def compare(reference: Any, local: Any) -> dict[str, Any]:
    ref = signature(reference)
    got = signature(local)
    if not ref or not got:
        return {"semantic_same": False, "reason": "missing_plan"}
    return {
        "semantic_same": ref == got,
        "collection_match": ref.get("collection") == got.get("collection"),
        "group_by_match": ref.get("group_by") == got.get("group_by"),
        "metric_semantic_match": ref.get("metrics") == got.get("metrics"),
        "filter_match": ref.get("filters") == got.get("filters"),
    }


def run_evaluation() -> dict[str, Any]:
    catalog = load_catalog()
    planner = OllamaPlanner()
    training = build_training_examples(limit=int(os.environ.get("Q2_LOCAL_EVAL_TRAIN_LIMIT", "1500")))
    run_id = f"local-eval-{utc_now().strftime('%Y%m%d%H%M%S')}"
    rows = []
    for question in DEFAULT_QUESTIONS:
        started = time.monotonic()
        reference = plan_from_template(question)
        local = planner.plan(question)
        local_errors = validate_plan(local.plan, catalog) if local.plan else []
        comparison = compare(reference, local.plan)
        ok = bool(local.plan and not local_errors and comparison.get("semantic_same"))
        rows.append({
            "question": question,
            "reference_collection": reference.collection if reference else None,
            "local_collection": local.plan.collection if local.plan else None,
            "local_ok": bool(local.plan and not local_errors),
            "semantic_same": ok,
            "elapsed_ms": local.elapsed_ms or round((time.monotonic() - started) * 1000, 3),
            "local_error": local.error,
            "local_errors": local_errors,
            "comparison": comparison,
            "reference_plan": reference.model_dump(mode="json") if reference else None,
            "local_plan": local.plan.model_dump(mode="json") if local.plan else None,
        })
    passed = sum(1 for row in rows if row["semantic_same"])
    valid = sum(1 for row in rows if row["local_ok"])
    document = {
        "run_id": run_id,
        "project": os.environ.get("Q2_PROJECT", "q2"),
        "created_at": utc_now(),
        "local_model": planner.model,
        "training_examples": training.get("total_examples", 0),
        "total": len(rows),
        "passed": passed,
        "valid": valid,
        "pass_ratio": round(passed / len(rows), 4) if rows else 0,
        "valid_ratio": round(valid / len(rows), 4) if rows else 0,
        "avg_elapsed_ms": round(sum(row["elapsed_ms"] for row in rows) / len(rows), 3) if rows else 0,
        "rows": rows,
    }
    insert_q2_document("local_lm_eval_run", document)
    return document


def main() -> None:
    result = run_evaluation()
    print(f"run_id={result['run_id']} passed={result['passed']}/{result['total']} valid={result['valid']}/{result['total']} avg_ms={result['avg_elapsed_ms']}")
    for row in result["rows"]:
        print(f"{'OK' if row['semantic_same'] else 'GAP'} {row['reference_collection']}->{row['local_collection']} {row['elapsed_ms']}ms {row['question']}")


if __name__ == "__main__":
    main()
