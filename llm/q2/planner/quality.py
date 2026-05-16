from __future__ import annotations

import re
from typing import Any

from .aggregation_builder import build_aggregation
from .catalog import collection_time_field
from .models import FilterSpec, Metric, PlanResponse, QueryPlan, SortSpec, TimeRange
from .mongo_client import run_aggregation


EVENT_COLLECTIONS = {"detect", "report", "timeline"}
PRODUCT_FILTER_FIELDS = [
    "CompanyName",
    "ProductName",
    "FileDescription",
    "FileName",
    "FilePath",
    "Name",
    "Desc",
    "data.name",
    "data.displayName",
    "data.publisher",
    "data.Name",
    "data.DriverName",
    "data.HotFixID",
    "data.Caption",
    "data.version",
    "data.Version",
    "data.Model",
    "data.VideoProcessor",
    "data.InstalledDisplayDrivers",
    "data.label",
]


def assess_response(response: PlanResponse, catalog: dict[str, Any], question: str | None = None) -> PlanResponse:
    """Attach quality hints and bounded improvement probes to a query response."""
    if response.plan is None:
        response.quality_status = "failed"
        response.failure_reason = "planner_failed"
        response.candidate_fix = {
            "type": "prompt_or_template_needed",
            "reason": "planner did not produce a valid QueryPlan",
        }
        return response

    if not response.ok:
        response.quality_status = "failed"
        response.failure_reason = classify_errors(response.errors)
        response.candidate_fix = {
            "type": "runtime_failure",
            "reason": response.failure_reason,
            "action": "check Mongo connectivity, generated aggregation, or planner constraints",
        }
        return response

    if response.result_count is None:
        response.quality_status = "unknown"
        return response

    if response.result_count > 0:
        response.quality_status = "ok"
        return response

    response.quality_status = "needs_review"
    response.failure_reason = "zero_result"
    response.probes = run_zero_result_probes(response.plan, catalog)
    response.candidate_fix = choose_candidate_fix(response.plan, response.probes, question=question)
    return response


def has_explicit_time_range(question: str | None) -> bool:
    if not question:
        return False
    return bool(re.search(r"오늘|최근|지난|일주일|\d+\s*일|\d+\s*시간|\d+\s*개월|\d+\s*년", question))


def classify_errors(errors: list[str]) -> str:
    text = "\n".join(errors).lower()
    if "connection refused" in text or "serverselectiontimeout" in text or "timed out" in text:
        return "mongo_connectivity"
    if "maxtimemsexpired" in text or "operation exceeded time limit" in text:
        return "mongo_timeout"
    if "planner" in text or "validation" in text:
        return "planner_error"
    return "runtime_error"


def run_zero_result_probes(plan: QueryPlan, catalog: dict[str, Any]) -> list[dict[str, Any]]:
    probes: list[dict[str, Any]] = []
    probes.extend(time_range_probes(plan, catalog))
    probes.extend(event_collection_probes(plan, catalog))
    probes.extend(filter_field_probes(plan, catalog))
    return probes[:8]


def time_range_probes(plan: QueryPlan, catalog: dict[str, Any]) -> list[dict[str, Any]]:
    if plan.time_range.type != "relative":
        return []
    current_days = plan.time_range.days or (1 if plan.time_range.hours else 7)
    if current_days >= 365:
        return []
    probe_plan = plan.model_copy(
        update={
            "time_range": TimeRange(
                type="relative",
                days=365,
                timezone=plan.time_range.timezone,
                reference_time=plan.time_range.reference_time,
            )
        }
    )
    return [execute_probe("time_expand_365d", probe_plan, catalog, {"days": 365})]


def event_collection_probes(plan: QueryPlan, catalog: dict[str, Any]) -> list[dict[str, Any]]:
    if plan.collection not in EVENT_COLLECTIONS:
        return []
    probes = []
    for collection in sorted(EVENT_COLLECTIONS - {plan.collection}):
        probe_plan = plan.model_copy(update={
            "collection": collection,
            "metrics": remap_event_metrics(plan.metrics, collection),
            "sort": remap_event_sort(plan.sort, collection),
        })
        probes.append(execute_probe(f"collection_{collection}", probe_plan, catalog, {"collection": collection}))
    return probes


def remap_event_metrics(metrics: list[Metric], collection: str) -> list[Metric]:
    remapped = []
    for metric in metrics:
        if metric.field == "count" and metric.name in {"detect_count", "report_count", "timeline_count"}:
            remapped.append(metric.model_copy(update={"name": f"{collection}_count"}))
        else:
            remapped.append(metric)
    return remapped


def remap_event_sort(sort: list[SortSpec], collection: str) -> list[SortSpec]:
    remapped = []
    for item in sort:
        if item.field in {"detect_count", "report_count", "timeline_count"}:
            remapped.append(item.model_copy(update={"field": f"{collection}_count"}))
        else:
            remapped.append(item)
    return remapped


def filter_field_probes(plan: QueryPlan, catalog: dict[str, Any]) -> list[dict[str, Any]]:
    regex_filters = [item for item in plan.filters if item.op == "regex" and item.field in PRODUCT_FILTER_FIELDS]
    if not regex_filters:
        return []
    base_filter = regex_filters[0]
    probes = []
    for field in PRODUCT_FILTER_FIELDS:
        if field == base_filter.field:
            continue
        filters = [
            FilterSpec(field=field, op="regex", value=base_filter.value) if item is base_filter else item
            for item in plan.filters
        ]
        probe_plan = plan.model_copy(update={"filters": filters})
        probes.append(execute_probe(f"filter_field_{field}", probe_plan, catalog, {"field": field}))
    return probes


def execute_probe(name: str, plan: QueryPlan, catalog: dict[str, Any], patch: dict[str, Any]) -> dict[str, Any]:
    try:
        pipeline = build_aggregation(plan, catalog)
        rows = run_aggregation(plan.collection, pipeline, limit=3)
        return {
            "name": name,
            "ok": True,
            "collection": plan.collection,
            "patch": patch,
            "result_count": len(rows),
            "sample": rows[:1],
        }
    except Exception as exc:
        return {
            "name": name,
            "ok": False,
            "collection": plan.collection,
            "patch": patch,
            "error": str(exc),
        }


def choose_candidate_fix(plan: QueryPlan, probes: list[dict[str, Any]], question: str | None = None) -> dict[str, Any] | None:
    useful = [probe for probe in probes if probe.get("ok") and probe.get("result_count", 0) > 0]
    if not useful:
        return {
            "type": "needs_human_review",
            "reason": "zero_result and no probe found data",
            "plan_collection": plan.collection,
        }
    best = useful[0]
    if best["name"].startswith("time_expand"):
        if has_explicit_time_range(question):
            return {
                "type": "explicit_time_window_no_data",
                "reason": "requested time window returned zero rows; wider probe found older data, so do not widen automatically",
                "patch": best["patch"],
                "probe": best["name"],
            }
        return {
            "type": "default_time_range",
            "reason": "short default time range returned zero rows but wider range found data",
            "patch": best["patch"],
            "probe": best["name"],
        }
    if best["name"].startswith("collection_"):
        return {
            "type": "collection_routing",
            "reason": "alternate event collection returned data",
            "patch": best["patch"],
            "probe": best["name"],
        }
    if best["name"].startswith("filter_field_"):
        return {
            "type": "filter_field_mapping",
            "reason": "same term matched a different field",
            "patch": best["patch"],
            "probe": best["name"],
        }
    return {
        "type": "template_candidate",
        "reason": "probe found data",
        "patch": best["patch"],
        "probe": best["name"],
    }
