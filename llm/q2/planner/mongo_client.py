from __future__ import annotations

import os
from typing import Any, Dict, List, Optional
from urllib.parse import quote_plus

from pymongo import MongoClient
from pymongo.errors import PyMongoError

SPROCESS_SUMMARY_COLLECTION = "sprocess_summary"
SPROCESS_SUMMARY_FIELDS = {
    "timestamp",
    "bucket_hour",
    "id",
    "ProcName",
    "ProcPath",
    "FilePath",
    "FileName",
    "CompanyName",
    "ProductName",
    "ProductVersion",
    "FileDescription",
    "FileVersion",
    "Signer",
    "Codesign",
    "PProcName",
    "IsSystem",
    "pscore",
    "CPU",
    "Memory",
    "IO",
    "Handle",
    "CounterCount",
    "Crash",
    "Detect",
    "Kill",
    "Killed",
    "Running",
    "Start",
    "Stop",
    "raw_count",
}


def mongo_timeout_ms() -> int:
    try:
        return max(500, int(os.environ.get("Q2_MONGO_TIMEOUT_MS", "1500")))
    except ValueError:
        return 1500


def _mongo_uri_from_env(prefix: str, default_db: str, fallback_prefix: str | None = None) -> tuple[str, str]:
    db_name = os.environ.get(f"{prefix}_DB") or (os.environ.get(f"{fallback_prefix}_DB") if fallback_prefix else None) or default_db
    uri = os.environ.get(f"{prefix}_URI") or (os.environ.get(f"{fallback_prefix}_URI") if fallback_prefix else None)
    if uri:
        return uri, db_name
    host = os.environ.get(f"{prefix}_HOST") or (os.environ.get(f"{fallback_prefix}_HOST") if fallback_prefix else None) or "127.0.0.1"
    port = os.environ.get(f"{prefix}_PORT") or (os.environ.get(f"{fallback_prefix}_PORT") if fallback_prefix else None) or "27017"
    auth_source = os.environ.get(f"{prefix}_AUTH_SOURCE") or (os.environ.get(f"{fallback_prefix}_AUTH_SOURCE") if fallback_prefix else None) or db_name
    user = os.environ.get(f"{prefix}_USER") or (os.environ.get(f"{fallback_prefix}_USER") if fallback_prefix else None)
    password = os.environ.get(f"{prefix}_PASSWORD") or (os.environ.get(f"{fallback_prefix}_PASSWORD") if fallback_prefix else None)
    auth = f"{quote_plus(user)}:{quote_plus(password)}@" if user and password else ""
    return f"mongodb://{auth}{host}:{port}/{db_name}?authSource={auth_source}", db_name


def source_mongo_uri_from_env() -> tuple[str, str]:
    return _mongo_uri_from_env("ORANGE_SOURCE_MONGO", "public", fallback_prefix="ORANGE_MONGO")


def store_mongo_uri_from_env() -> tuple[str, str]:
    return _mongo_uri_from_env("ORANGE_STORE_MONGO", "ai", fallback_prefix="ORANGE_MONGO")


def mongo_uri_from_env() -> tuple[str, str]:
    return source_mongo_uri_from_env()


def mongo_client_from_env() -> MongoClient:
    uri, _ = source_mongo_uri_from_env()
    client = MongoClient(uri, serverSelectionTimeoutMS=mongo_timeout_ms())
    client.admin.command("ping")
    return client


def store_mongo_client_from_env() -> MongoClient:
    uri, _ = store_mongo_uri_from_env()
    client = MongoClient(uri, serverSelectionTimeoutMS=mongo_timeout_ms())
    client.admin.command("ping")
    return client


def sprocess_summary_enabled() -> bool:
    return os.environ.get("Q2_SPROCESS_SUMMARY_ENABLED", "1") not in {"0", "false", "False", "no"}


def _referenced_fields(value: Any) -> set[str]:
    fields: set[str] = set()
    if isinstance(value, str):
        if value.startswith("$") and not value.startswith("$$"):
            fields.add(value[1:].split(".", 1)[0])
        return fields
    if isinstance(value, list):
        for item in value:
            fields.update(_referenced_fields(item))
        return fields
    if isinstance(value, dict):
        for _key, child in value.items():
            fields.update(_referenced_fields(child))
    return fields


def _match_fields(value: Any) -> set[str]:
    fields: set[str] = set()
    if isinstance(value, list):
        for item in value:
            fields.update(_match_fields(item))
        return fields
    if isinstance(value, dict):
        for key, child in value.items():
            if key.startswith("$"):
                fields.update(_match_fields(child))
            else:
                fields.add(key.split(".", 1)[0])
                fields.update(_referenced_fields(child))
    return fields


def can_use_sprocess_summary(pipeline: List[Dict[str, Any]]) -> bool:
    """Return true when a sprocess pipeline can run on ai.sprocess_summary.

    The summary keeps cumulative metric field names, so existing avg_by_count
    aggregations remain valid. Pipelines that need raw-only fields such as
    Command or ticket stay on public.sprocess.
    """
    for stage in pipeline:
        if "$lookup" in stage:
            continue
        if "$match" in stage:
            fields = _match_fields(stage["$match"])
            if not fields <= SPROCESS_SUMMARY_FIELDS:
                return False
            continue
        if "$group" in stage:
            fields = _referenced_fields(stage["$group"])
            return fields <= SPROCESS_SUMMARY_FIELDS
        fields = _referenced_fields(stage)
        if not fields <= SPROCESS_SUMMARY_FIELDS:
            return False
    return True


def run_aggregation(collection: str, pipeline: List[Dict[str, Any]], limit: int = 100) -> List[Dict[str, Any]]:
    source_collection = "sprocess" if collection == "sprocess_nodeinfo" else collection
    if collection == "sprocess" and sprocess_summary_enabled() and can_use_sprocess_summary(pipeline):
        store_uri, store_db_name = store_mongo_uri_from_env()
        store_client = MongoClient(store_uri, serverSelectionTimeoutMS=mongo_timeout_ms())
        store_client.admin.command("ping")
        try:
            if store_client[store_db_name][SPROCESS_SUMMARY_COLLECTION].estimated_document_count() > 0:
                return list(
                    store_client[store_db_name][SPROCESS_SUMMARY_COLLECTION].aggregate(
                        pipeline,
                        allowDiskUse=True,
                        maxTimeMS=30000,
                    )
                )[:limit]
        except PyMongoError:
            pass
    uri, db_name = source_mongo_uri_from_env()
    client = MongoClient(uri, serverSelectionTimeoutMS=mongo_timeout_ms())
    client.admin.command("ping")
    return list(client[db_name][source_collection].aggregate(pipeline, allowDiskUse=True, maxTimeMS=30000))[:limit]


def insert_document(collection: str, document: Dict[str, Any]) -> None:
    uri, db_name = source_mongo_uri_from_env()
    client = MongoClient(uri, serverSelectionTimeoutMS=mongo_timeout_ms())
    client.admin.command("ping")
    client[db_name][collection].insert_one(document)


def insert_ai_document(collection: str, document: Dict[str, Any]) -> None:
    uri, ai_db_name = store_mongo_uri_from_env()
    client = MongoClient(uri, serverSelectionTimeoutMS=mongo_timeout_ms())
    client.admin.command("ping")
    db = client[ai_db_name]
    if collection == "llm_query":
        try:
            db[collection].create_index([("created_at", -1)])
            db[collection].create_index([("project", 1), ("created_at", -1)])
            db[collection].create_index([("question", 1)])
            db[collection].create_index([("ok", 1), ("collection", 1), ("created_at", -1)])
        except PyMongoError:
            pass
    if collection == "llm_improvement_candidate":
        try:
            db[collection].create_index([("created_at", -1)])
            db[collection].create_index([("project", 1), ("status", 1), ("created_at", -1)])
            db[collection].create_index([("candidate_fix.type", 1), ("created_at", -1)])
        except PyMongoError:
            pass
    db[collection].insert_one(document)


def q2_store_targets() -> list[tuple[str, str]]:
    _, store_db = store_mongo_uri_from_env()
    return [(store_db, "{collection}")]


def q2_collection(client: MongoClient, collection: str):
    last_error: Exception | None = None
    for db_name, pattern in q2_store_targets():
        collection_name = pattern.format(collection=collection)
        db = client[db_name]
        try:
            db.command("collStats", collection_name)
            return db[collection_name], db_name, collection_name
        except PyMongoError as exc:
            last_error = exc
            try:
                db[collection_name].find_one({}, {"_id": 1})
                return db[collection_name], db_name, collection_name
            except PyMongoError as find_exc:
                last_error = find_exc
                continue
    if last_error:
        raise last_error
    raise RuntimeError("no q2 store target available")


def ensure_q2_indexes(collection, logical_name: str) -> None:
    try:
        collection.create_index([("created_at", -1)])
        collection.create_index([("project", 1), ("created_at", -1)])
        if logical_name == "llm_query":
            collection.create_index([("question", 1)])
            collection.create_index([("ok", 1), ("collection", 1), ("created_at", -1)])
            collection.create_index([("quality_status", 1), ("failure_reason", 1), ("created_at", -1)])
        elif logical_name == "llm_improvement_candidate":
            collection.create_index([("candidate_id", 1)], unique=True)
            collection.create_index([("status", 1), ("created_at", -1)])
            collection.create_index([("candidate_fix.type", 1), ("created_at", -1)])
        elif logical_name == "autotest_status":
            collection.create_index([("run_id", 1), ("updated_at", -1)])
            collection.create_index([("state", 1), ("updated_at", -1)])
        elif logical_name == "improvement_request":
            collection.create_index([("request_id", 1)], unique=True)
            collection.create_index([("project", 1), ("status", 1), ("created_at", -1)])
        elif logical_name == "improvement_run":
            collection.create_index([("run_id", 1)], unique=True)
            collection.create_index([("project", 1), ("status", 1), ("created_at", -1)])
        elif logical_name == "autoresearch_result":
            collection.create_index([("run_id", 1), ("stream_index", 1)], unique=True)
            collection.create_index([("project", 1), ("created_at", -1)])
            collection.create_index([("project", 1), ("status", 1), ("created_at", -1)])
            collection.create_index([("project", 1), ("collection", 1), ("created_at", -1)])
        elif logical_name == "macbook_worker_event":
            collection.create_index([("project", 1), ("created_at", -1)])
            collection.create_index([("project", 1), ("worker", 1), ("created_at", -1)])
            collection.create_index([("project", 1), ("status", 1), ("created_at", -1)])
        elif logical_name == "llm_backend_compare":
            collection.create_index([("project", 1), ("created_at", -1)])
            collection.create_index([("project", 1), ("local_model", 1), ("created_at", -1)])
            collection.create_index([("project", 1), ("local_ok", 1), ("created_at", -1)])
            collection.create_index([("project", 1), ("comparison.same", 1), ("created_at", -1)])
        elif logical_name == "local_lm_training_example":
            collection.create_index([("project", 1), ("question_key", 1)], unique=True)
            collection.create_index([("project", 1), ("collection", 1), ("priority", -1), ("last_seen", -1)])
            collection.create_index([("project", 1), ("last_seen", -1)])
        elif logical_name == "local_lm_eval_run":
            collection.create_index([("project", 1), ("created_at", -1)])
            collection.create_index([("project", 1), ("run_id", 1)], unique=True)
        elif logical_name == "local_lm_trainer_event":
            collection.create_index([("project", 1), ("created_at", -1)])
            collection.create_index([("project", 1), ("status", 1), ("created_at", -1)])
            collection.create_index([("project", 1), ("worker", 1), ("created_at", -1)])
        elif logical_name == "local_lm_hard_case":
            collection.create_index([("project", 1), ("question_key", 1)], unique=True)
            collection.create_index([("project", 1), ("status", 1), ("updated_at", -1)])
            collection.create_index([("project", 1), ("collection", 1), ("updated_at", -1)])
    except PyMongoError:
        pass


def insert_q2_document(logical_name: str, document: Dict[str, Any]) -> Dict[str, Any]:
    client = store_mongo_client_from_env()
    last_error: Exception | None = None
    for db_name, pattern in q2_store_targets():
        collection_name = pattern.format(collection=logical_name)
        collection = client[db_name][collection_name]
        try:
            ensure_q2_indexes(collection, logical_name)
            collection.insert_one(document)
            return {"db": db_name, "collection": collection_name}
        except PyMongoError as exc:
            last_error = exc
            continue
    if last_error:
        raise last_error
    raise RuntimeError("no q2 store target available")


def upsert_q2_document(logical_name: str, query: Dict[str, Any], document: Dict[str, Any]) -> Dict[str, Any]:
    client = store_mongo_client_from_env()
    last_error: Exception | None = None
    for db_name, pattern in q2_store_targets():
        collection_name = pattern.format(collection=logical_name)
        collection = client[db_name][collection_name]
        try:
            ensure_q2_indexes(collection, logical_name)
            collection.update_one(query, {"$set": document}, upsert=True)
            return {"db": db_name, "collection": collection_name}
        except PyMongoError as exc:
            last_error = exc
            continue
    if last_error:
        raise last_error
    raise RuntimeError("no q2 store target available")


def find_q2_documents(
    logical_name: str,
    query: Optional[Dict[str, Any]] = None,
    limit: int = 100,
    sort: Optional[List[tuple[str, int]]] = None,
) -> list[Dict[str, Any]]:
    client = store_mongo_client_from_env()
    last_error: Exception | None = None
    for db_name, pattern in q2_store_targets():
        collection_name = pattern.format(collection=logical_name)
        collection = client[db_name][collection_name]
        try:
            cursor = collection.find(query or {}, {"_id": 0})
            if sort:
                cursor = cursor.sort(sort)
            if limit:
                cursor = cursor.limit(limit)
            return list(cursor)
        except PyMongoError as exc:
            last_error = exc
            continue
    if last_error:
        raise last_error
    return []


def count_q2_documents(logical_name: str, query: Optional[Dict[str, Any]] = None) -> int:
    client = store_mongo_client_from_env()
    last_error: Exception | None = None
    for db_name, pattern in q2_store_targets():
        collection_name = pattern.format(collection=logical_name)
        collection = client[db_name][collection_name]
        try:
            return int(collection.count_documents(query or {}))
        except PyMongoError as exc:
            last_error = exc
            continue
    if last_error:
        raise last_error
    return 0


def q2_store_status(logical_name: str) -> Dict[str, Any]:
    client = store_mongo_client_from_env()
    last_error: Exception | None = None
    for db_name, pattern in q2_store_targets():
        collection_name = pattern.format(collection=logical_name)
        collection = client[db_name][collection_name]
        try:
            return {
                "db": db_name,
                "collection": collection_name,
                "count": int(collection.count_documents({})),
            }
        except PyMongoError as exc:
            last_error = exc
            continue
    if last_error:
        raise last_error
    return {"error": "no q2 store target available"}


def ai_collection_count(collection: str, query: Optional[Dict[str, Any]] = None) -> int:
    uri, ai_db_name = store_mongo_uri_from_env()
    client = MongoClient(uri, serverSelectionTimeoutMS=mongo_timeout_ms())
    client.admin.command("ping")
    return int(client[ai_db_name][collection].count_documents(query or {}))
