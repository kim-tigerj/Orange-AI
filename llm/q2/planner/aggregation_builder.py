from __future__ import annotations

from datetime import datetime, timedelta, timezone
from typing import Any, Dict, List
from zoneinfo import ZoneInfo

from .catalog import collection_time_field
from .models import FilterSpec, QueryPlan


def now_utc() -> datetime:
    return datetime.now(timezone.utc)


def reference_now(plan: QueryPlan) -> datetime:
    if not plan.time_range.reference_time:
        return now_utc()
    ref = datetime.fromisoformat(plan.time_range.reference_time.replace("Z", "+00:00"))
    if ref.tzinfo is None:
        ref = ref.replace(tzinfo=ZoneInfo(plan.time_range.timezone))
    return ref.astimezone(timezone.utc)


def time_match(plan: QueryPlan, time_field: str) -> Dict[str, Any]:
    if plan.time_range.type == "relative":
        if plan.time_range.hours:
            start = reference_now(plan) - timedelta(hours=plan.time_range.hours)
        else:
            start = reference_now(plan) - timedelta(days=plan.time_range.days or 7)
        if time_field == "timestamp":
            return {time_field: {"$gte": int(start.timestamp() * 1000)}}
        return {time_field: {"$gte": int(start.timestamp())}}

    if not plan.time_range.start or not plan.time_range.end:
        raise ValueError("absolute time_range requires start and end")
    start = datetime.fromisoformat(plan.time_range.start)
    end = datetime.fromisoformat(plan.time_range.end)
    if start.tzinfo is None:
        start = start.replace(tzinfo=ZoneInfo(plan.time_range.timezone))
    if end.tzinfo is None:
        end = end.replace(tzinfo=ZoneInfo(plan.time_range.timezone))
    if time_field == "timestamp":
        return {time_field: {"$gte": int(start.timestamp() * 1000), "$lt": int(end.timestamp() * 1000)}}
    return {time_field: {"$gte": int(start.timestamp()), "$lt": int(end.timestamp())}}


def filter_match_item(item: FilterSpec) -> Dict[str, Any]:
    field = source_field(item.field)
    if item.op == "eq":
        return {field: item.value}
    if item.op == "ne":
        return {field: {"$ne": item.value}}
    if item.op == "gt":
        return {field: {"$gt": item.value}}
    if item.op == "gte":
        return {field: {"$gte": item.value}}
    if item.op == "lt":
        return {field: {"$lt": item.value}}
    if item.op == "lte":
        return {field: {"$lte": item.value}}
    if item.op == "regex":
        return {field: {"$regex": str(item.value), "$options": "i"}}
    if item.op == "exists":
        return {field: {"$exists": True, "$nin": [None, ""]}}
    if item.op == "missing":
        return {"$or": [{field: {"$exists": False}}, {field: None}, {field: ""}]}
    if item.op == "empty":
        return {"$or": [{field: None}, {field: ""}]}
    if item.op == "not_empty":
        return {field: {"$nin": [None, ""]}}
    raise ValueError(f"unsupported filter op: {item.op}")


def build_match(plan: QueryPlan, time_field: str) -> Dict[str, Any]:
    time_clause = time_match(plan, time_field) if time_field else {}
    filter_clauses = [filter_match_item(item) for item in plan.filters]
    if not filter_clauses:
        return time_clause
    if not time_clause:
        operator = "$or" if plan.filter_mode == "any" else "$and"
        return {operator: filter_clauses}
    operator = "$or" if plan.filter_mode == "any" else "$and"
    return {"$and": [time_clause, {operator: filter_clauses}]}


def build_match_for_filters(filters: List[FilterSpec], filter_mode: str) -> Dict[str, Any]:
    clauses = [filter_match_item(item) for item in filters]
    if not clauses:
        return {}
    if len(clauses) == 1:
        return clauses[0]
    operator = "$or" if filter_mode == "any" else "$and"
    return {operator: clauses}


def date_expr(time_field: str, timezone_name: str) -> Dict[str, Any]:
    if time_field == "timestamp":
        return {"$toDate": f"${time_field}"}
    return {"$toDate": {"$multiply": [f"${time_field}", 1000]}}


def group_expr(group: str, time_field: str, timezone_name: str) -> Any:
    date_value = date_expr(time_field, timezone_name)
    if group == "weekday":
        return {"$dayOfWeek": {"date": date_value, "timezone": timezone_name}}
    if group == "hour":
        return {"$hour": {"date": date_value, "timezone": timezone_name}}
    if group == "date":
        return {"$dateToString": {"format": "%Y-%m-%d", "date": date_value, "timezone": timezone_name}}
    return f"${source_field(group)}"


def output_alias(field: str) -> str:
    return field.replace(".", "_").replace("[]", "items")


def source_field(field: str) -> str:
    node_map = {
        "node.CSName": "node_os.CSName",
        "node.OSCaption": "node_os.Caption",
        "node.BuildNumber": "node_os.BuildNumber",
        "node.UBR": "node_os.UBR",
        "node.OSArchitecture": "node_os.OSArchitecture",
        "node.CPUName": "node_cpu.CPUName",
        "node.Cores": "node_cpu.Cores",
        "node.LogicalProcessors": "node_cpu.LogicalProcessors",
        "node.MaxClockSpeed": "node_cpu.MaxClockSpeed",
        "target.status": "target_node.status",
        "target.ComputerName": "target_node.System.ComputerName",
        "target.UserName": "target_node.User.user.name",
        "target.UserRole": "target_node.User.user.role",
        "target.ip": "target_node.System.ip",
        "target.Manufacturer": "target_node.System.Manufacturer",
        "target.Model": "target_node.System.Model",
    }
    return node_map.get(field, field)


def metric_expr(op: str, field: str) -> Dict[str, Any]:
    if op == "count":
        return {"$sum": 1}
    if op == "count_distinct":
        return {"$addToSet": f"${source_field(field)}"}
    mongo_op = {"sum": "$sum", "avg": "$avg", "max": "$max", "min": "$min"}[op]
    return {mongo_op: f"${source_field(field)}"}


def avg_by_count_fields(metric_name: str) -> tuple[str, str]:
    return f"__{metric_name}_sum", f"__{metric_name}_counter_sum"


def needs_disk_size_conversion(plan: QueryPlan) -> bool:
    fields = set(plan.group_by)
    fields.update(item.field for item in plan.filters)
    fields.update(metric.field for metric in plan.metrics)
    fields.update(sort.field for sort in plan.sort)
    return bool({"data.FreeSpaceGB", "data.SizeGB"} & fields)


def disk_size_conversion_stage() -> Dict[str, Any]:
    def to_gb(source: str) -> Dict[str, Any]:
        return {
            "$convert": {
                "input": {
                    "$replaceAll": {
                        "input": {
                            "$replaceAll": {
                                "input": {"$toString": source},
                                "find": "G",
                                "replacement": "",
                            }
                        },
                        "find": ",",
                        "replacement": "",
                    }
                },
                "to": "double",
                "onError": None,
                "onNull": None,
            }
        }

    return {
        "$addFields": {
            "data.FreeSpaceGB": to_gb("$data.FreeSpace"),
            "data.SizeGB": to_gb("$data.Size"),
        }
    }


def build_aggregation(plan: QueryPlan, catalog: Dict[str, Any]) -> List[Dict[str, Any]]:
    time_field = collection_time_field(catalog, plan.collection)
    if plan.collection == "sprocess_nodeinfo":
        return build_sprocess_nodeinfo_aggregation(plan, time_field)

    pre_filters = [
        item for item in plan.filters
        if not item.field.startswith("node.") and not (plan.collection == "nodeinfo" and item.field.startswith("data."))
    ]
    post_filters = [item for item in plan.filters if item.field.startswith("node.")]
    pre_plan = plan.model_copy(update={"filters": pre_filters})
    first_match = build_match(pre_plan, time_field)
    pipeline: List[Dict[str, Any]] = [{"$match": first_match}] if first_match else []
    if plan.collection == "nodeinfo" and needs_data_unwind(plan):
        pipeline.append({"$unwind": "$data"})
        if needs_disk_size_conversion(plan):
            pipeline.append(disk_size_conversion_stage())
        data_filters = [item for item in plan.filters if item.field.startswith("data.")]
        data_match = build_match_for_filters(data_filters, "all")
        if data_match:
            pipeline.append({"$match": data_match})
    if plan.collection == "nodeinfo" and needs_target_node_lookup(plan):
        pipeline.extend(target_node_lookup_stages())
    if plan.collection == "nodeinfo" and needs_nodeinfo_lookup(plan):
        pipeline.extend(nodeinfo_lookup_stages())
        post_match = build_match_for_filters(post_filters, plan.filter_mode)
        if post_match:
            pipeline.append({"$match": post_match})
    if plan.collection == "sprocess_nodeinfo":
        pipeline.extend(nodeinfo_lookup_stages())
        post_match = build_match_for_filters(post_filters, plan.filter_mode)
        if post_match:
            pipeline.append({"$match": post_match})

    group_id = {
        output_alias(group): group_expr(group, time_field, plan.time_range.timezone)
        for group in plan.group_by
    }
    group_stage: Dict[str, Any] = {"_id": group_id}
    for metric in plan.metrics:
        if metric.op == "avg_by_count":
            value_sum, counter_sum = avg_by_count_fields(metric.name)
            group_stage[value_sum] = {"$sum": f"${metric.field}"}
            group_stage[counter_sum] = {"$sum": "$CounterCount"}
        else:
            group_stage[metric.name] = metric_expr(metric.op, metric.field)
    pipeline.append({"$group": group_stage})

    project: Dict[str, Any] = {"_id": 0}
    for group in plan.group_by:
        alias = output_alias(group)
        project[alias] = f"$_id.{alias}"
    for metric in plan.metrics:
        if metric.op == "avg_by_count":
            value_sum, counter_sum = avg_by_count_fields(metric.name)
            project[metric.name] = {
                "$cond": [
                    {"$gt": [f"${counter_sum}", 0]},
                    {"$divide": [f"${value_sum}", f"${counter_sum}"]},
                    None,
                ]
            }
        elif metric.op == "count_distinct":
            project[metric.name] = {"$size": f"${metric.name}"}
        else:
            project[metric.name] = 1
    pipeline.append({"$project": project})

    if plan.sort:
        sort_spec = {
            output_alias(sort.field) if sort.field in plan.group_by else sort.field: 1 if sort.direction == "asc" else -1
            for sort in plan.sort
        }
        pipeline.append({"$sort": sort_spec})
        if plan.limit_per_group and plan.group_by:
            pipeline.extend([
                {
                    "$setWindowFields": {
                        "partitionBy": f"${plan.group_by[0]}",
                        "sortBy": sort_spec,
                        "output": {"_q2_rank": {"$documentNumber": {}}},
                    }
                },
                {"$match": {"_q2_rank": {"$lte": plan.limit_per_group}}},
                {"$unset": "_q2_rank"},
            ])
    pipeline.append({"$limit": plan.limit})
    return pipeline


def build_sprocess_nodeinfo_aggregation(plan: QueryPlan, time_field: str) -> List[Dict[str, Any]]:
    pre_filters = [item for item in plan.filters if not (item.field.startswith("node.") or item.field.startswith("target."))]
    post_filters = [item for item in plan.filters if item.field.startswith("node.") or item.field.startswith("target.")]
    pre_plan = plan.model_copy(update={"filters": pre_filters})
    first_match = build_match(pre_plan, time_field)
    pipeline: List[Dict[str, Any]] = [{"$match": first_match}] if first_match else []

    pre_lookup_groups = [
        group
        for group in plan.group_by
        if not (group.startswith("node.") or group.startswith("target."))
    ]
    if "id" not in pre_lookup_groups:
        pre_lookup_groups.insert(0, "id")
    group_id = {
        output_alias(group): group_expr(group, time_field, plan.time_range.timezone)
        for group in pre_lookup_groups
    }
    group_stage: Dict[str, Any] = {"_id": group_id}
    for metric in plan.metrics:
        if metric.op == "avg_by_count":
            value_sum, counter_sum = avg_by_count_fields(metric.name)
            group_stage[value_sum] = {"$sum": f"${metric.field}"}
            group_stage[counter_sum] = {"$sum": "$CounterCount"}
        else:
            group_stage[metric.name] = metric_expr(metric.op, metric.field)
    pipeline.append({"$group": group_stage})

    project: Dict[str, Any] = {"_id": 0}
    for group in pre_lookup_groups:
        alias = output_alias(group)
        project[alias] = f"$_id.{alias}"
    for metric in plan.metrics:
        if metric.op == "avg_by_count":
            value_sum, counter_sum = avg_by_count_fields(metric.name)
            project[metric.name] = {
                "$cond": [
                    {"$gt": [f"${counter_sum}", 0]},
                    {"$divide": [f"${value_sum}", f"${counter_sum}"]},
                    None,
                ]
            }
        elif metric.op == "count_distinct":
            project[metric.name] = {"$size": f"${metric.name}"}
        else:
            project[metric.name] = 1
    pipeline.append({"$project": project})

    pipeline.extend(nodeinfo_lookup_stages())
    if any(
        field.startswith("target.")
        for field in [*plan.group_by, *(item.field for item in plan.filters), *(sort.field for sort in plan.sort)]
    ):
        pipeline.extend(target_node_lookup_stages())
    post_match = build_match_for_filters(post_filters, plan.filter_mode)
    if post_match:
        pipeline.append({"$match": post_match})

    add_fields = {}
    for group in plan.group_by:
        if group.startswith("node.") or group.startswith("target."):
            add_fields[output_alias(group)] = f"${source_field(group)}"
    if add_fields:
        pipeline.append({"$addFields": add_fields})

    if plan.sort:
        sort_spec = {
            output_alias(sort.field) if sort.field in plan.group_by else sort.field: 1 if sort.direction == "asc" else -1
            for sort in plan.sort
        }
        pipeline.append({"$sort": sort_spec})
    pipeline.append({"$limit": plan.limit})
    return pipeline


def needs_data_unwind(plan: QueryPlan) -> bool:
    fields = list(plan.group_by)
    fields.extend(metric.field for metric in plan.metrics)
    fields.extend(item.field for item in plan.filters)
    return any(field.startswith("data.") for field in fields)


def needs_target_node_lookup(plan: QueryPlan) -> bool:
    fields = list(plan.group_by)
    fields.extend(metric.field for metric in plan.metrics)
    fields.extend(item.field for item in plan.filters)
    fields.extend(sort.field for sort in plan.sort)
    return any(field.startswith("target.") for field in fields)


def needs_nodeinfo_lookup(plan: QueryPlan) -> bool:
    fields = list(plan.group_by)
    fields.extend(metric.field for metric in plan.metrics)
    fields.extend(item.field for item in plan.filters)
    fields.extend(sort.field for sort in plan.sort)
    return any(field.startswith("node.") for field in fields)


def target_node_lookup_stages() -> List[Dict[str, Any]]:
    return [
        {
            "$lookup": {
                "from": "node",
                "localField": "id",
                "foreignField": "id",
                "as": "target_node",
            }
        },
        {"$unwind": {"path": "$target_node", "preserveNullAndEmptyArrays": True}},
    ]


def nodeinfo_lookup_stages() -> List[Dict[str, Any]]:
    return [
        {
            "$lookup": {
                "from": "nodeinfo",
                "let": {"node_id": "$id"},
                "pipeline": [
                    {"$match": {"$expr": {"$and": [{"$eq": ["$id", "$$node_id"]}, {"$eq": ["$name", "OS"]}]}}},
                    {"$unwind": "$data"},
                    {
                        "$project": {
                            "_id": 0,
                            "CSName": "$data.CSName",
                            "Caption": "$data.Caption",
                            "BuildNumber": "$data.BuildNumber",
                            "UBR": "$data.UBR",
                            "OSArchitecture": "$data.OSArchitecture",
                        }
                    },
                    {"$limit": 1},
                ],
                "as": "node_os",
            }
        },
        {"$unwind": {"path": "$node_os", "preserveNullAndEmptyArrays": True}},
        {
            "$lookup": {
                "from": "nodeinfo",
                "let": {"node_id": "$id"},
                "pipeline": [
                    {"$match": {"$expr": {"$and": [{"$eq": ["$id", "$$node_id"]}, {"$eq": ["$name", "CPU"]}]}}},
                    {"$unwind": "$data"},
                    {
                        "$group": {
                            "_id": None,
                            "CPUName": {"$first": "$data.Name"},
                            "Cores": {"$max": "$data.NumberOfCores"},
                            "LogicalProcessors": {"$max": "$data.NumberOfLogicalProcessors"},
                            "MaxClockSpeed": {"$max": "$data.MaxClockSpeed"},
                        }
                    },
                    {"$project": {"_id": 0}},
                ],
                "as": "node_cpu",
            }
        },
        {"$unwind": {"path": "$node_cpu", "preserveNullAndEmptyArrays": True}},
    ]
