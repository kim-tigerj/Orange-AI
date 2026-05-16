from __future__ import annotations

import json
from functools import lru_cache
from pathlib import Path
from typing import Any, Dict, List, Set

from .catalog import collection_fields
from .models import QueryPlan


DERIVED_GROUPS = {"weekday", "hour", "date"}
JOINED_NODEINFO_GROUPS = {
    "node.CSName",
    "node.OSCaption",
    "node.BuildNumber",
    "node.UBR",
    "node.OSArchitecture",
    "node.CPUName",
    "node.Cores",
    "node.LogicalProcessors",
    "node.MaxClockSpeed",
    "target.status",
    "target.ComputerName",
    "target.UserName",
    "target.UserRole",
    "target.ip",
    "target.Manufacturer",
    "target.Model",
}
PROFILED_NODEINFO_FIELDS = {
    "data.best",
    "data.BIOS.Manufacturer",
    "data.BIOS.Name",
    "data.BIOS.ReleaseDate",
    "data.BaseBoard.Manufacturer",
    "data.BaseBoard.Product",
    "data.BaseBoard.SerialNumber",
    "data.BaseBoard.Status",
    "data.SystemType",
}
OPS = {"sum", "avg", "max", "min", "count", "count_distinct", "avg_by_count"}
COUNTER_AVG_FIELDS = {"pscore", "CPU", "Memory", "IO", "Handle"}


@lru_cache(maxsize=1)
def profiled_nodeinfo_fields() -> Set[str]:
    """Allow fields observed by the local nodeinfo profiler.

    The static catalog intentionally stays small, but q2_autoresearch generates
    profile-grounded questions from the latest inventory shape. Keep validation
    aligned with that profiler so nested nodeinfo fields do not fail at runtime
    just because the catalog has not been refreshed yet.
    """
    profile_path = Path(__file__).resolve().parents[1] / "output" / "nodeinfo_profile.json"
    try:
        profile = json.loads(profile_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return set()
    fields = profile.get("fields")
    if not isinstance(fields, dict):
        return set()
    return {f"data.{field}" for field in fields if isinstance(field, str) and field}


def validate_plan(plan: QueryPlan, catalog: Dict[str, Any]) -> List[str]:
    errors: List[str] = []
    collections = catalog.get("collections", {})
    if plan.collection not in collections:
        return [f"collection not allowed: {plan.collection}"]

    fields: Set[str] = collection_fields(catalog, plan.collection)
    if plan.collection == "nodeinfo":
        fields |= PROFILED_NODEINFO_FIELDS | profiled_nodeinfo_fields()
    allowed_group = fields | DERIVED_GROUPS
    if plan.collection in {"nodeinfo", "sprocess_nodeinfo"}:
        allowed_group |= JOINED_NODEINFO_GROUPS

    if not plan.time_range:
        errors.append("time_range is required")
    if not plan.group_by:
        errors.append("group_by is required")

    for group in plan.group_by:
        if group not in allowed_group:
            errors.append(f"group_by field not allowed: {group}")

    for item in plan.filters:
        if item.field not in fields:
            errors.append(f"filter field not allowed: {item.field}")
        if item.op in {"regex", "eq", "ne", "gt", "gte", "lt", "lte"} and item.value is None:
            errors.append(f"filter value is required for {item.op}: {item.field}")

    metric_names = set()
    for metric in plan.metrics:
        metric_names.add(metric.name)
        if metric.op not in OPS:
            errors.append(f"metric op not allowed: {metric.op}")
        if metric.op != "count" and metric.field not in fields:
            errors.append(f"metric field not allowed: {metric.field}")
        if metric.op == "avg_by_count":
            if plan.collection not in {"sprocess", "sprocess_nodeinfo"}:
                errors.append("avg_by_count is only allowed for sprocess or sprocess_nodeinfo")
            if metric.field not in COUNTER_AVG_FIELDS:
                errors.append(f"avg_by_count field must be one of {sorted(COUNTER_AVG_FIELDS)}")
            if "CounterCount" not in fields:
                errors.append("CounterCount field is required for avg_by_count")

    sortable = metric_names | set(plan.group_by)
    for sort in plan.sort:
        if sort.field not in sortable:
            errors.append(f"sort field must be metric or group field: {sort.field}")

    return errors
