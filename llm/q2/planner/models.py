from __future__ import annotations

from typing import Any, Dict, List, Literal, Optional

from pydantic import BaseModel, Field


class TimeRange(BaseModel):
    type: Literal["relative", "absolute"]
    timezone: str = "Asia/Seoul"
    days: Optional[int] = Field(default=None, ge=1, le=365)
    hours: Optional[int] = Field(default=None, ge=1, le=8760)
    start: Optional[str] = None
    end: Optional[str] = None
    reference_time: Optional[str] = None


class Metric(BaseModel):
    name: str
    op: Literal["sum", "avg", "max", "min", "count", "count_distinct", "avg_by_count"]
    field: str


class SortSpec(BaseModel):
    field: str
    direction: Literal["asc", "desc"] = "desc"


class FilterSpec(BaseModel):
    field: str
    op: Literal["eq", "ne", "gt", "gte", "lt", "lte", "regex", "exists", "missing", "empty", "not_empty"]
    value: Optional[Any] = None


class QueryPlan(BaseModel):
    plan_type: Literal["mongo_query"] = "mongo_query"
    collection: Literal["system", "sprocess", "node", "nodeinfo", "sprocess_nodeinfo", "filelist", "detect", "report", "timeline", "command_template", "command", "user", "group"]
    time_range: TimeRange
    group_by: List[str] = Field(default_factory=list)
    metrics: List[Metric]
    filters: List[FilterSpec] = Field(default_factory=list)
    filter_mode: Literal["all", "any"] = "all"
    sort: List[SortSpec]
    limit: int = Field(default=100, ge=1, le=1000)
    limit_per_group: Optional[int] = Field(default=None, ge=1, le=100)
    output: Literal["table", "line_chart", "bar_chart", "pie_chart"] = "table"
    notes: List[str] = Field(default_factory=list)


class PlanRequest(BaseModel):
    question: str
    collections: List[Literal["system", "sprocess", "node", "nodeinfo", "sprocess_nodeinfo", "filelist", "detect", "report", "timeline", "command_template", "command", "user", "group"]] = Field(default_factory=lambda: ["system", "sprocess", "node", "nodeinfo", "sprocess_nodeinfo", "filelist", "detect", "report", "timeline", "command_template", "command", "user", "group"])
    timezone: str = "Asia/Seoul"
    execute: bool = False
    reference_time: Optional[str] = None


class PlanResponse(BaseModel):
    ok: bool
    cache_hit: bool = False
    backend: str
    plan: Optional[QueryPlan] = None
    aggregation: Optional[List[Dict[str, Any]]] = None
    errors: List[str] = Field(default_factory=list)
    execution_ms: Optional[float] = None
    result_count: Optional[int] = None
    result_preview: Optional[List[Dict[str, Any]]] = None
    quality_status: Optional[Literal["unknown", "ok", "failed", "needs_review"]] = None
    failure_reason: Optional[str] = None
    probes: List[Dict[str, Any]] = Field(default_factory=list)
    candidate_fix: Optional[Dict[str, Any]] = None


class CacheStats(BaseModel):
    exact_size: int
    hits: int
    misses: int
    path: Optional[str] = None
