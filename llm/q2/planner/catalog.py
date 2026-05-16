from __future__ import annotations

import json
from pathlib import Path
from typing import Any, Dict, Set


ROOT = Path(__file__).resolve().parents[1]
CATALOG_PATH = ROOT / "catalog" / "system_sprocess.json"


def load_catalog() -> Dict[str, Any]:
    return json.loads(CATALOG_PATH.read_text(encoding="utf-8"))


def collection_fields(catalog: Dict[str, Any], collection: str) -> Set[str]:
    return set(catalog["collections"][collection]["fields"])


def collection_time_field(catalog: Dict[str, Any], collection: str) -> str:
    return str(catalog["collections"][collection]["time_field"])

