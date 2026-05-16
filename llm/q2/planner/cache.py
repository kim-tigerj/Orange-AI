from __future__ import annotations

import hashlib
import json
import os
import re
import threading
import uuid
from dataclasses import dataclass, field
from pathlib import Path
from typing import Dict, Optional, Tuple

from .models import QueryPlan


_CACHE_SAVE_LOCK = threading.RLock()


def normalize_question(question: str) -> str:
    value = question.strip().lower()
    replacements = {
        "지난 일주일": "최근 7일",
        "최근 일주일": "최근 7일",
        "일주일간": "7일간",
        "오늘": "오늘",
    }
    for src, dst in replacements.items():
        value = value.replace(src, dst)
    value = re.sub(r"\s+", " ", value)
    return value


def cache_key(question: str, *, schema_version: str, planner_version: str, timezone: str) -> str:
    payload = {
        "normalized_question": normalize_question(question),
        "schema_version": schema_version,
        "planner_version": planner_version,
        "timezone": timezone,
    }
    data = json.dumps(payload, ensure_ascii=False, sort_keys=True)
    return hashlib.sha256(data.encode("utf-8")).hexdigest()


@dataclass
class PlanCache:
    exact: Dict[str, QueryPlan] = field(default_factory=dict)
    hits: int = 0
    misses: int = 0
    path: Optional[Path] = None

    @classmethod
    def from_env(cls) -> "PlanCache":
        path = Path(os.environ.get("Q2_PLAN_CACHE", "llm/q2/output/plan_cache.json"))
        cache = cls(path=path)
        cache.load()
        return cache

    def load(self) -> None:
        if self.path is None or not self.path.exists():
            return
        try:
            data = json.loads(self.path.read_text(encoding="utf-8"))
            self.exact = {
                key: QueryPlan.model_validate(value)
                for key, value in data.get("plans", {}).items()
            }
            self.hits = int(data.get("hits", 0))
            self.misses = int(data.get("misses", 0))
        except Exception:
            self.exact = {}

    def save(self) -> None:
        if self.path is None:
            return
        with _CACHE_SAVE_LOCK:
            self.path.parent.mkdir(parents=True, exist_ok=True)
            plans = list(self.exact.items())
            data = {
                "hits": self.hits,
                "misses": self.misses,
                "plans": {
                    key: plan.model_dump(mode="json")
                    for key, plan in plans
                },
            }
            tmp = self.path.with_name(f"{self.path.name}.{uuid.uuid4().hex}.tmp")
            tmp.write_text(json.dumps(data, ensure_ascii=False, indent=2), encoding="utf-8")
            tmp.replace(self.path)

    def get(self, key: str) -> Optional[QueryPlan]:
        with _CACHE_SAVE_LOCK:
            plan = self.exact.get(key)
            if plan is None:
                self.misses += 1
                return None
            self.hits += 1
            return plan

    def set(self, key: str, plan: QueryPlan) -> None:
        with _CACHE_SAVE_LOCK:
            self.exact[key] = plan
            self.save()

    def delete(self, key: str) -> None:
        with _CACHE_SAVE_LOCK:
            if key in self.exact:
                del self.exact[key]
                self.save()

    def clear(self) -> None:
        with _CACHE_SAVE_LOCK:
            self.exact.clear()
            self.hits = 0
            self.misses = 0
            self.save()

    def stats(self) -> Tuple[int, int, int]:
        with _CACHE_SAVE_LOCK:
            return len(self.exact), self.hits, self.misses
