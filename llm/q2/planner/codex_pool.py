from __future__ import annotations

import json
import os
import subprocess
import tempfile
from concurrent.futures import ThreadPoolExecutor, TimeoutError as FutureTimeout
from dataclasses import dataclass
from pathlib import Path
from typing import Optional

from .models import QueryPlan
from .template_planner import plan_from_template


PLANNER_VERSION = "codex-planner-v46"
DEFAULT_MODEL_LIBRARY = "Codex CLI"
ROOT = Path(__file__).resolve().parents[3]
SCHEMA_PATH = ROOT / "llm" / "q2" / "query_plan_schema.json"
PROMPT_PATH = ROOT / "llm" / "q2" / "prompts" / "system_sprocess_planner.md"


@dataclass
class PlannerResult:
    backend: str
    plan: Optional[QueryPlan]
    raw: str = ""
    error: str = ""


class CodexPool:
    """Bounded Codex CLI backend facade."""

    def __init__(self, timeout_seconds: int = 90):
        self.timeout_seconds = int(os.environ.get("Q2_CODEX_TIMEOUT", str(timeout_seconds)))
        self.codex_enabled = os.environ.get("Q2_CODEX_ENABLED") == "1"
        self.workers = max(1, int(os.environ.get("Q2_CODEX_WORKERS", "2")))
        self.executor = ThreadPoolExecutor(max_workers=self.workers, thread_name_prefix="q2-codex")

    def status(self) -> dict:
        return {
            "codex_enabled": self.codex_enabled,
            "model_library": os.environ.get("Q2_MODEL_LIBRARY", DEFAULT_MODEL_LIBRARY),
            "model": os.environ.get("Q2_CODEX_MODEL", ""),
            "backend": "codex-cli" if self.codex_enabled else "template",
            "workers": self.workers,
            "timeout_seconds": self.timeout_seconds,
        }

    def plan(self, question: str, timezone: str = "Asia/Seoul") -> PlannerResult:
        template_plan = plan_from_template(question, timezone=timezone)
        if template_plan is not None:
            return PlannerResult(backend="template", plan=template_plan)
        if not self.codex_enabled:
            return PlannerResult(
                backend="template",
                plan=None,
                error="no template matched and Q2_CODEX_ENABLED is not set",
            )
        future = self.executor.submit(self.plan_with_codex_exec, question, timezone)
        try:
            return future.result(timeout=self.timeout_seconds + 5)
        except FutureTimeout:
            return PlannerResult(backend="codex-exec", plan=None, error="codex worker timeout")
        except Exception as exc:
            return PlannerResult(backend="codex-exec", plan=None, error=f"codex worker failed: {exc}")

    def plan_with_codex_exec(self, question: str, timezone: str) -> PlannerResult:
        prompt = build_prompt(question, timezone)
        try:
            with tempfile.NamedTemporaryFile("w+", suffix=".json", delete=False) as out:
                output_path = out.name
            completed = subprocess.run(
                [
                    "codex",
                    "-a",
                    "never",
                    "--sandbox",
                    "read-only",
                    "-C",
                    str(ROOT),
                    "exec",
                    "--output-schema",
                    str(SCHEMA_PATH),
                    "--output-last-message",
                    output_path,
                    prompt,
                ],
                text=True,
                capture_output=True,
                timeout=self.timeout_seconds,
                check=False,
            )
        except subprocess.TimeoutExpired:
            return PlannerResult(backend="codex-exec", plan=None, error="codex timeout")
        except OSError as exc:
            return PlannerResult(backend="codex-exec", plan=None, error=f"codex execution failed: {exc}")
        raw = read_output_file(output_path) or completed.stdout.strip()
        try:
            data = json.loads(extract_json(raw))
            return PlannerResult(backend="codex-exec", plan=QueryPlan.model_validate(data), raw=raw)
        except Exception as exc:
            stderr = completed.stderr.strip()
            error = str(exc)
            if completed.returncode != 0 and stderr:
                error = f"{error}; codex exit={completed.returncode}; stderr={stderr[-500:]}"
            return PlannerResult(backend="codex-exec", plan=None, raw=raw, error=error)


def read_output_file(path: str) -> str:
    try:
        output_path = Path(path)
        value = output_path.read_text(encoding="utf-8").strip()
        output_path.unlink(missing_ok=True)
        return value
    except Exception:
        return ""


def extract_json(raw: str) -> str:
    value = raw.strip()
    if value.startswith("{") and value.endswith("}"):
        return value
    start = value.find("{")
    end = value.rfind("}")
    if start >= 0 and end > start:
        return value[start:end + 1]
    return value


def build_prompt(question: str, timezone: str) -> str:
    template = PROMPT_PATH.read_text(encoding="utf-8")
    return template.replace("{question}", question).replace("{timezone}", timezone)
