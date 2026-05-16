#!/usr/bin/env python3
from __future__ import annotations

import argparse
import os
import sys
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

from pymongo import ReplaceOne

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))

from llm.q2.planner.mongo_client import mongo_client_from_env, source_mongo_uri_from_env, store_mongo_client_from_env, store_mongo_uri_from_env


SUMMARY_COLLECTION = "sprocess_summary"
HOUR_MS = 60 * 60 * 1000


def utc_now() -> datetime:
    return datetime.now(timezone.utc)


def summary_pipeline(start_ts: int | None, limit: int) -> list[dict[str, Any]]:
    match: dict[str, Any] = {"timestamp": {"$type": "number"}, "CounterCount": {"$gt": 0}}
    if start_ts:
        match["timestamp"]["$gte"] = start_ts
    pipeline: list[dict[str, Any]] = [
        {"$match": match},
        {
            "$project": {
                "bucket_hour": {"$subtract": ["$timestamp", {"$mod": ["$timestamp", HOUR_MS]}]},
                "id": 1,
                "ProcName": {"$ifNull": ["$ProcName", ""]},
                "ProcPath": {"$ifNull": ["$ProcPath", ""]},
                "FilePath": {"$ifNull": ["$FilePath", ""]},
                "FileName": {"$ifNull": ["$FileName", ""]},
                "CompanyName": {"$ifNull": ["$CompanyName", ""]},
                "ProductName": {"$ifNull": ["$ProductName", ""]},
                "ProductVersion": {"$ifNull": ["$ProductVersion", ""]},
                "FileDescription": {"$ifNull": ["$FileDescription", ""]},
                "FileVersion": {"$ifNull": ["$FileVersion", ""]},
                "Signer": {"$ifNull": ["$Signer", ""]},
                "Codesign": {"$ifNull": ["$Codesign", ""]},
                "PProcName": {"$ifNull": ["$PProcName", ""]},
                "IsSystem": {"$ifNull": ["$IsSystem", None]},
                "pscore": {"$ifNull": ["$pscore", 0]},
                "CPU": {"$ifNull": ["$CPU", 0]},
                "Memory": {"$ifNull": ["$Memory", 0]},
                "IO": {"$ifNull": ["$IO", 0]},
                "Handle": {"$ifNull": ["$Handle", 0]},
                "CounterCount": {"$ifNull": ["$CounterCount", 0]},
                "Crash": {"$ifNull": ["$Crash", 0]},
                "Detect": {"$ifNull": ["$Detect", 0]},
                "Kill": {"$ifNull": ["$Kill", 0]},
                "Killed": {"$ifNull": ["$Killed", 0]},
                "Running": {"$ifNull": ["$Running", 0]},
                "Start": {"$ifNull": ["$Start", 0]},
                "Stop": {"$ifNull": ["$Stop", 0]},
                "timestamp": 1,
            }
        },
        {
            "$group": {
                "_id": {
                    "bucket_hour": "$bucket_hour",
                    "id": "$id",
                    "ProcName": "$ProcName",
                    "ProcPath": "$ProcPath",
                    "FilePath": "$FilePath",
                    "FileName": "$FileName",
                    "CompanyName": "$CompanyName",
                    "ProductName": "$ProductName",
                    "ProductVersion": "$ProductVersion",
                    "FileDescription": "$FileDescription",
                    "FileVersion": "$FileVersion",
                    "Signer": "$Signer",
                    "Codesign": "$Codesign",
                    "PProcName": "$PProcName",
                    "IsSystem": "$IsSystem",
                },
                "pscore": {"$sum": "$pscore"},
                "CPU": {"$sum": "$CPU"},
                "Memory": {"$sum": "$Memory"},
                "IO": {"$sum": "$IO"},
                "Handle": {"$sum": "$Handle"},
                "CounterCount": {"$sum": "$CounterCount"},
                "Crash": {"$sum": "$Crash"},
                "Detect": {"$sum": "$Detect"},
                "Kill": {"$sum": "$Kill"},
                "Killed": {"$sum": "$Killed"},
                "Running": {"$sum": "$Running"},
                "Start": {"$sum": "$Start"},
                "Stop": {"$sum": "$Stop"},
                "raw_count": {"$sum": 1},
                "timestamp": {"$max": "$timestamp"},
            }
        },
        {
            "$project": {
                "_id": 0,
                "summary_key": {
                    "$concat": [
                        {"$toString": "$_id.bucket_hour"},
                        "|",
                        {"$toString": "$_id.id"},
                        "|",
                        "$_id.ProcName",
                        "|",
                        "$_id.ProcPath",
                        "|",
                        "$_id.FilePath",
                        "|",
                        "$_id.FileName",
                        "|",
                        "$_id.CompanyName",
                        "|",
                        "$_id.ProductName",
                        "|",
                        "$_id.ProductVersion",
                        "|",
                        "$_id.FileDescription",
                        "|",
                        "$_id.FileVersion",
                        "|",
                        "$_id.Signer",
                        "|",
                        "$_id.Codesign",
                        "|",
                        "$_id.PProcName",
                        "|",
                        {"$toString": "$_id.IsSystem"},
                    ]
                },
                "bucket_hour": "$_id.bucket_hour",
                "id": "$_id.id",
                "ProcName": "$_id.ProcName",
                "ProcPath": "$_id.ProcPath",
                "FilePath": "$_id.FilePath",
                "FileName": "$_id.FileName",
                "CompanyName": "$_id.CompanyName",
                "ProductName": "$_id.ProductName",
                "ProductVersion": "$_id.ProductVersion",
                "FileDescription": "$_id.FileDescription",
                "FileVersion": "$_id.FileVersion",
                "Signer": "$_id.Signer",
                "Codesign": "$_id.Codesign",
                "PProcName": "$_id.PProcName",
                "IsSystem": "$_id.IsSystem",
                "pscore": 1,
                "CPU": 1,
                "Memory": 1,
                "IO": 1,
                "Handle": 1,
                "CounterCount": 1,
                "Crash": 1,
                "Detect": 1,
                "Kill": 1,
                "Killed": 1,
                "Running": 1,
                "Start": 1,
                "Stop": 1,
                "raw_count": 1,
                "timestamp": 1,
                "summary_grain": "hour",
                "updated_at": utc_now(),
            }
        },
    ]
    if limit:
        pipeline.append({"$limit": limit})
    return pipeline


def ensure_indexes(collection) -> None:
    collection.create_index([("summary_key", 1)], unique=True)
    collection.create_index([("timestamp", -1)])
    collection.create_index([("ProcName", 1), ("timestamp", -1)])
    collection.create_index([("CompanyName", 1), ("timestamp", -1)])
    collection.create_index([("ProductName", 1), ("timestamp", -1)])
    collection.create_index([("Signer", 1), ("timestamp", -1)])
    collection.create_index([("id", 1), ("timestamp", -1)])


def main() -> int:
    parser = argparse.ArgumentParser(description="Build q2 ai.sprocess_summary from public.sprocess")
    parser.add_argument("--days", type=int, default=int(os.environ.get("Q2_SPROCESS_SUMMARY_DAYS", "30")))
    parser.add_argument("--limit", type=int, default=0, help="debug limit before writing, 0 means all matched rows")
    parser.add_argument("--batch-size", type=int, default=1000)
    parser.add_argument("--rebuild", action="store_true")
    args = parser.parse_args()

    source_client = mongo_client_from_env()
    _, source_db_name = source_mongo_uri_from_env()
    store_client = store_mongo_client_from_env()
    _, store_db_name = store_mongo_uri_from_env()
    source = source_client[source_db_name]["sprocess"]
    target = store_client[store_db_name][SUMMARY_COLLECTION]

    ensure_indexes(target)
    if args.rebuild:
        target.delete_many({})

    max_ts_row = source.find_one({"timestamp": {"$type": "number"}}, {"timestamp": 1}, sort=[("timestamp", -1)])
    max_ts = int((max_ts_row or {}).get("timestamp") or 0)
    start_ts = max_ts - args.days * 24 * HOUR_MS if max_ts and args.days > 0 else None

    processed = 0
    operations: list[ReplaceOne] = []
    for row in source.aggregate(summary_pipeline(start_ts, args.limit), allowDiskUse=True, maxTimeMS=120000):
        operations.append(ReplaceOne({"summary_key": row["summary_key"]}, row, upsert=True))
        if len(operations) >= args.batch_size:
            result = target.bulk_write(operations, ordered=False)
            processed += result.upserted_count + result.modified_count
            operations.clear()
    if operations:
        result = target.bulk_write(operations, ordered=False)
        processed += result.upserted_count + result.modified_count

    meta = {
        "project": os.environ.get("Q2_PROJECT", "q2"),
        "collection": SUMMARY_COLLECTION,
        "created_at": utc_now(),
        "source_db": source_db_name,
        "store_db": store_db_name,
        "source_max_timestamp": max_ts,
        "start_timestamp": start_ts,
        "days": args.days,
        "processed": processed,
        "summary_count": target.estimated_document_count(),
    }
    store_client[store_db_name]["summary_job"].insert_one(meta)
    print(meta)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
