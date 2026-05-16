from __future__ import annotations

import json
import time
import os
import re
import hashlib
import asyncio
from concurrent.futures import ThreadPoolExecutor
from datetime import datetime, timedelta, timezone
from pathlib import Path
from typing import Any

from fastapi import FastAPI, Request, WebSocket, WebSocketDisconnect
from fastapi.encoders import jsonable_encoder
from fastapi.responses import FileResponse, Response
from fastapi.staticfiles import StaticFiles

from .planner.aggregation_builder import build_aggregation
from .planner.cache import PlanCache, cache_key
from .planner.catalog import load_catalog
from .planner.catalog import collection_time_field
from .planner.codex_pool import PLANNER_VERSION, CodexPool
from .planner.local_training import TRAINING_COLLECTION, build_training_examples
from .planner.models import CacheStats, PlanRequest, PlanResponse
from .planner.mongo_client import (
    find_q2_documents,
    insert_q2_document,
    q2_store_status,
    run_aggregation,
    source_mongo_uri_from_env,
    store_mongo_client_from_env,
    store_mongo_uri_from_env,
    upsert_q2_document,
)
from .planner.ollama_backend import OllamaPlanner
from .planner.quality import assess_response
from .planner.template_planner import plan_from_template
from .planner.validator import validate_plan


ROOT = Path(__file__).resolve().parent
WEB_DIR = ROOT / "web"
OUTPUT_DIR = ROOT / "output"
VERSION_PATH = ROOT / "VERSION"
LLM_QUERY_SPOOL = OUTPUT_DIR / "llm_query_spool.jsonl"
IMPROVEMENT_CANDIDATE_SPOOL = OUTPUT_DIR / "llm_improvement_candidate_spool.jsonl"
IMPROVEMENT_CANDIDATE_STATE = OUTPUT_DIR / "llm_improvement_candidate_state.json"
AUTOTEST_STATUS = OUTPUT_DIR / "q2_autotest_status.json"
AUTOTEST_LAST = OUTPUT_DIR / "q2_autotest_last.json"

def q2_version() -> str:
    try:
        value = VERSION_PATH.read_text(encoding="utf-8").strip()
    except OSError:
        value = "0.0.1.1"
    return value or "0.0.1.1"


def q2_runtime_info() -> dict[str, Any]:
    planner = pool.status()
    return {
        "backend": planner.get("backend") or "template",
        "library": planner.get("model_library") or os.environ.get("Q2_MODEL_LIBRARY", "Codex CLI"),
        "model": planner.get("model") or os.environ.get("Q2_CODEX_MODEL", ""),
        "codex_enabled": planner.get("codex_enabled"),
        "local": local_planner.status(),
    }


app = FastAPI(title="Orange q2 Natural Language Mongo Planner", version=q2_version())
app.mount("/web", StaticFiles(directory=str(WEB_DIR)), name="web")

catalog = load_catalog()
cache = PlanCache.from_env()
pool = CodexPool()
local_planner = OllamaPlanner()
shadow_executor = ThreadPoolExecutor(max_workers=max(1, int(os.environ.get("Q2_LOCAL_SHADOW_WORKERS", "1"))), thread_name_prefix="q2-local-shadow")
autotest_clients: set[WebSocket] = set()
llm_query_clients: set[WebSocket] = set()


async def broadcast_autotest_status(status: dict[str, Any]) -> None:
    if not autotest_clients:
        return
    payload = jsonable_encoder(status)
    disconnected: list[WebSocket] = []
    for websocket in list(autotest_clients):
        try:
            await websocket.send_json(payload)
        except Exception:
            disconnected.append(websocket)
    for websocket in disconnected:
        autotest_clients.discard(websocket)


async def broadcast_llm_query(document: dict[str, Any]) -> None:
    if not llm_query_clients:
        return
    clean_document = dict(document)
    clean_document.pop("_id", None)
    payload = jsonable_encoder(clean_document)
    disconnected: list[WebSocket] = []
    for websocket in list(llm_query_clients):
        try:
            await websocket.send_json(payload)
        except Exception:
            disconnected.append(websocket)
    for websocket in disconnected:
        llm_query_clients.discard(websocket)


def notify_llm_query(document: dict[str, Any]) -> None:
    loop = getattr(app.state, "loop", None)
    if not loop or not loop.is_running():
        print("q2 llm_query websocket skipped: event loop not ready", flush=True)
        return
    future = asyncio.run_coroutine_threadsafe(broadcast_llm_query(document), loop)

    def report_error(done: asyncio.Future) -> None:
        try:
            done.result()
        except Exception as exc:
            print(f"q2 llm_query websocket broadcast failed: {exc}", flush=True)

    future.add_done_callback(report_error)


@app.on_event("startup")
async def capture_event_loop() -> None:
    app.state.loop = asyncio.get_running_loop()


def utc_now() -> datetime:
    return datetime.now(timezone.utc)


def person_name_for_investigation(question: str) -> str | None:
    match = re.search(r"([가-힣]{2,4})(?:의|을|를|은|는)?\s*(?:모든\s*걸|전부|전체|종합)?\s*(?:조사|분석|털어)", question)
    if not match:
        return None
    name = re.sub(r"(?:의|을|를|은|는)$", "", match.group(1))
    return name or None


def recent_timestamp_ms(days: int) -> int:
    return int((datetime.now(timezone.utc) - timedelta(days=days)).timestamp() * 1000)


def value_by_path(row: dict[str, Any], path: str) -> Any:
    current: Any = row
    for part in path.split("."):
        if not isinstance(current, dict):
            return None
        current = current.get(part)
    return current


def row_node_id(row: dict[str, Any]) -> str | None:
    for key in ["id", "node_id", "nodeId", "NodeId"]:
        value = row.get(key)
        if value:
            return str(value)
    nested_candidates = [
        "_id.id",
        "_id.node_id",
        "_id.nodeId",
        "node.id",
        "target_node.id",
    ]
    for path in nested_candidates:
        value = value_by_path(row, path)
        if value:
            return str(value)
    return None


def build_node_label(node: dict[str, Any]) -> str:
    user = node.get("user") or value_by_path(node, "User.user.name")
    computer = node.get("computer") or value_by_path(node, "System.ComputerName") or value_by_path(node, "System.name")
    ip = node.get("ip") or value_by_path(node, "System.ip")
    node_id = node.get("id")
    parts = [str(part) for part in [user, computer, ip] if part]
    if parts:
        return " / ".join(parts)
    return str(node_id or "")


def enrich_result_rows(rows: list[dict[str, Any]]) -> list[dict[str, Any]]:
    node_ids = sorted({node_id for row in rows if (node_id := row_node_id(row))})
    if not node_ids:
        return rows
    try:
        nodes = run_aggregation(
            "node",
            [
                {"$match": {"id": {"$in": node_ids}}},
                {
                    "$project": {
                        "_id": 0,
                        "id": 1,
                        "user": "$User.user.name",
                        "role": "$User.user.role",
                        "computer": "$System.ComputerName",
                        "ip": "$System.ip",
                        "os": "$System.name",
                        "manufacturer": "$System.Manufacturer",
                        "model": "$System.Model",
                    }
                },
            ],
            limit=max(100, len(node_ids)),
        )
        node_map = {str(node.get("id")): node for node in nodes if node.get("id")}
    except Exception as exc:
        print(f"q2 node enrich failed: {exc}", flush=True)
        return rows

    enriched: list[dict[str, Any]] = []
    for row in rows:
        node_id = row_node_id(row)
        node = node_map.get(str(node_id)) if node_id else None
        if not node:
            if node_id:
                next_row = dict(row)
                next_row.setdefault("node_label", str(node_id))
                enriched.append(next_row)
            else:
                enriched.append(row)
            continue
        next_row = dict(row)
        label = build_node_label(node)
        next_row.setdefault("node_label", label)
        next_row.setdefault("node_user", node.get("user"))
        next_row.setdefault("node_computer", node.get("computer"))
        next_row.setdefault("node_ip", node.get("ip"))
        next_row.setdefault("node_os", node.get("os"))
        next_row.setdefault("node_manufacturer", node.get("manufacturer"))
        next_row.setdefault("node_model", node.get("model"))
        enriched.append(next_row)
    return enriched


def investigate_person(question: str, days: int = 30) -> PlanResponse | None:
    name = person_name_for_investigation(question)
    if not name:
        return None

    rows: list[dict] = []
    devices = run_aggregation(
        "node",
        [
            {"$match": {"User.user.name": {"$regex": name, "$options": "i"}}},
            {
                "$project": {
                    "_id": 0,
                    "section": {"$literal": "장비"},
                    "name": "$User.user.name",
                    "role": "$User.user.role",
                    "id": "$id",
                    "status": "$status",
                    "computer": "$System.ComputerName",
                    "ip": "$System.ip",
                    "os": "$System.name",
                    "manufacturer": "$System.Manufacturer",
                    "model": "$System.Model",
                    "memory_total_mb": "$System.Memory.total",
                    "memory_rate": "$System.Memory.rate",
                    "cpu_rate": "$System.CPU.rate",
                }
            },
            {"$sort": {"status": 1, "computer": 1}},
            {"$limit": 100},
        ],
        limit=100,
    )
    rows.extend(devices)
    node_ids = [row["id"] for row in devices if row.get("id")]
    if not node_ids:
        return PlanResponse(
            ok=True,
            backend="investigation",
            errors=[],
            result_count=0,
            result_preview=[],
        )

    rows.append({"section": "요약", "name": name, "node_count": len(set(node_ids))})

    rows.extend(run_aggregation(
        "sprocess",
        [
            {"$match": {"id": {"$in": node_ids}, "timestamp": {"$gte": recent_timestamp_ms(days)}}},
            {
                "$group": {
                    "_id": {"ProcName": "$ProcName"},
                    "ProcPath": {"$first": "$ProcPath"},
                    "Command": {"$first": "$Command"},
                    "CompanyName": {"$first": "$CompanyName"},
                    "ProductName": {"$first": "$ProductName"},
                    "ProductVersion": {"$first": "$ProductVersion"},
                    "FileDescription": {"$first": "$FileDescription"},
                    "FileVersion": {"$first": "$FileVersion"},
                    "Signer": {"$first": "$Signer"},
                    "IsSystem": {"$first": "$IsSystem"},
                    "sample_count": {"$sum": "$CounterCount"},
                    "pscore_sum": {"$sum": "$pscore"},
                    "counter_sum": {"$sum": "$CounterCount"},
                    "cpu_sum": {"$sum": "$CPU"},
                    "memory_sum": {"$sum": "$Memory"},
                    "io_sum": {"$sum": "$IO"},
                    "handle_sum": {"$sum": "$Handle"},
                    "detect_sum": {"$sum": "$Detect"},
                    "crash_sum": {"$sum": "$Crash"},
                    "start_sum": {"$sum": "$Start"},
                    "stop_sum": {"$sum": "$Stop"},
                    "lasttime": {"$max": "$lasttime"},
                }
            },
            {
                "$project": {
                    "_id": 0,
                    "section": {"$literal": "최근 프로세스"},
                    "ProcName": "$_id.ProcName",
                    "ProcPath": 1,
                    "Command": 1,
                    "CompanyName": 1,
                    "ProductName": 1,
                    "ProductVersion": 1,
                    "FileDescription": 1,
                    "FileVersion": 1,
                    "Signer": 1,
                    "IsSystem": 1,
                    "sample_count": 1,
                    "pscore_avg": {"$cond": [{"$gt": ["$counter_sum", 0]}, {"$divide": ["$pscore_sum", "$counter_sum"]}, None]},
                    "cpu_avg": {"$cond": [{"$gt": ["$counter_sum", 0]}, {"$divide": ["$cpu_sum", "$counter_sum"]}, None]},
                    "memory_avg_mb": {"$cond": [{"$gt": ["$counter_sum", 0]}, {"$divide": ["$memory_sum", "$counter_sum"]}, None]},
                    "io_avg_mbps": {"$cond": [{"$gt": ["$counter_sum", 0]}, {"$divide": ["$io_sum", "$counter_sum"]}, None]},
                    "handle_avg": {"$cond": [{"$gt": ["$counter_sum", 0]}, {"$divide": ["$handle_sum", "$counter_sum"]}, None]},
                    "detect_sum": 1,
                    "crash_sum": 1,
                    "start_sum": 1,
                    "stop_sum": 1,
                    "lasttime": 1,
                }
            },
            {"$sort": {"pscore_avg": -1}},
            {"$limit": 10},
        ],
        limit=10,
    ))

    rows.extend(run_aggregation(
        "filelist",
        [
            {"$match": {"id": {"$in": node_ids}, "timestamp": {"$gte": recent_timestamp_ms(days)}}},
            {
                "$group": {
                    "_id": {"FileName": "$FileName", "FilePath": "$FilePath"},
                    "CompanyName": {"$first": "$CompanyName"},
                    "ProductName": {"$first": "$ProductName"},
                    "ProductVersion": {"$first": "$ProductVersion"},
                    "FileDescription": {"$first": "$FileDescription"},
                    "FileVersion": {"$first": "$FileVersion"},
                    "Signer": {"$first": "$Signer"},
                    "Codesign": {"$first": "$Codesign"},
                    "IsSystem": {"$first": "$IsSystem"},
                    "MD5": {"$first": "$MD5"},
                    "HostUrl": {"$first": "$HostUrl"},
                    "ZoneId": {"$first": "$ZoneId"},
                    "CreateTime": {"$max": "$CreateTime"},
                    "EventTime": {"$max": "$EventTime"},
                    "InstallTime": {"$max": "$InstallTime"},
                    "lasttime": {"$max": "$lasttime"},
                    "file_count": {"$sum": 1},
                    "install_count": {"$sum": "$InstallCount"},
                    "file_size_max": {"$max": "$FileSize"},
                }
            },
            {
                "$project": {
                    "_id": 0,
                    "section": {"$literal": "최근 파일"},
                    "FileName": "$_id.FileName",
                    "FilePath": "$_id.FilePath",
                    "CompanyName": 1,
                    "ProductName": 1,
                    "ProductVersion": 1,
                    "FileDescription": 1,
                    "FileVersion": 1,
                    "Signer": 1,
                    "Codesign": 1,
                    "IsSystem": 1,
                    "MD5": 1,
                    "HostUrl": 1,
                    "ZoneId": 1,
                    "CreateTime": 1,
                    "EventTime": 1,
                    "InstallTime": 1,
                    "lasttime": 1,
                    "file_count": 1,
                    "install_count": 1,
                    "file_size_max": 1,
                }
            },
            {"$sort": {"file_count": -1}},
            {"$limit": 10},
        ],
        limit=10,
    ))

    rows.extend(run_aggregation(
        "detect",
        [
            {"$match": {"id": {"$in": node_ids}, "timestamp": {"$gte": recent_timestamp_ms(days)}}},
            {
                "$group": {
                    "_id": {"RuleId": "$RuleId", "Desc": "$Desc", "FileName": "$FileName"},
                    "detect_count": {"$sum": "$count"},
                    "lasttime": {"$max": "$lasttime"},
                }
            },
            {
                "$project": {
                    "_id": 0,
                    "section": {"$literal": "최근 탐지"},
                    "RuleId": "$_id.RuleId",
                    "Desc": "$_id.Desc",
                    "FileName": "$_id.FileName",
                    "detect_count": 1,
                    "lasttime": 1,
                }
            },
            {"$sort": {"detect_count": -1}},
            {"$limit": 10},
        ],
        limit=10,
    ))

    rows.extend(run_aggregation(
        "report",
        [
            {"$match": {"id": {"$in": node_ids}, "timestamp": {"$gte": recent_timestamp_ms(days)}}},
            {
                "$group": {
                    "_id": {"RuleId": "$RuleId", "Desc": "$Desc", "Name": "$Name", "Value": "$Value"},
                    "report_count": {"$sum": "$count"},
                    "lasttime": {"$max": "$lasttime"},
                }
            },
            {
                "$project": {
                    "_id": 0,
                    "section": {"$literal": "최근 리포트"},
                    "RuleId": "$_id.RuleId",
                    "Desc": "$_id.Desc",
                    "Name": "$_id.Name",
                    "Value": "$_id.Value",
                    "report_count": 1,
                    "lasttime": 1,
                }
            },
            {"$sort": {"report_count": -1}},
            {"$limit": 10},
        ],
        limit=10,
    ))

    return PlanResponse(
        ok=True,
        backend="investigation",
        errors=[],
        execution_ms=None,
        result_count=len(rows),
        result_preview=rows,
    )


def json_default(value):
    if isinstance(value, datetime):
        return value.isoformat()
    return str(value)


def append_llm_query_spool(document: dict) -> None:
    OUTPUT_DIR.mkdir(parents=True, exist_ok=True)
    with LLM_QUERY_SPOOL.open("a", encoding="utf-8") as handle:
        handle.write(json.dumps(document, ensure_ascii=False, default=json_default) + "\n")


def append_improvement_candidate_spool(document: dict) -> None:
    OUTPUT_DIR.mkdir(parents=True, exist_ok=True)
    with IMPROVEMENT_CANDIDATE_SPOOL.open("a", encoding="utf-8") as handle:
        handle.write(json.dumps(document, ensure_ascii=False, default=json_default) + "\n")


def spool_count() -> int:
    if not LLM_QUERY_SPOOL.exists():
        return 0
    return sum(1 for line in LLM_QUERY_SPOOL.read_text(encoding="utf-8").splitlines() if line.strip())


def improvement_candidate_spool_count() -> int:
    if not IMPROVEMENT_CANDIDATE_SPOOL.exists():
        return 0
    return sum(1 for line in IMPROVEMENT_CANDIDATE_SPOOL.read_text(encoding="utf-8").splitlines() if line.strip())


def read_jsonl_tail(path: Path, limit: int = 100) -> list[dict[str, Any]]:
    if not path.exists():
        return []
    rows: list[dict[str, Any]] = []
    lines = [line for line in path.read_text(encoding="utf-8").splitlines() if line.strip()]
    for line in lines[-max(1, min(limit, 1000)):]:
        try:
            rows.append(json.loads(line))
        except json.JSONDecodeError:
            continue
    return rows


def candidate_id(document: dict[str, Any]) -> str:
    key = {
        "question": document.get("question"),
        "planner_version": document.get("planner_version"),
        "collection": document.get("collection"),
        "failure_reason": document.get("failure_reason"),
        "candidate_fix": document.get("candidate_fix"),
    }
    payload = json.dumps(key, ensure_ascii=False, sort_keys=True, default=json_default)
    return hashlib.sha1(payload.encode("utf-8")).hexdigest()[:16]


def load_candidate_state() -> dict[str, Any]:
    if not IMPROVEMENT_CANDIDATE_STATE.exists():
        return {}
    try:
        return json.loads(IMPROVEMENT_CANDIDATE_STATE.read_text(encoding="utf-8"))
    except json.JSONDecodeError:
        return {}


def read_json_file(path: Path, default: Any) -> Any:
    if not path.exists():
        return default
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except json.JSONDecodeError:
        return default


def save_candidate_state(state: dict[str, Any]) -> None:
    OUTPUT_DIR.mkdir(parents=True, exist_ok=True)
    IMPROVEMENT_CANDIDATE_STATE.write_text(
        json.dumps(state, ensure_ascii=False, indent=2, default=json_default),
        encoding="utf-8",
    )


def summarize_candidate(document: dict[str, Any], state: dict[str, Any]) -> dict[str, Any]:
    cid = candidate_id(document)
    candidate_state = state.get(cid, {})
    probes = document.get("probes") or []
    positive_probes = [probe for probe in probes if probe.get("ok") and probe.get("result_count", 0) > 0]
    eval_case = {
        "id": f"candidate_{cid}",
        "request": document.get("question", ""),
        "expected_collection": document.get("candidate_fix", {}).get("patch", {}).get("collection") or document.get("collection"),
        "expected_group_by": (document.get("plan") or {}).get("group_by", []),
        "expected_metrics": [metric.get("field") for metric in (document.get("plan") or {}).get("metrics", [])],
        "expected_filters": [item.get("field") for item in (document.get("plan") or {}).get("filters", [])],
    }
    return {
        "id": cid,
        "created_at": document.get("created_at"),
        "status": candidate_state.get("status", document.get("status", "proposed")),
        "note": candidate_state.get("note", ""),
        "question": document.get("question"),
        "collection": document.get("collection"),
        "failure_reason": document.get("failure_reason"),
        "candidate_fix": document.get("candidate_fix"),
        "positive_probe_count": len(positive_probes),
        "best_probe": positive_probes[0] if positive_probes else None,
        "probe_count": len(probes),
        "eval_case": eval_case,
        "plan": document.get("plan"),
        "probes": probes,
    }


def normalize_question_key(question: str) -> str:
    normalized = re.sub(r"\s+", " ", (question or "").strip()).lower()
    return hashlib.sha256(normalized.encode("utf-8")).hexdigest()


def write_improvement_candidate(document: dict) -> None:
    cid = candidate_id(document)
    document["candidate_id"] = cid
    try:
        upsert_q2_document(
            "llm_improvement_candidate",
            {"candidate_id": cid, "project": document.get("project", "q2")},
            document,
        )
    except Exception as exc:
        print(f"q2 improvement candidate db log failed: {exc}", flush=True)


def log_llm_query(endpoint: str, request: PlanRequest, response: PlanResponse, started: float, client_host: str = "") -> None:
    document = {
        "created_at": utc_now(),
        "project": os.environ.get("Q2_PROJECT", "q2"),
        "endpoint": endpoint,
        "question": request.question,
        "timezone": request.timezone,
        "client_host": client_host,
        "ok": response.ok,
        "backend": response.backend,
        "cache_hit": response.cache_hit,
        "planner_version": PLANNER_VERSION,
        "schema_version": catalog["schema_version"],
        "collection": response.plan.collection if response.plan else None,
        "plan": response.plan.model_dump(mode="json") if response.plan else None,
        "aggregation": response.aggregation,
        "errors": response.errors,
        "execution_ms": response.execution_ms,
        "result_count": response.result_count,
        "quality_status": response.quality_status,
        "failure_reason": response.failure_reason,
        "probes": response.probes,
        "candidate_fix": response.candidate_fix,
        "total_ms": round((time.monotonic() - started) * 1000, 3),
    }
    if response.candidate_fix:
        write_improvement_candidate({
            "created_at": document["created_at"],
            "project": document["project"],
            "status": "proposed",
            "source_endpoint": endpoint,
            "question": request.question,
            "planner_version": PLANNER_VERSION,
            "schema_version": catalog["schema_version"],
            "collection": document["collection"],
            "failure_reason": response.failure_reason,
            "candidate_fix": response.candidate_fix,
            "probes": response.probes,
            "plan": document["plan"],
        })
    try:
        insert_q2_document("llm_query", document)
        notify_llm_query(document)
    except Exception as exc:
        print(f"q2 llm_query db log failed: {exc}", flush=True)


def plan_signature(plan: Any) -> dict[str, Any] | None:
    if plan is None:
        return None
    data = plan.model_dump(mode="json") if hasattr(plan, "model_dump") else dict(plan)
    return {
        "collection": data.get("collection"),
        "group_by": data.get("group_by") or [],
        "metrics": [
            {"name": item.get("name"), "op": item.get("op"), "field": item.get("field")}
            for item in data.get("metrics", [])
        ],
        "filters": [
            {"field": item.get("field"), "op": item.get("op"), "value": item.get("value")}
            for item in data.get("filters", [])
        ],
        "sort": data.get("sort") or [],
    }


def compare_plan_signatures(reference: dict[str, Any] | None, local: dict[str, Any] | None) -> dict[str, Any]:
    if not reference or not local:
        return {"same": False, "reason": "missing_plan"}
    reference_metrics = [
        {"op": item.get("op"), "field": item.get("field")}
        for item in reference.get("metrics", [])
    ]
    local_metrics = [
        {"op": item.get("op"), "field": item.get("field")}
        for item in local.get("metrics", [])
    ]
    collection_match = reference.get("collection") == local.get("collection")
    group_by_match = reference.get("group_by") == local.get("group_by")
    metric_match = reference.get("metrics") == local.get("metrics")
    metric_semantic_match = reference_metrics == local_metrics
    filter_match = reference.get("filters") == local.get("filters")
    sort_match = reference.get("sort") == local.get("sort")
    semantic_same = collection_match and group_by_match and metric_semantic_match and filter_match
    return {
        "same": reference == local,
        "semantic_same": semantic_same,
        "collection_match": collection_match,
        "group_by_match": group_by_match,
        "metric_match": metric_match,
        "metric_semantic_match": metric_semantic_match,
        "filter_match": filter_match,
        "sort_match": sort_match,
    }


def run_local_shadow_compare(request: PlanRequest, reference: PlanResponse, endpoint: str) -> None:
    if os.environ.get("Q2_LOCAL_SHADOW_ENABLED", "1") in {"0", "false", "False", "no"}:
        return
    started = utc_now()
    local = local_planner.plan(request.question, timezone=request.timezone)
    local_errors: list[str] = []
    local_aggregation = None
    if local.plan is not None:
        local_errors = validate_plan(local.plan, catalog)
        if not local_errors:
            try:
                local_aggregation = build_aggregation(local.plan, catalog)
            except Exception as exc:
                local_errors.append(str(exc))
    reference_sig = plan_signature(reference.plan)
    local_sig = plan_signature(local.plan)
    document = {
        "created_at": started,
        "project": os.environ.get("Q2_PROJECT", "q2"),
        "endpoint": endpoint,
        "question": request.question,
        "timezone": request.timezone,
        "reference_backend": reference.backend,
        "reference_ok": reference.ok,
        "reference_quality_status": reference.quality_status,
        "reference_collection": reference.plan.collection if reference.plan else None,
        "reference_plan": reference.plan.model_dump(mode="json") if reference.plan else None,
        "reference_signature": reference_sig,
        "local_backend": local.backend,
        "local_library": "Ollama",
        "local_model": local.model,
        "local_ok": local.plan is not None and not local_errors,
        "local_error": local.error,
        "local_errors": local_errors,
        "local_elapsed_ms": local.elapsed_ms,
        "local_collection": local.plan.collection if local.plan else None,
        "local_plan": local.plan.model_dump(mode="json") if local.plan else None,
        "local_signature": local_sig,
        "local_aggregation": local_aggregation,
        "comparison": compare_plan_signatures(reference_sig, local_sig),
    }
    try:
        insert_q2_document("llm_backend_compare", document)
    except Exception as exc:
        print(f"q2 local shadow compare db log failed: {exc}", flush=True)


def schedule_local_shadow_compare(request: PlanRequest, response: PlanResponse, endpoint: str) -> None:
    if response.plan is None:
        return
    shadow_executor.submit(run_local_shadow_compare, request, response, endpoint)


@app.get("/")
def index():
    return FileResponse(str(WEB_DIR / "index.html"))


@app.head("/")
def index_head():
    return FileResponse(str(WEB_DIR / "index.html"))


@app.get("/manifest.json")
def manifest():
    return FileResponse(str(WEB_DIR / "manifest.json"), media_type="application/manifest+json")


@app.get("/sw.js")
def service_worker():
    headers = {
        "Service-Worker-Allowed": "/",
        "Cache-Control": "no-cache",
    }
    return FileResponse(str(WEB_DIR / "sw.js"), media_type="application/javascript", headers=headers)


@app.get("/health")
def health():
    _, source_db = source_mongo_uri_from_env()
    _, store_db = store_mongo_uri_from_env()
    return {
        "ok": True,
        "service": "q2",
        "version": q2_version(),
        "host": "0.0.0.0",
        "port": 3184,
        "planner_version": PLANNER_VERSION,
        "schema_version": catalog["schema_version"],
        "planner": pool.status(),
        "model": q2_runtime_info(),
        "runtime": q2_runtime_info(),
        "source_db": source_db,
        "store_db": store_db,
        "websocket_clients": {
            "autotest": len(autotest_clients),
            "llm_query": len(llm_query_clients),
        },
    }


@app.get("/api/schema")
def schema():
    return catalog


@app.get("/api/source-time-range")
def source_time_range():
    ranges: dict[str, Any] = {}
    max_ms: int | None = None
    max_collection = ""
    for collection in catalog.get("collections", {}):
        time_field = collection_time_field(catalog, collection)
        if not time_field:
            continue
        try:
            rows = run_aggregation(
                collection,
                [
                    {
                        "$group": {
                            "_id": None,
                            "min": {"$min": f"${time_field}"},
                            "max": {"$max": f"${time_field}"},
                            "count": {"$sum": 1},
                        }
                    }
                ],
                limit=1,
            )
            if not rows:
                continue
            row = rows[0]
            min_value = row.get("min")
            max_value = row.get("max")
            if max_value is None:
                continue
            current_max_ms = int(max_value if time_field == "timestamp" else max_value * 1000)
            ranges[collection] = {
                "time_field": time_field,
                "count": row.get("count", 0),
                "min": min_value,
                "max": max_value,
                "max_iso": datetime.fromtimestamp(current_max_ms / 1000, tz=timezone.utc).isoformat(),
            }
            if max_ms is None or current_max_ms > max_ms:
                max_ms = current_max_ms
                max_collection = collection
        except Exception as exc:
            ranges[collection] = {"time_field": time_field, "error": str(exc)}
    reference_time = datetime.fromtimestamp(max_ms / 1000, tz=timezone.utc).isoformat() if max_ms else None
    return {
        "ok": max_ms is not None,
        "reference_time": reference_time,
        "max_collection": max_collection,
        "collections": ranges,
    }


@app.get("/api/data-profile")
def data_profile(limit: int = 20):
    try:
        rows = find_q2_documents(
            "data_profile",
            {"project": os.environ.get("Q2_PROJECT", "q2")},
            limit=max(1, min(limit, 100)),
            sort=[("created_at", -1)],
        )
    except Exception as exc:
        return {"ok": False, "count": 0, "profiles": [], "error": str(exc)}
    return {"ok": True, "count": len(rows), "profiles": rows}


@app.get("/api/llm-query/status")
def llm_query_status():
    stores = {}
    for logical_name in ["llm_query", "llm_improvement_candidate", "autotest_status"]:
        try:
            stores[logical_name] = q2_store_status(logical_name)
        except Exception as exc:
            stores[logical_name] = {"error": str(exc)}
    status = {
        "project": os.environ.get("Q2_PROJECT", "q2"),
        "preferred_db": store_mongo_uri_from_env()[1],
        "stores": stores,
    }
    return status


@app.post("/api/llm-query/flush")
def flush_llm_query_spool():
    if not LLM_QUERY_SPOOL.exists():
        return {"ok": True, "flushed": 0, "remaining": 0}

    lines = [line for line in LLM_QUERY_SPOOL.read_text(encoding="utf-8").splitlines() if line.strip()]
    flushed = 0
    remaining = []
    for line in lines:
        try:
            insert_q2_document("llm_query", json.loads(line))
            flushed += 1
        except Exception:
            remaining.append(line)
    if remaining:
        LLM_QUERY_SPOOL.write_text("\n".join(remaining) + "\n", encoding="utf-8")
    else:
        LLM_QUERY_SPOOL.unlink(missing_ok=True)
    return {"ok": not remaining, "flushed": flushed, "remaining": len(remaining)}


@app.get("/api/improvement-candidates")
def improvement_candidates(limit: int = 100):
    state = {}
    try:
        raw_rows = find_q2_documents(
            "llm_improvement_candidate",
            {"project": os.environ.get("Q2_PROJECT", "q2")},
            limit=max(1, min(limit, 1000)),
            sort=[("created_at", -1)],
        )
    except Exception as exc:
        return {"ok": False, "source": "db", "count": 0, "candidates": [], "error": str(exc)}
    seen: dict[str, dict[str, Any]] = {}
    duplicate_count: dict[str, int] = {}
    for row in raw_rows:
        cid = candidate_id(row)
        duplicate_count[cid] = duplicate_count.get(cid, 0) + 1
        seen[cid] = row
    candidates = [summarize_candidate(row, state) for row in seen.values()]
    for item in candidates:
        item["occurrences"] = duplicate_count.get(item["id"], 1)
    candidates.sort(key=lambda item: item.get("created_at") or "", reverse=True)
    return {
        "ok": True,
        "source": "db",
        "count": len(candidates),
        "candidates": candidates,
    }


@app.post("/api/improvement-candidates/{candidate_id_value}/status")
async def update_improvement_candidate_status(candidate_id_value: str, request: Request):
    body = await request.json()
    status = body.get("status")
    if status not in {"proposed", "accepted", "rejected", "implemented"}:
        return {"ok": False, "error": "status must be proposed, accepted, rejected, or implemented"}
    update = {
        "candidate_id": candidate_id_value,
        "project": os.environ.get("Q2_PROJECT", "q2"),
        "status": status,
        "note": body.get("note", ""),
        "updated_at": utc_now(),
    }
    upsert_q2_document(
        "llm_improvement_candidate",
        {"candidate_id": candidate_id_value, "project": update["project"]},
        update,
    )
    return {"ok": True, "id": candidate_id_value, "status": status}


@app.get("/api/improvement-requests")
def improvement_requests(limit: int = 100, status: str = ""):
    query: dict[str, Any] = {"project": os.environ.get("Q2_PROJECT", "q2")}
    if status:
        query["status"] = status
    try:
        rows = find_q2_documents(
            "improvement_request",
            query,
            limit=max(1, min(limit, 1000)),
            sort=[("created_at", -1)],
        )
    except Exception as exc:
        return {"ok": False, "count": 0, "requests": [], "error": str(exc)}
    return {"ok": True, "count": len(rows), "requests": rows}


@app.post("/api/improvement-requests")
async def create_improvement_request(request: Request):
    body = await request.json()
    instruction = str(body.get("instruction") or "").strip()
    if not instruction:
        return {"ok": False, "error": "instruction is required"}
    request_id = hashlib.sha256(f"{utc_now().isoformat()}:{instruction}".encode("utf-8")).hexdigest()[:16]
    document = {
        "request_id": request_id,
        "project": os.environ.get("Q2_PROJECT", "q2"),
        "created_at": utc_now(),
        "status": "pending",
        "instruction": instruction,
        "candidate_id": body.get("candidate_id") or "",
        "question": body.get("question") or "",
        "source": "web",
    }
    insert_q2_document("improvement_request", document)
    document.pop("_id", None)
    return {"ok": True, "request": document}


@app.post("/api/improvement-requests/{request_id}/status")
async def update_improvement_request_status(request_id: str, request: Request):
    body = await request.json()
    status = body.get("status")
    if status not in {"pending", "processed", "attempted", "rejected"}:
        return {"ok": False, "error": "status must be pending, processed, attempted, or rejected"}
    rows = find_q2_documents(
        "improvement_request",
        {"project": os.environ.get("Q2_PROJECT", "q2"), "request_id": request_id},
        limit=1,
    )
    if not rows:
        return {"ok": False, "error": "request not found"}
    document = rows[0]
    document.update(
        {
            "status": status,
            "updated_at": utc_now(),
            "response": body.get("response", document.get("response", "")),
        }
    )
    upsert_q2_document("improvement_request", {"request_id": request_id}, document)
    return {"ok": True, "request_id": request_id, "status": status}


@app.get("/api/llm-query/recent")
def recent_llm_queries(limit: int = 100):
    try:
        rows = find_q2_documents(
            "llm_query",
            {"project": os.environ.get("Q2_PROJECT", "q2")},
            limit=max(1, min(limit, 50000)),
            sort=[("created_at", -1)],
        )
    except Exception as exc:
        return {"ok": False, "source": "db", "count": 0, "rows": [], "error": str(exc)}
    return {
        "ok": True,
        "source": "db",
        "count": len(rows),
        "rows": rows,
    }


@app.websocket("/ws/llm-query")
async def llm_query_websocket(websocket: WebSocket):
    await websocket.accept()
    llm_query_clients.add(websocket)
    try:
        rows = find_q2_documents(
            "llm_query",
            {"project": os.environ.get("Q2_PROJECT", "q2")},
            limit=100,
            sort=[("created_at", -1)],
        )
        if rows:
            await websocket.send_json(jsonable_encoder({"type": "snapshot", "rows": rows}))
        while True:
            await websocket.receive_text()
    except WebSocketDisconnect:
        pass
    finally:
        llm_query_clients.discard(websocket)


@app.get("/api/autotest/status")
def autotest_status():
    try:
        rows = find_q2_documents("autotest_status", {}, limit=1, sort=[("updated_at", -1)])
    except Exception as exc:
        return {"ok": False, "state": "db_error", "recent": [], "error": str(exc)}
    if not rows:
        return {"ok": True, "state": "idle", "message": "no autotest status yet"}
    status = rows[0]
    status["ok"] = True
    return status


@app.get("/api/autotest/last")
def autotest_last():
    try:
        rows = find_q2_documents("autotest_status", {"state": "completed"}, limit=1, sort=[("updated_at", -1)])
    except Exception as exc:
        return {"ok": False, "state": "db_error", "recent": [], "error": str(exc)}
    if not rows:
        return {"ok": True, "state": "empty", "message": "no autotest result yet"}
    rows[0]["ok"] = True
    return rows[0]


@app.post("/api/autotest/status")
async def update_autotest_status(request: Request):
    body = await request.json()
    body["project"] = os.environ.get("Q2_PROJECT", "q2")
    body["updated_at"] = utc_now()
    run_id = body.get("run_id") or "default"
    try:
        upsert_q2_document("autotest_status", {"run_id": run_id}, body)
    except Exception as exc:
        body["state"] = "db_error"
        body["db_error"] = str(exc)
        await broadcast_autotest_status(body)
        return {"ok": False, "run_id": run_id, "error": str(exc)}
    await broadcast_autotest_status(body)
    return {"ok": True, "run_id": run_id}


@app.post("/api/autoresearch/results")
async def write_autoresearch_result(request: Request):
    body = await request.json()
    project = os.environ.get("Q2_PROJECT", "q2")
    run_id = str(body.get("run_id") or "default")
    row = body.get("row") or {}
    if not isinstance(row, dict) or not row.get("question"):
        return {"ok": False, "error": "row.question is required"}
    status = "mismatch" if row.get("expected_match") is False else row.get("quality_status") or ("ok" if row.get("ok") else "failed")
    document = {
        "project": project,
        "run_id": run_id,
        "stream_index": row.get("stream_index"),
        "created_at": utc_now(),
        "reference_time": body.get("reference_time") or "",
        "status": status,
        "question": row.get("question"),
        "category": row.get("category") or "",
        "source": row.get("source") or "",
        "ok": bool(row.get("ok")),
        "collection": row.get("collection"),
        "expected_collections": row.get("expected_collections") or [],
        "expected_match": row.get("expected_match"),
        "quality_status": row.get("quality_status"),
        "failure_reason": row.get("failure_reason"),
        "candidate_fix": row.get("candidate_fix"),
        "result_count": row.get("result_count"),
        "elapsed_ms": row.get("elapsed_ms"),
        "errors": row.get("errors") or [],
    }
    query = {"project": project, "run_id": run_id, "stream_index": row.get("stream_index")}
    try:
        upsert_q2_document("autoresearch_result", query, document)
    except Exception as exc:
        return {"ok": False, "status": status, "error": str(exc)}

    if status in {"mismatch", "failed", "needs_review"} or row.get("candidate_fix"):
        request_key = hashlib.sha256(
            json.dumps(
                {
                    "kind": "autoresearch",
                    "question": row.get("question"),
                    "status": status,
                    "collection": row.get("collection"),
                    "expected": row.get("expected_collections") or [],
                    "failure": row.get("failure_reason"),
                },
                ensure_ascii=False,
                sort_keys=True,
                default=json_default,
            ).encode("utf-8")
        ).hexdigest()[:16]
        instruction = (
            "오토리서치 실패/불일치 케이스를 개선하라. "
            f"질문='{row.get('question')}', status={status}, "
            f"actual_collection={row.get('collection')}, expected={row.get('expected_collections')}, "
            f"failure={row.get('failure_reason')}, candidate_fix={row.get('candidate_fix')}"
        )
        try:
            upsert_q2_document(
                "improvement_request",
                {"project": project, "request_id": request_key},
                {
                    "request_id": request_key,
                    "project": project,
                    "created_at": utc_now(),
                    "status": "pending",
                    "instruction": instruction,
                    "candidate_id": "",
                    "question": row.get("question") or "",
                    "source": "autoresearch",
                    "autoresearch_status": status,
                },
            )
        except Exception as exc:
            return {"ok": False, "status": status, "error": str(exc)}
    return {"ok": True, "status": status}


@app.get("/api/autoresearch/results")
def autoresearch_results(limit: int = 100, status: str = ""):
    query: dict[str, Any] = {"project": os.environ.get("Q2_PROJECT", "q2")}
    if status:
        query["status"] = status
    try:
        rows = find_q2_documents(
            "autoresearch_result",
            query,
            limit=max(1, min(limit, 1000)),
            sort=[("created_at", -1)],
        )
    except Exception as exc:
        return {"ok": False, "count": 0, "rows": [], "error": str(exc)}
    return {"ok": True, "count": len(rows), "rows": rows}


@app.get("/api/macbook-worker/events")
def macbook_worker_events(limit: int = 100, worker: str = "", status: str = ""):
    query: dict[str, Any] = {"project": os.environ.get("Q2_PROJECT", "q2")}
    if worker:
        query["worker"] = worker
    if status:
        query["status"] = status
    try:
        rows = find_q2_documents(
            "macbook_worker_event",
            query,
            limit=max(1, min(limit, 1000)),
            sort=[("created_at", -1)],
        )
    except Exception as exc:
        return {"ok": False, "count": 0, "rows": [], "error": str(exc)}
    return {"ok": True, "count": len(rows), "rows": rows}


@app.post("/api/autotest/claim-question")
async def claim_autotest_question(request: Request):
    body = await request.json()
    question = str(body.get("question") or "").strip()
    if not question:
        return {"ok": False, "claimed": False, "error": "question is required"}
    project = os.environ.get("Q2_PROJECT", "q2")
    question_key = normalize_question_key(question)
    intent_key = str(body.get("intent_key") or "").strip()
    dedupe_mode = str(body.get("dedupe_mode") or "intent").strip()
    duplicate_query: dict[str, Any]
    if dedupe_mode == "question":
        duplicate_query = {"project": project, "question_key": question_key}
    else:
        duplicate_query = {
            "project": project,
            "$or": [
                {"question_key": question_key},
                {"intent_key": intent_key} if intent_key else {"question_key": question_key},
            ],
        }
    try:
        existing = find_q2_documents(
            "autotest_seen_question",
            duplicate_query,
            limit=1,
        )
        if existing:
            return {"ok": True, "claimed": False, "question_key": question_key, "intent_key": intent_key}
        insert_q2_document(
            "autotest_seen_question",
            {
                "project": project,
                "question_key": question_key,
                "intent_key": intent_key,
                "question": question,
                "source": body.get("source") or "",
                "category": body.get("category") or "",
                "dedupe_mode": dedupe_mode,
                "created_at": utc_now(),
            },
        )
        return {"ok": True, "claimed": True, "question_key": question_key, "intent_key": intent_key}
    except Exception as exc:
        return {"ok": False, "claimed": False, "error": str(exc)}


@app.get("/api/autotest/seen-questions")
def autotest_seen_questions(limit: int = 50000):
    project = os.environ.get("Q2_PROJECT", "q2")
    bounded_limit = max(1, min(int(limit), 100000))
    try:
        rows = find_q2_documents(
            "autotest_seen_question",
            {"project": project},
            limit=bounded_limit,
            sort=[("created_at", -1)],
        )
    except Exception as exc:
        return {"ok": False, "count": 0, "rows": [], "error": str(exc)}
    return {"ok": True, "count": len(rows), "rows": rows}


@app.websocket("/ws/autotest")
async def autotest_websocket(websocket: WebSocket):
    await websocket.accept()
    autotest_clients.add(websocket)
    try:
        rows = find_q2_documents("autotest_status", {}, limit=1, sort=[("updated_at", -1)])
        if rows:
            rows[0]["ok"] = True
            await websocket.send_json(jsonable_encoder(rows[0]))
        while True:
            await websocket.receive_text()
    except WebSocketDisconnect:
        pass
    finally:
        autotest_clients.discard(websocket)


@app.get("/api/cache/stats", response_model=CacheStats)
def cache_stats():
    size, hits, misses = cache.stats()
    path = str(cache.path) if cache.path else None
    return CacheStats(exact_size=size, hits=hits, misses=misses, path=path)


@app.get("/api/local-backend/status")
def local_backend_status():
    try:
        compare_store = q2_store_status("llm_backend_compare")
    except Exception as exc:
        compare_store = {"error": str(exc)}
    return {
        "ok": True,
        "runtime": local_planner.health(),
        "compare_store": compare_store,
    }


@app.get("/api/local-backend/compare")
def local_backend_compare(days: int = 7, limit: int = 100):
    since = utc_now() - timedelta(days=max(1, min(days, 90)))
    limit = max(1, min(limit, 500))
    client = store_mongo_client_from_env()
    _, db_name = store_mongo_uri_from_env()
    collection = client[db_name]["llm_backend_compare"]
    training_collection = client[db_name][TRAINING_COLLECTION]
    hard_case_collection = client[db_name]["local_lm_hard_case"]
    match = {
        "project": os.environ.get("Q2_PROJECT", "q2"),
        "created_at": {"$gte": since},
    }
    total = collection.count_documents(match)
    semantic_evaluated = collection.count_documents({**match, "comparison.semantic_same": {"$exists": True}})
    local_ok = collection.count_documents({**match, "local_ok": True})
    exact_same = collection.count_documents({**match, "comparison.same": True})
    semantic_same = collection.count_documents({**match, "comparison.semantic_same": True})
    avg_rows = list(collection.aggregate([
        {"$match": {**match, "local_elapsed_ms": {"$type": "number"}}},
        {"$group": {"_id": None, "avg_ms": {"$avg": "$local_elapsed_ms"}, "max_ms": {"$max": "$local_elapsed_ms"}}},
    ]))
    by_collection = list(collection.aggregate([
        {"$match": match},
        {"$group": {
            "_id": "$reference_collection",
            "total": {"$sum": 1},
            "local_ok": {"$sum": {"$cond": ["$local_ok", 1, 0]}},
            "semantic_same": {"$sum": {"$cond": ["$comparison.semantic_same", 1, 0]}},
            "exact_same": {"$sum": {"$cond": ["$comparison.same", 1, 0]}},
        }},
        {"$sort": {"total": -1}},
        {"$limit": 20},
    ]))
    issue_fields = [
        "collection_match",
        "group_by_match",
        "metric_semantic_match",
        "filter_match",
        "sort_match",
    ]
    issues = []
    for field in issue_fields:
        count = collection.count_documents({**match, f"comparison.{field}": False})
        if count:
            issues.append({"label": field, "value": count})
    recent = list(collection.find(
        match,
        {
            "_id": 0,
            "created_at": 1,
            "question": 1,
            "reference_backend": 1,
            "reference_collection": 1,
            "local_model": 1,
            "local_ok": 1,
            "local_elapsed_ms": 1,
            "local_collection": 1,
            "local_error": 1,
            "local_errors": 1,
            "comparison": 1,
        },
    ).sort("created_at", -1).limit(limit))
    training_total = training_collection.count_documents({"project": os.environ.get("Q2_PROJECT", "q2")})
    hard_case_open = hard_case_collection.count_documents({"project": os.environ.get("Q2_PROJECT", "q2"), "status": "open"})
    hard_case_resolved = hard_case_collection.count_documents({"project": os.environ.get("Q2_PROJECT", "q2"), "status": "resolved"})
    training_by_collection = list(training_collection.aggregate([
        {"$match": {"project": os.environ.get("Q2_PROJECT", "q2")}},
        {"$group": {"_id": "$collection", "count": {"$sum": 1}}},
        {"$sort": {"count": -1}},
    ]))
    eval_run = client[db_name]["local_lm_eval_run"].find_one(
        {"project": os.environ.get("Q2_PROJECT", "q2")},
        {"_id": 0},
        sort=[("created_at", -1)],
    )
    trainer_events = list(client[db_name]["local_lm_trainer_event"].find(
        {"project": os.environ.get("Q2_PROJECT", "q2")},
        {
            "_id": 0,
            "created_at": 1,
            "status": 1,
            "message": 1,
            "question": 1,
            "collection": 1,
            "quality_status": 1,
            "result_count": 1,
            "reference_collection": 1,
            "local_collection": 1,
            "semantic_same": 1,
            "local_ok": 1,
            "local_elapsed_ms": 1,
            "local_error": 1,
            "evaluation_passed": 1,
            "evaluation_total": 1,
            "total_examples": 1,
            "training_after.total_examples": 1,
            "elapsed_ms": 1,
        },
    ).sort("created_at", -1).limit(30))
    capability = local_lm_capability(
        total=total,
        semantic_evaluated=semantic_evaluated,
        semantic_same=semantic_same,
        local_ok=local_ok,
        training_total=training_total,
    )
    return {
        "ok": True,
        "days": days,
        "capability": capability,
        "training": {
            "examples": training_total,
            "by_collection": [
                {"collection": row.get("_id") or "unknown", "count": row.get("count", 0)}
                for row in training_by_collection
            ],
            "hard_cases_open": hard_case_open,
            "hard_cases_resolved": hard_case_resolved,
        },
        "evaluation": eval_run or {},
        "trainer_events": trainer_events,
        "total": total,
        "semantic_evaluated": semantic_evaluated,
        "local_ok": local_ok,
        "exact_same": exact_same,
        "semantic_same": semantic_same,
        "local_ok_ratio": round(local_ok / total, 4) if total else 0,
        "exact_same_ratio": round(exact_same / total, 4) if total else 0,
        "semantic_same_ratio": round(semantic_same / semantic_evaluated, 4) if semantic_evaluated else 0,
        "avg_local_elapsed_ms": round(avg_rows[0]["avg_ms"], 3) if avg_rows else None,
        "max_local_elapsed_ms": round(avg_rows[0]["max_ms"], 3) if avg_rows else None,
        "by_collection": [
            {
                "collection": row.get("_id") or "unknown",
                "total": row.get("total", 0),
                "local_ok": row.get("local_ok", 0),
                "semantic_same": row.get("semantic_same", 0),
                "exact_same": row.get("exact_same", 0),
            }
            for row in by_collection
        ],
        "issues": issues,
        "recent": recent,
    }


def local_lm_capability(total: int, semantic_evaluated: int, semantic_same: int, local_ok: int, training_total: int) -> dict[str, Any]:
    semantic_ratio = semantic_same / semantic_evaluated if semantic_evaluated else 0
    valid_ratio = local_ok / total if total else 0
    if semantic_evaluated < 20:
        level = "insufficient"
        title = "검증 데이터 부족"
        verdict = "로컬 LM 능력을 판단할 만큼 새 판정 데이터가 아직 부족합니다."
    elif semantic_ratio >= 0.8 and valid_ratio >= 0.9:
        level = "ready"
        title = "제품 후보"
        verdict = "제한된 범위에서 로컬 LM을 백엔드 후보로 볼 수 있습니다."
    elif semantic_ratio >= 0.5 and valid_ratio >= 0.7:
        level = "pilot"
        title = "파일럿 가능"
        verdict = "사람 검토를 붙인 파일럿 용도까지는 검토할 수 있습니다."
    elif semantic_ratio >= 0.2:
        level = "training"
        title = "훈련 중"
        verdict = "일부 패턴을 학습했지만 아직 자동 처리 백엔드로 쓰기엔 부족합니다."
    else:
        level = "not_ready"
        title = "아직 불가"
        verdict = "현재 로컬 LM은 q2 쿼리 생성기로 바로 쓰기 어렵습니다. 검증된 예시 주입과 컬렉션별 프롬프트 분리가 필요합니다."
    return {
        "level": level,
        "title": title,
        "verdict": verdict,
        "semantic_ratio": round(semantic_ratio, 4),
        "valid_ratio": round(valid_ratio, 4),
        "training_examples": training_total,
    }


@app.post("/api/local-backend/train")
def local_backend_train(limit: int = 1200):
    result = build_training_examples(limit=limit)
    return result


@app.post("/api/cache/clear", response_model=CacheStats)
def clear_cache():
    cache.clear()
    return cache_stats()


def plan_request(request: PlanRequest) -> PlanResponse:
    key = cache_key(
        request.question,
        schema_version=catalog["schema_version"],
        planner_version=PLANNER_VERSION,
        timezone=request.timezone,
    )
    template_plan = plan_from_template(request.question, timezone=request.timezone)
    cached = cache.get(key)
    if cached is not None:
        cached_errors = validate_plan(cached, catalog)
        if cached_errors:
            cache.delete(key)
        elif template_plan is not None and cached != template_plan:
            cache.set(key, template_plan)
            cached = template_plan
        if not cached_errors:
            if request.reference_time and cached.time_range.type == "relative":
                cached = cached.model_copy(
                    update={
                        "time_range": cached.time_range.model_copy(
                            update={"reference_time": request.reference_time}
                        )
                    }
                )
            aggregation = build_aggregation(cached, catalog)
            return PlanResponse(ok=True, cache_hit=True, backend="cache", plan=cached, aggregation=aggregation)

    result = pool.plan(request.question, timezone=request.timezone)
    if result.plan is None:
        return PlanResponse(ok=False, backend=result.backend, errors=[result.error or "planner failed"])

    errors = validate_plan(result.plan, catalog)
    if errors:
        return PlanResponse(ok=False, backend=result.backend, plan=result.plan, errors=errors)

    plan = result.plan
    if request.reference_time and plan.time_range.type == "relative":
        plan = plan.model_copy(
            update={
                "time_range": plan.time_range.model_copy(
                    update={"reference_time": request.reference_time}
                )
            }
        )
    cache.set(key, result.plan)
    aggregation = build_aggregation(plan, catalog)
    return PlanResponse(ok=True, cache_hit=False, backend=result.backend, plan=plan, aggregation=aggregation)


@app.post("/api/mongo-plan", response_model=PlanResponse)
def mongo_plan(request: PlanRequest, http_request: Request):
    started = time.monotonic()
    response = plan_request(request)
    schedule_local_shadow_compare(request, response, "/api/mongo-plan")
    log_llm_query("/api/mongo-plan", request, response, started, http_request.client.host if http_request.client else "")
    return response


@app.post("/api/mongo-query", response_model=PlanResponse)
def mongo_query(request: PlanRequest, http_request: Request):
    started = time.monotonic()
    try:
        investigation = investigate_person(request.question)
    except Exception as exc:
        response = PlanResponse(ok=False, backend="investigation", errors=[str(exc)])
        response.execution_ms = round((time.monotonic() - started) * 1000, 3)
        log_llm_query("/api/mongo-query", request, response, started, http_request.client.host if http_request.client else "")
        return response
    if investigation is not None:
        investigation.result_preview = enrich_result_rows(investigation.result_preview or [])
        investigation.result_count = len(investigation.result_preview)
        investigation.execution_ms = round((time.monotonic() - started) * 1000, 3)
        schedule_local_shadow_compare(request, investigation, "/api/mongo-query")
        log_llm_query("/api/mongo-query", request, investigation, started, http_request.client.host if http_request.client else "")
        return investigation

    response = plan_request(request)
    if not response.ok or response.plan is None or response.aggregation is None:
        response = assess_response(response, catalog, question=request.question)
        schedule_local_shadow_compare(request, response, "/api/mongo-query")
        log_llm_query("/api/mongo-query", request, response, started, http_request.client.host if http_request.client else "")
        return response
    try:
        response.result_preview = enrich_result_rows(run_aggregation(response.plan.collection, response.aggregation, limit=100))
        response.result_count = len(response.result_preview)
        response.execution_ms = round((time.monotonic() - started) * 1000, 3)
        response = assess_response(response, catalog, question=request.question)
        schedule_local_shadow_compare(request, response, "/api/mongo-query")
        log_llm_query("/api/mongo-query", request, response, started, http_request.client.host if http_request.client else "")
        return response
    except Exception as exc:
        response.ok = False
        response.errors.append(str(exc))
        response.execution_ms = round((time.monotonic() - started) * 1000, 3)
        response = assess_response(response, catalog, question=request.question)
        schedule_local_shadow_compare(request, response, "/api/mongo-query")
        log_llm_query("/api/mongo-query", request, response, started, http_request.client.host if http_request.client else "")
        return response
