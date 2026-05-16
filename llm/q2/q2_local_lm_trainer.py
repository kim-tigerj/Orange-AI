#!/usr/bin/env python3
from __future__ import annotations

import argparse
import os
import random
import signal
import sys
import time
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parents[2]
Q2_DIR = Path(__file__).resolve().parent
sys.path.insert(0, str(Q2_DIR))
sys.path.insert(0, str(ROOT))

from q2_autoresearch import profile_scenarios
from q2_autotest import DEFAULT_SERVER, Scenario, dataset_reference_time, request_json
from q2_autotest_daemon import claim_question, intent_fingerprint, load_claimed_questions, load_seen_questions, normalize_question
from llm.q2.planner.catalog import load_catalog
from llm.q2.planner.local_training import build_training_examples, mark_hard_case_resolved, record_hard_case
from llm.q2.planner.mongo_client import find_q2_documents, insert_q2_document
from llm.q2.planner.ollama_backend import OllamaPlanner
from llm.q2.planner.validator import validate_plan
from llm.q2.q2_local_lm_eval import DEFAULT_QUESTIONS as EVAL_QUESTIONS
from llm.q2.q2_local_lm_eval import compare, run_evaluation


STOP = False


def utc_now() -> datetime:
    return datetime.now(timezone.utc).replace(tzinfo=None)


def stop_signal(_signum, _frame) -> None:
    global STOP
    STOP = True


def log_event(status: str, message: str, **fields: Any) -> None:
    document = {
        "project": os.environ.get("Q2_PROJECT", "q2"),
        "created_at": utc_now(),
        "worker": "local_lm_trainer",
        "status": status,
        "message": message,
        **fields,
    }
    try:
        insert_q2_document("local_lm_trainer_event", document)
    except Exception as exc:
        print(f"local lm trainer event log failed: {exc}", file=sys.stderr, flush=True)
    print(f"[local_lm_trainer] {status} {message}", flush=True)


def wait_for_q2(base_url: str, timeout: float) -> bool:
    while not STOP:
        try:
            data = request_json("GET", f"{base_url}/health", timeout=timeout)
            return bool(data.get("ok"))
        except Exception as exc:
            log_event("waiting", "q2 health wait", error=str(exc))
            time.sleep(5)
    return False


def load_reference_time(base_url: str, timeout: float) -> str:
    try:
        return dataset_reference_time(base_url, min(timeout, 30.0))
    except Exception as exc:
        log_event("reference_time_failed", "failed to load source reference time; continuing without it", error=str(exc))
        return ""


def unique_scenarios(scenarios: list[Scenario], limit: int, seen: set[str] | None = None, seen_intents: set[str] | None = None) -> list[Scenario]:
    selected: list[Scenario] = []
    seen = seen if seen is not None else set()
    seen_intents = seen_intents if seen_intents is not None else set()
    for scenario in scenarios:
        question = scenario.question.strip()
        question_key = normalize_question(question)
        intent_key = intent_fingerprint(question)
        if not question_key or question_key in seen or intent_key in seen_intents:
            continue
        seen.add(question_key)
        seen_intents.add(intent_key)
        selected.append(scenario)
        if len(selected) >= limit:
            break
    return selected


def open_hard_case_scenario() -> Scenario | None:
    try:
        rows = find_q2_documents(
            "local_lm_hard_case",
            {"project": os.environ.get("Q2_PROJECT", "q2"), "status": "open"},
            limit=1,
            sort=[("updated_at", 1)],
        )
    except Exception as exc:
        log_event("hard_case_load_failed", "failed to load local LM hard case", error=str(exc))
        return None
    if not rows:
        return None
    row = rows[0]
    question = str(row.get("question") or "").strip()
    if not question:
        return None
    collection = str(row.get("collection") or "unknown")
    return Scenario(
        category="local-hard-case",
        question=question,
        expected_collections=(collection,),
        source="local_lm:hard_case",
    )


def challenge_variant(question: str, seed: int) -> str:
    text = question.strip()
    prefixes = [
        "",
        "운영 점검 관점에서 ",
        "장애 대응 관점에서 ",
        "자산 관리 관점에서 ",
        "현황 파악용으로 ",
        "관리자 보고용으로 ",
    ]
    variants = [
        "{text} 결과를 표로 정리해줘",
        "{text} 전체 목록으로 보여줘",
        "{text} 기준으로 다시 분석해줘",
        "{text} 자세히 찾아줘",
        "{text} 현황을 알려줘",
        "{text} 관련 노드를 찾아줘",
        "{text} 기준 상위 항목을 보여줘",
        "{text} 분포를 확인해줘",
        "{text} 대상 목록을 뽑아줘",
        "{text} 요약과 목록을 같이 보여줘",
    ]
    prefix = prefixes[(seed // len(variants)) % len(prefixes)]
    body = variants[seed % len(variants)].format(text=text)
    return f"{prefix}{body}".strip()


def resolved_hard_case_variant_scenario(seed: int, seen: set[str]) -> Scenario | None:
    try:
        rows = find_q2_documents(
            "local_lm_hard_case",
            {"project": os.environ.get("Q2_PROJECT", "q2"), "status": "resolved"},
            limit=100,
            sort=[("updated_at", 1)],
        )
    except Exception as exc:
        log_event("hard_case_variant_failed", "failed to load resolved hard cases", error=str(exc))
        return None
    if not rows:
        return None
    for offset in range(len(rows)):
        row = rows[(seed + offset) % len(rows)]
        question = challenge_variant(str(row.get("question") or ""), seed + offset)
        if not question or normalize_question(question) in seen:
            continue
        collection = str(row.get("collection") or "unknown")
        return Scenario(
            category="local-hard-case-variant",
            question=question,
            expected_collections=(collection,),
            source="local_lm:hard_case_variant",
        )
    return None


def continuous_challenge_scenario(seed: int) -> Scenario:
    base_questions = list(EVAL_QUESTIONS)
    try:
        rows = find_q2_documents(
            "local_lm_hard_case",
            {"project": os.environ.get("Q2_PROJECT", "q2")},
            limit=200,
            sort=[("updated_at", -1)],
        )
        for row in rows:
            question = str(row.get("question") or "").strip()
            if question and question not in base_questions:
                base_questions.append(question)
    except Exception as exc:
        log_event("continuous_seed_failed", "failed to load continuous challenge seed rows", error=str(exc))
    if not base_questions:
        base_questions = ["전체 장비의 윈도우 버전 분포를 보여줘"]
    base = base_questions[seed % len(base_questions)]
    question = challenge_variant(base, seed // max(1, len(base_questions)))
    return Scenario(
        category="local-continuous-challenge",
        question=question,
        expected_collections=("unknown",),
        source="local_lm:continuous",
    )


def record_evaluation_gaps(evaluation: dict[str, Any]) -> int:
    count = 0
    for row in evaluation.get("rows") or []:
        reference_plan = row.get("reference_plan")
        question = str(row.get("question") or "").strip()
        if row.get("semantic_same") or not question or not reference_plan:
            continue
        record_hard_case(
            question=question,
            reference_plan=reference_plan,
            local_plan=row.get("local_plan"),
            comparison=row.get("comparison") or {},
            local_errors=row.get("local_errors") or [],
        )
        count += 1
    if count:
        log_event(
            "eval_gaps_recorded",
            "promoted failed local LM evaluation rows to hard cases",
            evaluation_run_id=evaluation.get("run_id"),
            evaluation_passed=evaluation.get("passed"),
            evaluation_total=evaluation.get("total"),
            hard_case_count=count,
        )
    return count


def execute_question(base_url: str, question: str, reference_time: str, timeout: float) -> dict[str, Any]:
    payload: dict[str, Any] = {"question": question}
    if reference_time:
        payload["reference_time"] = reference_time
    started = time.monotonic()
    try:
        data = request_json("POST", f"{base_url}/api/mongo-query", payload, timeout=timeout)
        return {
            "ok": bool(data.get("ok")),
            "backend": data.get("backend"),
            "collection": (data.get("plan") or {}).get("collection"),
            "plan": data.get("plan"),
            "quality_status": data.get("quality_status"),
            "result_count": data.get("result_count"),
            "failure_reason": data.get("failure_reason"),
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


def generate_questions(
    base_url: str,
    timeout: float,
    seed: int,
    batch_size: int,
    seen: set[str] | None = None,
    seen_intents: set[str] | None = None,
) -> list[Scenario]:
    scenarios = profile_scenarios(base_url, min(timeout, 30.0), seed, max(batch_size * 30, 80))
    random.Random(seed).shuffle(scenarios)
    candidates = unique_scenarios(scenarios, max(batch_size * 6, 20), seen, seen_intents)
    if not candidates and seen_intents:
        log_event("dedupe_relaxed", "intent dedupe exhausted candidates; retrying exact-question dedupe only")
        candidates = unique_scenarios(scenarios, max(batch_size * 6, 20), seen, set())
    selected: list[Scenario] = []
    for scenario in candidates:
        if not claim_question(base_url, scenario, timeout):
            continue
        selected.append(scenario)
        if len(selected) >= batch_size:
            break
    return selected


def run_cycle(
    base_url: str,
    batch_size: int,
    timeout: float,
    reference_time: str,
    seed: int,
    seen: set[str] | None = None,
    seen_intents: set[str] | None = None,
) -> dict[str, Any]:
    started = time.monotonic()
    training_before = build_training_examples(limit=int(os.environ.get("Q2_LOCAL_TRAIN_LIMIT", "2000")))
    scenarios = generate_questions(base_url, timeout, seed, batch_size, seen, seen_intents)
    query_rows = []
    for scenario in scenarios:
        if STOP:
            break
        result = execute_question(base_url, scenario.question, reference_time, timeout)
        query_rows.append({
            "question": scenario.question,
            "category": scenario.category,
            "source": scenario.source,
            **result,
        })
        log_event(
            "query_ok" if result.get("ok") and result.get("quality_status") == "ok" else "query_needs_review",
            "generated q2 training query",
            question=scenario.question,
            category=scenario.category,
            source=scenario.source,
            **result,
        )
    training_after = build_training_examples(limit=int(os.environ.get("Q2_LOCAL_TRAIN_LIMIT", "2000")))
    evaluation = run_evaluation()
    cycle = {
        "seed": seed,
        "elapsed_ms": round((time.monotonic() - started) * 1000, 3),
        "question_count": len(query_rows),
        "ok_count": sum(1 for row in query_rows if row.get("ok") and row.get("quality_status") == "ok"),
        "training_before": training_before,
        "training_after": training_after,
        "evaluation_run_id": evaluation.get("run_id"),
        "evaluation_passed": evaluation.get("passed"),
        "evaluation_total": evaluation.get("total"),
        "evaluation_valid": evaluation.get("valid"),
        "evaluation_avg_elapsed_ms": evaluation.get("avg_elapsed_ms"),
    }
    log_event("cycle_done", "local LM training cycle completed", **cycle)
    return cycle


def run_step(base_url: str, timeout: float, reference_time: str, seed: int, seen: set[str], seen_intents: set[str]) -> dict[str, Any]:
    started = time.monotonic()
    scenario = open_hard_case_scenario()
    if scenario:
        log_event(
            "hard_case_selected",
            "retrying unresolved local LM hard case",
            question=scenario.question,
            category=scenario.category,
            source=scenario.source,
        )
    else:
        scenarios = generate_questions(base_url, timeout, seed, 1, seen, seen_intents)
        if scenarios:
            scenario = scenarios[0]
    if not scenario:
        scenario = resolved_hard_case_variant_scenario(seed, seen)
        if scenario:
            log_event(
                "hard_case_variant_selected",
                "challenging local LM with a resolved hard-case variant",
                question=scenario.question,
                category=scenario.category,
                source=scenario.source,
            )
    if not scenario:
        evaluation = run_evaluation()
        gap_count = record_evaluation_gaps(evaluation)
        if gap_count:
            scenario = open_hard_case_scenario()
    if not scenario:
        scenario = continuous_challenge_scenario(seed)
        log_event(
            "continuous_challenge_selected",
            "no idle; generated continuous local LM challenge",
            question=scenario.question,
            category=scenario.category,
            source=scenario.source,
        )
    seen.add(normalize_question(scenario.question))
    seen_intents.add(intent_fingerprint(scenario.question))
    result = execute_question(base_url, scenario.question, reference_time, timeout)
    query_status = "query_ok" if result.get("ok") and result.get("quality_status") == "ok" else "query_needs_review"
    log_event(
        query_status,
        "generated one q2 training query",
        question=scenario.question,
        category=scenario.category,
        source=scenario.source,
        **{key: value for key, value in result.items() if key != "plan"},
    )

    training = build_training_examples(limit=int(os.environ.get("Q2_LOCAL_TRAIN_LIMIT", "2000")))
    log_event(
        "training_updated",
        "local LM training examples refreshed",
        question=scenario.question,
        collection=result.get("collection"),
        total_examples=training.get("total_examples"),
        source_rows=training.get("source_rows"),
        upserted=training.get("upserted"),
    )

    verification = improve_until_pass(
        scenario.question,
        result.get("plan"),
        timeout,
        max_attempts=int(os.environ.get("Q2_LOCAL_TRAINER_RETRIES", "3")),
        category=scenario.category,
        source=scenario.source,
        reference_collection=result.get("collection"),
    )
    return {
        "ok": True,
        "question": scenario.question,
        "collection": result.get("collection"),
        "query_status": query_status,
        "training_examples": training.get("total_examples"),
        "verification": verification,
        "elapsed_ms": round((time.monotonic() - started) * 1000, 3),
    }


def verify_local_lm(question: str, reference_plan: dict[str, Any] | None, timeout: float) -> dict[str, Any]:
    if not reference_plan:
        return {"semantic_same": False, "local_ok": False, "reason": "missing_reference_plan"}
    catalog = load_catalog()
    planner = OllamaPlanner()
    planner.timeout_seconds = max(planner.timeout_seconds, timeout)
    local = planner.plan(question)
    local_errors = validate_plan(local.plan, catalog) if local.plan else []
    comparison = compare(reference_plan, local.plan)
    semantic_same = bool(local.plan and not local_errors and comparison.get("semantic_same"))
    return {
        "semantic_same": semantic_same,
        "local_ok": bool(local.plan and not local_errors),
        "local_collection": local.plan.collection if local.plan else None,
        "local_plan": local.plan.model_dump(mode="json") if local.plan else None,
        "local_elapsed_ms": local.elapsed_ms,
        "local_error": local.error,
        "local_errors": local_errors,
        "comparison": comparison,
    }


def improve_until_pass(
    question: str,
    reference_plan: dict[str, Any] | None,
    timeout: float,
    max_attempts: int,
    **event_fields: Any,
) -> dict[str, Any]:
    last: dict[str, Any] = {}
    for attempt in range(1, max(1, max_attempts) + 1):
        last = verify_local_lm(question, reference_plan, timeout)
        status = "local_verify_ok" if last.get("semantic_same") else "local_verify_gap"
        log_event(
            status,
            "local LM verified one question",
            question=question,
            attempt=attempt,
            **event_fields,
            **last,
        )
        if last.get("semantic_same"):
            mark_hard_case_resolved(question)
            if attempt > 1:
                log_event(
                    "hard_case_resolved",
                    "local LM improved after hard-case correction",
                    question=question,
                    attempt=attempt,
                    **event_fields,
                )
            return {**last, "attempts": attempt}
        if reference_plan:
            record_hard_case(
                question=question,
                reference_plan=reference_plan,
                local_plan=last.get("local_plan"),
                comparison=last.get("comparison") or {},
                local_errors=last.get("local_errors") or [],
            )
            log_event(
                "hard_case_recorded",
                "stored corrected q2 plan for local LM retry",
                question=question,
                attempt=attempt,
                **event_fields,
                comparison=last.get("comparison") or {},
            )
            build_training_examples(limit=int(os.environ.get("Q2_LOCAL_TRAIN_LIMIT", "2000")))
    return {**last, "attempts": max_attempts}


def main() -> None:
    parser = argparse.ArgumentParser(description="q2 local LM continuous trainer")
    parser.add_argument("--server", default=os.environ.get("Q2_SERVER", DEFAULT_SERVER))
    parser.add_argument("--interval", type=int, default=int(os.environ.get("Q2_LOCAL_TRAINER_INTERVAL", "1")))
    parser.add_argument("--batch-size", type=int, default=int(os.environ.get("Q2_LOCAL_TRAINER_BATCH", "12")))
    parser.add_argument("--timeout", type=float, default=float(os.environ.get("Q2_LOCAL_TRAINER_TIMEOUT", "120")))
    parser.add_argument("--mode", choices=["step", "cycle"], default=os.environ.get("Q2_LOCAL_TRAINER_MODE", "step"))
    parser.add_argument("--once", action="store_true")
    args = parser.parse_args()

    signal.signal(signal.SIGTERM, stop_signal)
    signal.signal(signal.SIGINT, stop_signal)

    log_event(
        "started",
        "local LM trainer started",
        base_url=args.server,
        interval=args.interval,
        batch_size=args.batch_size,
    )
    seed = int(time.time())
    seen_questions = load_seen_questions(args.server, min(args.timeout, 30.0))
    seen_questions.update(load_claimed_questions(args.server, min(args.timeout, 30.0)))
    seen_intents: set[str] = set()
    log_event("dedupe_ready", "loaded previous questions for dedupe", seen_count=len(seen_questions), intent_count=len(seen_intents))
    reference_time = ""
    while not STOP:
        if not wait_for_q2(args.server, min(args.timeout, 10.0)):
            break
        if not reference_time:
            reference_time = load_reference_time(args.server, args.timeout)
        try:
            if args.mode == "cycle":
                run_cycle(args.server, args.batch_size, args.timeout, reference_time, seed, seen_questions, seen_intents)
            else:
                run_step(args.server, args.timeout, reference_time, seed, seen_questions, seen_intents)
        except Exception as exc:
            log_event("failed", "local LM training cycle crashed", error=str(exc))
        if args.once or STOP:
            break
        seed += 1
        deadline = time.monotonic() + max(0, args.interval)
        while not STOP and time.monotonic() < deadline:
            time.sleep(1)
    log_event("stopped", "local LM trainer stopped")


if __name__ == "__main__":
    main()
