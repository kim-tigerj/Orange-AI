from __future__ import annotations

import json
import os
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Optional
from urllib.error import URLError
from urllib.request import Request, urlopen

from .codex_pool import build_prompt, extract_json
from .local_training import inject_training_examples
from .models import QueryPlan


ROOT = Path(__file__).resolve().parents[3]
SCHEMA_PATH = ROOT / "llm" / "q2" / "query_plan_schema.json"


@dataclass
class LocalPlannerResult:
    backend: str
    model: str
    plan: Optional[QueryPlan]
    raw: str = ""
    error: str = ""
    elapsed_ms: float = 0.0


class OllamaPlanner:
    def __init__(self) -> None:
        self.enabled = os.environ.get("Q2_LOCAL_ENABLED", "1") not in {"0", "false", "False", "no"}
        self.base_url = os.environ.get("Q2_OLLAMA_URL", "http://127.0.0.1:11434").rstrip("/")
        self.model = os.environ.get("Q2_OLLAMA_MODEL", "qwen3-coder:30b")
        self.timeout_seconds = float(os.environ.get("Q2_OLLAMA_TIMEOUT", "120"))

    def status(self) -> dict:
        return {
            "enabled": self.enabled,
            "library": "Ollama",
            "backend": "ollama",
            "model": self.model,
            "url": self.base_url,
            "timeout_seconds": self.timeout_seconds,
        }

    def health(self) -> dict:
        if not self.enabled:
            return {**self.status(), "ok": False, "state": "disabled"}
        try:
            with urlopen(f"{self.base_url}/api/tags", timeout=3) as response:
                data = json.loads(response.read().decode("utf-8"))
            models = [item.get("name") for item in data.get("models", [])]
            return {**self.status(), "ok": True, "models": models, "model_available": self.model in models}
        except Exception as exc:
            return {**self.status(), "ok": False, "state": "unavailable", "error": str(exc)}

    def plan(self, question: str, timezone: str = "Asia/Seoul") -> LocalPlannerResult:
        started = time.monotonic()
        if not self.enabled:
            return LocalPlannerResult("ollama", self.model, None, error="local backend disabled")
        prompt = inject_training_examples(build_prompt(question, timezone), question)
        schema = json.loads(SCHEMA_PATH.read_text(encoding="utf-8"))
        payload = {
            "model": self.model,
            "prompt": prompt,
            "stream": False,
            "format": schema,
            "options": {
                "temperature": float(os.environ.get("Q2_OLLAMA_TEMPERATURE", "0")),
                "num_predict": int(os.environ.get("Q2_OLLAMA_NUM_PREDICT", "2048")),
            },
        }
        request = Request(
            f"{self.base_url}/api/generate",
            data=json.dumps(payload).encode("utf-8"),
            headers={"Content-Type": "application/json"},
            method="POST",
        )
        try:
            with urlopen(request, timeout=self.timeout_seconds) as response:
                data = json.loads(response.read().decode("utf-8"))
            raw = str(data.get("response") or "")
            plan = QueryPlan.model_validate(json.loads(extract_json(raw)))
            return LocalPlannerResult(
                "ollama",
                self.model,
                plan,
                raw=raw,
                elapsed_ms=round((time.monotonic() - started) * 1000, 3),
            )
        except URLError as exc:
            return LocalPlannerResult(
                "ollama",
                self.model,
                None,
                error=f"ollama unavailable: {exc}",
                elapsed_ms=round((time.monotonic() - started) * 1000, 3),
            )
        except Exception as exc:
            return LocalPlannerResult(
                "ollama",
                self.model,
                None,
                error=str(exc),
                elapsed_ms=round((time.monotonic() - started) * 1000, 3),
            )
