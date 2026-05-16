#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
import os
import sys
from collections import Counter
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))

from llm.q2.planner.mongo_client import mongo_client_from_env, source_mongo_uri_from_env, upsert_q2_document


PROFILED_COLLECTIONS = ["node", "nodeinfo", "sprocess", "filelist", "detect", "report", "timeline", "system"]


def utc_now() -> datetime:
    return datetime.now(timezone.utc)


def compact(value: Any, limit: int = 160) -> Any:
    if isinstance(value, str):
        return value if len(value) <= limit else value[:limit] + "..."
    if isinstance(value, (int, float, bool)) or value is None:
        return value
    return str(value)[:limit]


def flatten_keys(value: Any, prefix: str = "", depth: int = 0, max_depth: int = 4) -> set[str]:
    if depth > max_depth:
        return set()
    keys: set[str] = set()
    if isinstance(value, dict):
        for key, child in value.items():
            path = f"{prefix}.{key}" if prefix else str(key)
            keys.add(path)
            keys.update(flatten_keys(child, path, depth + 1, max_depth))
    elif isinstance(value, list):
        for item in value[:5]:
            keys.update(flatten_keys(item, prefix, depth + 1, max_depth))
    return keys


def top_values(collection, field: str, match: dict[str, Any] | None = None, limit: int = 40) -> list[dict[str, Any]]:
    pipeline: list[dict[str, Any]] = []
    if match:
        pipeline.append({"$match": match})
    pipeline.extend(
        [
            {"$unwind": {"path": f"${field.split('.')[0]}", "preserveNullAndEmptyArrays": True}} if field.startswith("data.") else {"$match": {}},
            {"$group": {"_id": f"${field}", "count": {"$sum": 1}}},
            {"$sort": {"count": -1}},
            {"$limit": limit},
        ]
    )
    if pipeline and pipeline[0] == {"$match": {}}:
        pipeline = pipeline[1:]
    rows = list(collection.aggregate(pipeline, allowDiskUse=True, maxTimeMS=30000))
    return [{"value": compact(row.get("_id")), "count": row.get("count", 0)} for row in rows]


def top_unwound_data_values(
    collection,
    field: str,
    *,
    root_match: dict[str, Any] | None = None,
    data_match: dict[str, Any] | None = None,
    limit: int = 40,
) -> list[dict[str, Any]]:
    pipeline: list[dict[str, Any]] = []
    if root_match:
        pipeline.append({"$match": root_match})
    pipeline.append({"$unwind": "$data"})
    if data_match:
        pipeline.append({"$match": data_match})
    pipeline.extend(
        [
            {"$group": {"_id": f"${field}", "count": {"$sum": 1}}},
            {"$sort": {"count": -1}},
            {"$limit": limit},
        ]
    )
    rows = list(collection.aggregate(pipeline, allowDiskUse=True, maxTimeMS=30000))
    return [{"value": compact(row.get("_id")), "count": row.get("count", 0)} for row in rows]


def profile_nodeinfo(collection) -> dict[str, Any]:
    by_name = list(
        collection.aggregate(
            [
                {"$group": {"_id": "$name", "docs": {"$sum": 1}, "nodes": {"$addToSet": "$id"}}},
                {"$project": {"_id": 0, "name": "$_id", "docs": 1, "node_count": {"$size": "$nodes"}}},
                {"$sort": {"docs": -1}},
            ],
            allowDiskUse=True,
            maxTimeMS=30000,
        )
    )
    sections = []
    for item in by_name:
        name = item["name"]
        sample = collection.find_one({"name": name}, {"_id": 0, "data": 1})
        data = (sample or {}).get("data")
        first = data[0] if isinstance(data, list) and data else data if isinstance(data, dict) else {}
        keys = sorted(flatten_keys(first))
        section: dict[str, Any] = {
            "name": name,
            "docs": item["docs"],
            "node_count": item["node_count"],
            "data_keys": keys,
        }
        if name == "UNINSTALL":
            section["top_products"] = top_values(collection, "data.name", {"name": name}, limit=80)
            section["top_versions"] = top_values(collection, "data.version", {"name": name}, limit=60)
            section["top_publishers"] = top_values(collection, "data.publisher", {"name": name}, limit=60)
            section["top_install_location_products"] = top_unwound_data_values(
                collection,
                "data.name",
                root_match={"name": name},
                data_match={
                    "data.name": {"$exists": True, "$nin": ["", None]},
                    "data.installLocation": {"$exists": True, "$nin": ["", None]},
                },
                limit=80,
            )
            section["top_install_location_publishers"] = top_unwound_data_values(
                collection,
                "data.publisher",
                root_match={"name": name},
                data_match={
                    "data.publisher": {"$exists": True, "$nin": ["", None]},
                    "data.installLocation": {"$exists": True, "$nin": ["", None]},
                },
                limit=80,
            )
            section["top_install_source_products"] = top_unwound_data_values(
                collection,
                "data.name",
                root_match={"name": name},
                data_match={
                    "data.name": {"$exists": True, "$nin": ["", None]},
                    "data.installSource": {"$exists": True, "$nin": ["", None]},
                },
                limit=80,
            )
            section["top_uninstall_string_products"] = top_unwound_data_values(
                collection,
                "data.name",
                root_match={"name": name},
                data_match={
                    "data.name": {"$exists": True, "$nin": ["", None]},
                    "data.uninstallString": {"$exists": True, "$nin": ["", None]},
                },
                limit=80,
            )
            section["top_path_products"] = top_unwound_data_values(
                collection,
                "data.name",
                root_match={"name": name},
                data_match={
                    "data.name": {"$exists": True, "$nin": ["", None]},
                    "data.Path": {"$exists": True, "$nin": ["", None]},
                },
                limit=80,
            )
            section["top_path_publishers"] = top_unwound_data_values(
                collection,
                "data.publisher",
                root_match={"name": name},
                data_match={
                    "data.publisher": {"$exists": True, "$nin": ["", None]},
                    "data.Path": {"$exists": True, "$nin": ["", None]},
                },
                limit=80,
            )
        elif name == "VACCINE":
            section["top_products"] = top_values(collection, "data.displayName", {"name": name}, limit=40)
            section["top_statuses"] = top_values(collection, "data.Status", {"name": name}, limit=20)
            section["top_signatures"] = top_values(collection, "data.Signature", {"name": name}, limit=20)
            section["top_states"] = section["top_statuses"]
        elif name == "UPDATE":
            section["top_hotfixes"] = top_values(collection, "data.HotFixID", {"name": name}, limit=80)
        elif name == "OS":
            section["top_os"] = top_values(collection, "data.Caption", {"name": name}, limit=40)
            section["top_builds"] = top_values(collection, "data.BuildNumber", {"name": name}, limit=40)
        elif name == "SYSTEM":
            section["top_manufacturers"] = top_values(collection, "data.Manufacturer", {"name": name}, limit=60)
            section["top_models"] = top_values(collection, "data.Model", {"name": name}, limit=80)
        elif name in {"CPU", "MEMORY", "DISK", "DISKDRIVE", "NETWORKADAPTER", "PRINTER", "VIDEOCARD", "AGENT"}:
            probe_fields = {
                "CPU": "data.Name",
                "MEMORY": "data.Manufacturer",
                "DISK": "data.Model",
                "DISKDRIVE": "data.FileSystem",
                "NETWORKADAPTER": "data.Name",
                "PRINTER": "data.DriverName",
                "VIDEOCARD": "data.Name",
                "AGENT": "data.data",
            }
            section["top_values"] = top_values(collection, probe_fields[name], {"name": name}, limit=80)
        sections.append(section)
    return {"sections": sections}


def profile_filelist(collection) -> dict[str, Any]:
    return {
        "top_file_names": top_values(collection, "FileName", limit=80),
        "top_company_names": top_values(collection, "CompanyName", limit=80),
        "top_product_names": top_values(collection, "ProductName", limit=80),
        "top_product_versions": top_values(collection, "ProductVersion", limit=80),
        "top_file_versions": top_values(collection, "FileVersion", limit=80),
        "top_signers": top_values(collection, "Signer", limit=60),
        "top_install_paths": top_values(collection, "InstallPath", limit=80),
        "top_extensions": [
            {"value": compact(row.get("_id")), "count": row.get("count", 0)}
            for row in collection.aggregate(
                    [
                        {"$match": {"FileName": {"$type": "string", "$ne": ""}}},
                        {
                            "$project": {
                                "extension": {
                                    "$toLower": {
                                        "$ifNull": [
                                            {
                                                "$arrayElemAt": [
                                                    {"$split": ["$FileName", "."]},
                                                    -1,
                                                ]
                                            },
                                            "",
                                        ]
                                    }
                                }
                            }
                        },
                        {"$match": {"extension": {"$nin": ["", None]}}},
                        {"$group": {"_id": "$extension", "count": {"$sum": 1}}},
                        {"$sort": {"count": -1}},
                        {"$limit": 40},
                    ],
                    allowDiskUse=True,
                    maxTimeMS=30000,
                )
        ],
    }


def profile_sprocess(collection) -> dict[str, Any]:
    return {
        "top_proc_names": top_values(collection, "ProcName", limit=100),
        "top_company_names": top_values(collection, "CompanyName", limit=80),
        "top_product_names": top_values(collection, "ProductName", limit=80),
        "top_file_descriptions": top_values(collection, "FileDescription", limit=80),
        "top_signers": top_values(collection, "Signer", limit=60),
        "top_parent_proc_names": top_values(collection, "PProcName", limit=60),
        "top_paths": top_values(collection, "ProcPath", limit=80),
    }


def profile_system(collection) -> dict[str, Any]:
    return {
        "time_range": list(
            collection.aggregate(
                [
                    {
                        "$group": {
                            "_id": None,
                            "min": {"$min": "$time"},
                            "max": {"$max": "$time"},
                            "count": {"$sum": 1},
                        }
                    }
                ],
                maxTimeMS=30000,
            )
        ),
        "top_health": top_values(collection, "health", limit=20),
        "top_commands": top_values(collection, "command", limit=20),
    }


def profile_node(collection) -> dict[str, Any]:
    return {
        "top_users": top_values(collection, "User.user.name", limit=80),
        "top_roles": top_values(collection, "User.user.role", limit=40),
        "top_statuses": top_values(collection, "status", limit=20),
        "top_computer_names": top_values(collection, "System.ComputerName", limit=80),
        "top_manufacturers": top_values(collection, "System.Manufacturer", limit=80),
        "top_models": top_values(collection, "System.Model", limit=80),
        "top_os_names": top_values(collection, "System.name", limit=60),
        "top_agent_versions": top_values(collection, "Agent.version", limit=60),
    }


def profile_generic(collection, sample_limit: int = 200) -> dict[str, Any]:
    count = collection.estimated_document_count()
    sample_rows = list(collection.find({}, {"_id": 0}).limit(sample_limit))
    key_counter: Counter[str] = Counter()
    value_samples: dict[str, list[Any]] = {}
    for row in sample_rows:
        for key in flatten_keys(row):
            key_counter[key] += 1
        for key, value in row.items():
            value_samples.setdefault(key, [])
            if len(value_samples[key]) < 5:
                value_samples[key].append(compact(value))
    profile: dict[str, Any] = {
        "count": count,
        "sample_size": len(sample_rows),
        "top_keys": [{"field": key, "sample_hits": hits} for key, hits in key_counter.most_common(80)],
        "samples": value_samples,
    }
    for time_field in ["timestamp", "time", "created", "updated", "lasttime"]:
        try:
            rows = list(
                collection.aggregate(
                    [{"$group": {"_id": None, "min": {"$min": f"${time_field}"}, "max": {"$max": f"${time_field}"}, "count": {"$sum": 1}}}],
                    maxTimeMS=30000,
                )
            )
        except Exception:
            continue
        if rows and (rows[0].get("min") is not None or rows[0].get("max") is not None):
            profile.setdefault("time_ranges", {})[time_field] = {
                "min": compact(rows[0].get("min")),
                "max": compact(rows[0].get("max")),
                "count": rows[0].get("count", 0),
            }
    return profile


def profile_collection(db, name: str, sample_limit: int) -> dict[str, Any]:
    collection = db[name]
    profile = profile_generic(collection, sample_limit=sample_limit)
    if name == "nodeinfo":
        profile.update(profile_nodeinfo(collection))
    elif name == "filelist":
        profile.update(profile_filelist(collection))
    elif name == "sprocess":
        profile.update(profile_sprocess(collection))
    elif name == "system":
        profile.update(profile_system(collection))
    elif name == "node":
        profile.update(profile_node(collection))
    return profile


def main() -> int:
    parser = argparse.ArgumentParser(description="Profile Orange source data for q2")
    parser.add_argument("--collections", default=",".join(PROFILED_COLLECTIONS))
    parser.add_argument("--sample-limit", type=int, default=300)
    args = parser.parse_args()

    client = mongo_client_from_env()
    _, db_name = source_mongo_uri_from_env()
    db = client[db_name]
    run_id = utc_now().strftime("%Y%m%d%H%M%S")
    profiles: dict[str, Any] = {}
    for name in [item.strip() for item in args.collections.split(",") if item.strip()]:
        profiles[name] = profile_collection(db, name, args.sample_limit)
        document = {
            "project": os.environ.get("Q2_PROJECT", "q2"),
            "run_id": run_id,
            "collection": name,
            "created_at": utc_now(),
            "source_db": db_name,
            "profile": profiles[name],
        }
        upsert_q2_document("data_profile", {"project": document["project"], "collection": name}, document)

    summary = {
        "project": os.environ.get("Q2_PROJECT", "q2"),
        "run_id": run_id,
        "created_at": utc_now(),
        "source_db": db_name,
        "collections": sorted(profiles),
        "nodeinfo_sections": [
            {"name": item["name"], "node_count": item["node_count"], "docs": item["docs"]}
            for item in profiles.get("nodeinfo", {}).get("sections", [])
        ],
    }
    upsert_q2_document("data_profile", {"project": summary["project"], "collection": "_summary"}, {**summary, "collection": "_summary"})
    print(json.dumps(summary, ensure_ascii=False, default=str, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
