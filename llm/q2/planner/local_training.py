from __future__ import annotations

import hashlib
import json
import os
import re
from datetime import datetime, timezone
from typing import Any

from pymongo import UpdateOne
from pymongo.errors import PyMongoError

from .mongo_client import store_mongo_client_from_env, store_mongo_uri_from_env


TRAINING_COLLECTION = "local_lm_training_example"
HARD_CASE_COLLECTION = "local_lm_hard_case"


def utc_now() -> datetime:
    return datetime.now(timezone.utc).replace(tzinfo=None)


def normalize_question(question: str) -> str:
    return re.sub(r"\s+", " ", (question or "").strip()).lower()


def question_key(question: str) -> str:
    return hashlib.sha256(normalize_question(question).encode("utf-8")).hexdigest()


def plan_brief(plan: dict[str, Any]) -> dict[str, Any]:
    return {
        "collection": plan.get("collection"),
        "group_by": plan.get("group_by") or [],
        "metrics": [
            {"name": item.get("name"), "op": item.get("op"), "field": item.get("field")}
            for item in plan.get("metrics", [])
        ],
        "filters": [
            {"field": item.get("field"), "op": item.get("op"), "value": item.get("value")}
            for item in plan.get("filters", [])
        ],
        "sort": plan.get("sort") or [],
    }


def ensure_training_indexes(collection) -> None:
    try:
        collection.create_index([("project", 1), ("question_key", 1)], unique=True)
        collection.create_index([("project", 1), ("collection", 1), ("priority", -1), ("last_seen", -1)])
        collection.create_index([("project", 1), ("last_seen", -1)])
    except PyMongoError:
        pass


def ensure_hard_case_indexes(collection) -> None:
    try:
        collection.create_index([("project", 1), ("question_key", 1)], unique=True)
        collection.create_index([("project", 1), ("status", 1), ("updated_at", -1)])
        collection.create_index([("project", 1), ("collection", 1), ("updated_at", -1)])
    except PyMongoError:
        pass


def build_training_examples(limit: int = 600) -> dict[str, Any]:
    project = os.environ.get("Q2_PROJECT", "q2")
    client = store_mongo_client_from_env()
    _, db_name = store_mongo_uri_from_env()
    db = client[db_name]
    target = db[TRAINING_COLLECTION]
    ensure_training_indexes(target)
    rows = list(db["llm_query"].find(
        {
            "project": project,
            "ok": True,
            "quality_status": {"$in": ["ok", None, ""]},
            "plan": {"$type": "object"},
        },
        {
            "_id": 0,
            "created_at": 1,
            "question": 1,
            "backend": 1,
            "collection": 1,
            "plan": 1,
            "result_count": 1,
            "cache_hit": 1,
        },
    ).sort("created_at", -1).limit(max(1, min(limit, 5000))))
    seen: dict[str, dict[str, Any]] = {}
    counts: dict[str, int] = {}
    for row in rows:
        question = str(row.get("question") or "").strip()
        plan = row.get("plan") or {}
        if not question or not plan.get("collection"):
            continue
        key = question_key(question)
        counts[key] = counts.get(key, 0) + 1
        if key not in seen:
            seen[key] = row
    operations = []
    now = utc_now()
    for key, row in seen.items():
        plan = row.get("plan") or {}
        collection = plan.get("collection")
        document = {
            "project": project,
            "question_key": key,
            "question": row.get("question"),
            "collection": collection,
            "plan": plan,
            "plan_brief": plan_brief(plan),
            "source_backend": row.get("backend"),
            "source_count": counts.get(key, 1),
            "result_count": row.get("result_count"),
            "last_seen": row.get("created_at") or now,
            "updated_at": now,
            "priority": priority_for_example(row),
        }
        operations.append(UpdateOne(
            {"project": project, "question_key": key},
            {"$set": document, "$setOnInsert": {"created_at": now}},
            upsert=True,
        ))
    if operations:
        target.bulk_write(operations, ordered=False)
    total = target.count_documents({"project": project})
    by_collection = list(target.aggregate([
        {"$match": {"project": project}},
        {"$group": {"_id": "$collection", "count": {"$sum": 1}}},
        {"$sort": {"count": -1}},
    ]))
    return {
        "ok": True,
        "source_rows": len(rows),
        "upserted": len(operations),
        "total_examples": total,
        "by_collection": [
            {"collection": row.get("_id") or "unknown", "count": row.get("count", 0)}
            for row in by_collection
        ],
    }


def priority_for_example(row: dict[str, Any]) -> int:
    plan = row.get("plan") or {}
    score = 0
    if row.get("backend") == "template":
        score += 30
    elif row.get("backend") == "cache":
        score += 20
    if row.get("result_count", 0) not in {None, 0}:
        score += 10
    score += min(10, len(plan.get("filters") or []))
    score += min(10, len(plan.get("group_by") or []))
    return score


def score_example(question: str, example: dict[str, Any]) -> int:
    q = normalize_question(question)
    eq = normalize_question(example.get("question") or "")
    score = int(example.get("priority") or 0)
    for token in re.findall(r"[A-Za-z0-9가-힣_.+-]{2,}", q):
        if token in eq:
            score += 8
    collection = example.get("collection")
    if collection == "nodeinfo" and any(word in q for word in ["설치", "버전", "윈도", "드라이버", "백신", "장비 수", "제조사", "모델"]):
        score += 20
    if collection == "node" and any(word in q for word in ["누구", "사람", "현재", "온라인", "오프라인", "장비 몇", "장비 수", "메모리 제일"]):
        score += 20
    if collection == "sprocess" and any(word in q for word in ["프로세스", "부하", "cpu", "메모리 평균", "핸들", "크래시"]):
        score += 20
    if collection == "filelist" and any(word in q for word in ["파일", "생성", "설치된 파일", "서명 없는 파일"]):
        score += 20
    return score


def load_training_examples(question: str, limit: int = 8) -> list[dict[str, Any]]:
    try:
        project = os.environ.get("Q2_PROJECT", "q2")
        client = store_mongo_client_from_env()
        _, db_name = store_mongo_uri_from_env()
        collection = client[db_name][TRAINING_COLLECTION]
        rows = list(collection.find(
            {"project": project},
            {"_id": 0, "question": 1, "collection": 1, "plan": 1, "priority": 1, "last_seen": 1},
        ).sort("priority", -1).limit(300))
    except Exception:
        return []
    rows.sort(key=lambda row: score_example(question, row), reverse=True)
    selected: list[dict[str, Any]] = []
    seen_collections: set[str] = set()
    for row in rows:
        if len(selected) >= limit:
            break
        collection = row.get("collection") or ""
        if collection in seen_collections and len(selected) < 4:
            continue
        selected.append(row)
        seen_collections.add(collection)
    return selected


def record_hard_case(
    question: str,
    reference_plan: dict[str, Any],
    local_plan: dict[str, Any] | None = None,
    comparison: dict[str, Any] | None = None,
    local_errors: list[str] | None = None,
) -> dict[str, Any]:
    project = os.environ.get("Q2_PROJECT", "q2")
    client = store_mongo_client_from_env()
    _, db_name = store_mongo_uri_from_env()
    collection = client[db_name][HARD_CASE_COLLECTION]
    ensure_hard_case_indexes(collection)
    now = utc_now()
    key = question_key(question)
    document = {
        "project": project,
        "question_key": key,
        "question": question,
        "collection": reference_plan.get("collection"),
        "reference_plan": reference_plan,
        "reference_plan_brief": plan_brief(reference_plan),
        "last_local_plan": local_plan,
        "last_comparison": comparison or {},
        "last_local_errors": local_errors or [],
        "status": "open",
        "updated_at": now,
    }
    collection.update_one(
        {"project": project, "question_key": key},
        {"$set": document, "$setOnInsert": {"created_at": now}, "$inc": {"failure_count": 1}},
        upsert=True,
    )
    return {"ok": True, "question_key": key}


def mark_hard_case_resolved(question: str) -> None:
    try:
        project = os.environ.get("Q2_PROJECT", "q2")
        client = store_mongo_client_from_env()
        _, db_name = store_mongo_uri_from_env()
        collection = client[db_name][HARD_CASE_COLLECTION]
        ensure_hard_case_indexes(collection)
        collection.update_one(
            {"project": project, "question_key": question_key(question)},
            {"$set": {"status": "resolved", "resolved_at": utc_now(), "updated_at": utc_now()}},
        )
    except Exception:
        pass


def load_hard_cases(question: str, limit: int = 4) -> list[dict[str, Any]]:
    try:
        project = os.environ.get("Q2_PROJECT", "q2")
        client = store_mongo_client_from_env()
        _, db_name = store_mongo_uri_from_env()
        collection = client[db_name][HARD_CASE_COLLECTION]
        rows = list(collection.find(
            {"project": project, "status": "open"},
            {"_id": 0, "question": 1, "collection": 1, "reference_plan": 1, "failure_count": 1, "updated_at": 1},
        ).sort("updated_at", -1).limit(200))
    except Exception:
        return []
    rows.sort(key=lambda row: score_example(question, {
        "question": row.get("question"),
        "collection": row.get("collection"),
        "plan": row.get("reference_plan"),
        "priority": 80 + int(row.get("failure_count") or 0),
    }), reverse=True)
    return rows[:limit]


def inject_training_examples(base_prompt: str, question: str) -> str:
    examples = load_training_examples(question)
    hard_cases = load_hard_cases(question)
    if not examples and not hard_cases:
        return base_prompt
    blocks = [
        "## q2 Hard Case Corrections",
        "",
        "These are questions the local model previously answered poorly. For similar requests, copy the corrected QueryPlan pattern exactly.",
        "",
    ]
    for index, case in enumerate(hard_cases, start=1):
        blocks.extend([
            f"### Correction {index}",
            f"Question: {case.get('question')}",
            "Correct QueryPlan:",
            json.dumps(case.get("reference_plan") or {}, ensure_ascii=False, separators=(",", ":")),
            "",
        ])
    blocks.extend([
        "## Validated q2 Training Examples",
        "",
        "The following examples were produced by validated q2 history. Follow these patterns over generic language intuition.",
        "Use the same collection/field/metric style when the user's request is similar.",
        "",
    ])
    for index, example in enumerate(examples, start=1):
        plan = example.get("plan") or {}
        blocks.extend([
            f"### Example {index}",
            f"Question: {example.get('question')}",
            "QueryPlan:",
            json.dumps(plan, ensure_ascii=False, separators=(",", ":")),
            "",
        ])
    training = "\n".join(blocks)
    marker = "## User Question"
    if marker in base_prompt:
        return base_prompt.replace(marker, f"{training}\n{marker}", 1)
    return f"{base_prompt}\n\n{training}"
