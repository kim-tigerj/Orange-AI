#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
import random
import re
import sys
import time
import uuid
from pathlib import Path
from typing import Any

from q2_autotest import (
    DEFAULT_SERVER,
    Scenario,
    base_scenarios,
    dataset_reference_time,
    generated_scenarios,
    request_json,
    run_scenario,
    summarize,
)
from q2_autoresearch import by_section, install_question_product, latest_profiles, profile_scenarios, top_values
from q2_autoresearch import blocked_install_product as profile_blocked_install_product


LOW_VALUE_INSTALL_PRODUCT_PATTERNS = (
    r"(?<![a-z0-9])wdk(?![a-z0-9])",
    r"(?<![a-z0-9])adk(?![a-z0-9])",
    r"\bwindows\s+driver\s+kit\b",
    r"\bwindows\s+driver\s+development\s+kit\b",
    r"\bdriver\s+kit\b",
    r"\bwindows\s+kits\b",
    r"\bwindows\s+sdk\b",
    r"\bsdk\b",
    r"\bsoftware\s+development\s+kit\b",
    r"\bdeveloper\s+kit\b",
    r"\bassessment\s+and\s+deployment\s+kit\b",
    r"\bdeployment\s+kit\b",
    r"\bdebugging\s+tools\s+for\s+windows\b",
    r"\bwindows\s+debugging\s+tools\b",
)

MAX_PROFILED_INSTALL_PRODUCT_VARIANTS_PER_POOL = 1
MAX_PROFILED_NODEINFO_VALUE_VARIANTS_PER_POOL = 1
SEEN_LOOKUP_LIMIT = 10000
QUESTION_DEDUPE_SOURCE_PREFIXES = (
    "autoresearch:",
    "controlled-variant",
    "tanium/itam-grounded",
    "tanium/itam-benchmark",
)
NODEINFO_VALUE_LABELS = (
    "CPU 모델",
    "메모리 제조사",
    "디스크 모델",
    "디스크 파일시스템",
    "네트워크 어댑터",
    "프린터 드라이버",
    "그래픽 카드",
    "에이전트 버전",
)
NODEINFO_VALUE_LABEL_ALIASES = (
    ("CPU 모델", ("CPU 모델", "CPU")),
    ("메모리 제조사", ("메모리 제조사",)),
    ("디스크 모델", ("디스크 모델", "디스크")),
    ("디스크 파일시스템", ("디스크 파일시스템",)),
    ("네트워크 어댑터", ("네트워크 어댑터",)),
    ("프린터 드라이버", ("프린터 드라이버", "프린터")),
    ("그래픽 카드", ("그래픽 카드", "그래픽 드라이버", "비디오카드", "디스플레이 어댑터")),
    ("에이전트 버전", ("에이전트 버전",)),
)
NODEINFO_PROFILE_VALUE_SOURCES = (
    ("CPU", "CPU 모델", "top_values"),
    ("MEMORY", "메모리 제조사", "top_values"),
    ("DISK", "디스크 모델", "top_values"),
    ("DISKDRIVE", "디스크 파일시스템", "top_values"),
    ("NETWORKADAPTER", "네트워크 어댑터", "top_values"),
    ("PRINTER", "프린터 드라이버", "top_values"),
    ("VIDEOCARD", "그래픽 카드", "top_values"),
    ("AGENT", "에이전트 버전", "top_values"),
)
FILELIST_VALUE_PATTERNS = (
    ("unsigned-file", re.compile(r"(?:서명\s*없는|서명이\s*없는|미서명).{0,20}파일", re.IGNORECASE)),
    ("signer", re.compile(r"(?:파일\s*인벤토리(?:에서|의)?\s*)?(.+?)\s*서명\s*파일", re.IGNORECASE)),
    ("company", re.compile(r"(?:파일\s*인벤토리(?:에서|의)?\s*)?(.+?)\s*회사\s*파일", re.IGNORECASE)),
    ("product", re.compile(r"(?:파일\s*인벤토리(?:에서|의)?\s*)?(.+?)\s*제품\s*파일", re.IGNORECASE)),
    ("file", re.compile(r"(.+?)\s*파일(?:을|이|은|는)?\s*(?:장비|경로|버전|분포|목록|보여|찾)", re.IGNORECASE)),
)
FILELIST_FIELD_TOKENS = {
    "codesign",
    "companyname",
    "filename",
    "filepath",
    "fileversion",
    "hosturl",
    "issystem",
    "productname",
    "productversion",
    "referrerurl",
    "signer",
    "zoneid",
    "서명자",
    "제품",
    "제품명",
    "제품버전",
    "제품 버전",
    "버전",
    "회사",
    "회사명",
    "파일명",
    "파일버전",
    "파일 버전",
    "경로",
    "출처",
    "다운로드 출처",
}


def normalize_question(question: str) -> str:
    return re.sub(r"\s+", " ", question.strip()).lower()


def normalize_inventory_product_key_value(value: str) -> str:
    product = normalize_question(install_question_product(value) or value)
    if not product:
        return ""
    visual_cpp = re.search(
        r"\b(?:microsoft\s+)?visual\s+c\+\+(?:\s+(\d{4}(?:\s*(?:-|to)?\s*\d{4})?))?.*?\bredistributable\b",
        product,
    )
    if visual_cpp:
        year = re.sub(r"\s*-\s*", " ", visual_cpp.group(1) or "").strip()
        if year:
            return f"microsoft visual c++ {year} redistributable"
        return "microsoft visual c++ redistributable"
    product = re.sub(r"\b\d+(?:[._-]\d+){1,5}\b", " ", product)
    product = re.sub(r"\b(?:x86|x64|64-bit|32-bit)\b", " ", product)
    product = re.sub(r"[^0-9a-z가-힣.+#]+", " ", product).strip()
    return re.sub(r"\s+", " ", product)[:80]


def product_aliases(value: str) -> set[str]:
    product = normalize_inventory_product_key_value(value)
    aliases = {product} if product else set()
    if re.match(r"microsoft visual c\+\+(?: \d{4}(?: \d{4})?)? redistributable$", product):
        aliases.add("microsoft visual c++ redistributable")
        aliases.add("visual c++ redistributable")
    if "google chrome" in product:
        aliases.add("chrome")
    if "microsoft edge" in product:
        aliases.add("edge")
    if "windows defender" in product:
        aliases.add("defender")
    if "ahnlab" in product or "v3" in product:
        aliases.update({"ahnlab", "안랩", "v3"})
    if "알약" in product:
        aliases.add("알약")
    return {alias for alias in aliases if alias}


def visual_cpp_versioned_key(product_key: str) -> str:
    """Return a normalized Visual C++ Redistributable key when a year is named."""
    key = normalize_inventory_product_key_value(product_key)
    match = re.match(r"^(?:microsoft\s+)?visual c\+\+ (.+?) redistributable$", key)
    if not match:
        return ""
    version_part = re.sub(r"\s+", " ", match.group(1)).strip()
    if not re.search(r"\d{4}", version_part):
        return ""
    return f"microsoft visual c++ {version_part} redistributable"


def load_profile_product_terms(base_url: str, timeout: float) -> set[str]:
    """Current source-profile product terms used to keep generated probes grounded."""
    try:
        profiles = latest_profiles(base_url, timeout)
    except Exception as exc:
        print(f"profile product lookup failed: {exc}", file=sys.stderr, flush=True)
        return set()
    sections = by_section(profiles.get("nodeinfo") or {})
    terms: set[str] = set()
    for value in top_values(sections.get("UNINSTALL", {}), "top_products", limit=120):
        terms.update(product_aliases(value))
    for value in top_values(sections.get("VACCINE", {}), "top_products", limit=80):
        terms.update(product_aliases(value))
    return terms


def normalize_profile_value(value: str) -> str:
    text = normalize_question(value)
    text = re.sub(r"(?:\((?:r|tm)\)|™|®)", " ", text)
    text = re.sub(r"[^0-9a-z가-힣.+#]+", " ", text).strip()
    return re.sub(r"\s+", " ", text)


def load_profile_nodeinfo_value_terms(base_url: str, timeout: float) -> set[str]:
    """Current source-profile nodeinfo values used to suppress absent inventory probes."""
    try:
        profiles = latest_profiles(base_url, timeout)
    except Exception as exc:
        print(f"profile nodeinfo value lookup failed: {exc}", file=sys.stderr, flush=True)
        return set()
    sections = by_section(profiles.get("nodeinfo") or {})
    terms: set[str] = set()
    for section_name, label, field in NODEINFO_PROFILE_VALUE_SOURCES:
        label_key = normalize_question(label)
        for value in top_values(sections.get(section_name, {}), field, limit=160):
            normalized = normalize_profile_value(value)
            if normalized:
                terms.add(f"{label_key}:{normalized}")
    for hotfix in top_values(sections.get("UPDATE", {}), "top_hotfixes", limit=160):
        normalized = normalize_question(hotfix)
        if normalized:
            terms.add(f"hotfix:{normalized}")
    for build in top_values(sections.get("OS", {}), "top_builds", limit=160):
        normalized = normalize_question(build)
        if normalized:
            terms.add(f"os-build:{normalized}")
    return terms


def profile_allows_product(product_key: str, profile_product_terms: set[str]) -> bool:
    if not product_key:
        return True
    if not profile_product_terms:
        return False
    key = normalize_inventory_product_key_value(product_key)
    if key in {"low-value-install-product"}:
        return False
    visual_cpp_key = visual_cpp_versioned_key(key)
    if visual_cpp_key:
        # A generic Visual C++ Redistributable alias means the family exists,
        # not that every historical year/version is present in this profile.
        return visual_cpp_key in profile_product_terms
    if key in profile_product_terms:
        return True
    return any((len(term) >= 3 and term in key) or (len(key) >= 3 and key in term) for term in profile_product_terms)


def inventory_product_key_parts(product_key: str) -> set[str]:
    """Split a compound installed-product key into independently grounded terms."""
    key = normalize_inventory_product_key_value(product_key)
    if not key:
        return set()
    parts = {
        normalize_inventory_product_key_value(part)
        for part in re.split(r"\s*(?:및|와|과|and)\s*|[,/]", key)
    }
    parts.discard("")
    return parts or {key}


def low_value_install_question(question: str) -> bool:
    text = normalize_question(question)
    if not any(token in text for token in ["설치", "프로그램", "소프트웨어", "제품", "게시자", "제거", "삭제", "uninstall"]):
        return False
    product_key = inventory_product_key_without_low_value_check(text)
    return profile_blocked_install_product(product_key or text) or any(
        re.search(pattern, text) for pattern in LOW_VALUE_INSTALL_PRODUCT_PATTERNS
    )


def inventory_product_key_without_low_value_check(question: str) -> str:
    text = normalize_question(question)
    if "파일" in text and not any(token in text for token in ["설치", "프로그램", "소프트웨어", "uninstall"]):
        return ""
    if (
        any(token in text for token in ["프린터", "그래픽 카드", "비디오카드", "네트워크 어댑터"])
        and not any(token in text for token in ["프로그램", "소프트웨어", "제품명", "게시자", "제거", "삭제", "uninstall"])
    ):
        return ""
    if not any(token in text for token in ["설치", "프로그램", "소프트웨어", "제품", "게시자", "제거", "삭제", "uninstall"]):
        return ""
    if any(token in text for token in ["회사 전체", "전체 설치", "설치 프로그램", "설치된 프로그램", "소프트웨어 인벤토리"]):
        return ""
    match = re.search(
        r"(.+?)\s*(?:설치|구버전|제품\s*버전|게시자\s*메타데이터|설치\s*항목|소프트웨어\s*인벤토리|설치\s*인벤토리|설치\s*상태|제거\s*명령|삭제\s*명령|uninstall)",
        text,
    )
    if not match:
        match = re.search(
            r"(.+?)\s*(?:게시자\s*)?제품(?:을|를)?\s*(?:버전|게시자|설치|분포|장비)",
            text,
        )
    if not match:
        return ""
    product = normalize_inventory_product_key_value(match.group(1))
    if not product or product in {"프로그램", "제품", "소프트웨어", "설치"}:
        return ""
    return product


def product_key_matches_any(product_key: str, product_keys: set[str]) -> bool:
    key = normalize_inventory_product_key_value(product_key)
    if not key or not product_keys:
        return False
    normalized = {normalize_inventory_product_key_value(value) for value in product_keys}
    normalized.discard("")
    if key in normalized:
        return True
    parts = inventory_product_key_parts(key)
    if parts & normalized:
        return True
    return any(
        len(value) >= 6 and (value in key or key in value)
        for value in normalized
    )


def product_key_allowed_by_profile(product_key: str, profile_product_terms: set[str]) -> bool:
    """Keep product-specific install probes grounded in the current profile."""
    if not product_key:
        return True
    return all(profile_allows_product(part, profile_product_terms) for part in inventory_product_key_parts(product_key))


def nodeinfo_value_allowed_by_profile(value_key: str, profile_nodeinfo_value_terms: set[str]) -> bool:
    """Keep concrete nodeinfo value probes grounded in the current profile."""
    if not value_key:
        return True
    if not profile_nodeinfo_value_terms:
        return False
    label, _, value = value_key.partition(":")
    normalized_key = f"{label}:{normalize_profile_value(value)}" if value else normalize_question(value_key)
    if normalized_key in profile_nodeinfo_value_terms:
        return True
    same_label_terms = [
        term.partition(":")[2]
        for term in profile_nodeinfo_value_terms
        if term.startswith(f"{label}:")
    ]
    value_text = normalized_key.partition(":")[2]
    return any(
        len(term) >= 4
        and len(value_text) >= 4
        and (term in value_text or value_text in term)
        for term in same_label_terms
    )


def intent_fingerprint(question: str) -> str:
    text = normalize_question(question)
    has_os_word = "윈도" in text or "운영체제" in text or bool(re.search(r"(?<![a-z0-9])os(?![a-z0-9])", text))
    kb_match = re.search(r"\bkb\d{6,8}\b", text)
    cpu_model_match = re.search(r"(.+?)\s*cpu\s*모델", text)
    concrete_nodeinfo_value = nodeinfo_value_key(text)
    products = [
        "update for x64-based windows systems",
        "microsoft update health tools",
        "windows pc 상태 검사",
        "windows defender",
        "symantec endpoint protection",
        "ahnlab v3 internet security 9.0",
        "ahnlab v3 lite",
        "google chrome",
        "microsoft edge",
        "microsoft onedrive",
        "orange 1.6",
        "kb5001716",
        "kb5011063",
        "kb5050111",
        "kb5063706",
        "kb5059504",
        "hancom pdf",
        "sindoh",
        "realtek",
        "intel uhd",
        "intel",
        "카카오톡",
        "캡처 도구",
        "nprotect",
        "안랩",
        "ahnlab",
        "v3",
        "알약",
        "inisafe",
        "magicline",
        "touchen",
        "microsoft",
        "office",
        "teams",
        "chrome",
        "edge",
        "firefox",
        "mongodb",
        "adobe",
        "zoom",
        "java",
        ".net",
        "한컴",
        "삼성",
        "samsung",
        "mirage",
        "vmware",
        "surface",
        "hp",
        "lenovo",
        "dell",
    ]
    generic_product_match = re.search(
        r"(.+?)\s*(?:설치|보안 제품|백신|게시자|제품)\s*(?:위치|경로|노드|장비|버전|상태|서명|메타데이터|인벤토리|여부)?",
        text,
    )
    if kb_match:
        vendor = kb_match.group(0)
    elif concrete_nodeinfo_value:
        vendor = concrete_nodeinfo_value[:100]
    elif cpu_model_match:
        vendor = re.sub(r"\s+", " ", cpu_model_match.group(1)).strip()[:80] or "any"
    else:
        vendor = next((item for item in products if item in text), "")
        if not vendor and generic_product_match:
            candidate = re.sub(r"\s+", " ", generic_product_match.group(1)).strip(" -:")
            if candidate and candidate not in {"회사 전체", "전체", "프로그램", "설치 프로그램", "소프트웨어"}:
                vendor = normalize_inventory_product_key_value(candidate)[:80] or candidate[:80]
        elif vendor in {"microsoft", "intel"} and generic_product_match:
            candidate = re.sub(r"\s+", " ", generic_product_match.group(1)).strip(" -:")
            if len(candidate) > len(vendor) + 3:
                vendor = normalize_inventory_product_key_value(candidate)[:80] or candidate[:80]
        vendor = vendor or "any"
    metric = next(
        (
            item
            for item in [
                "pathcounters",
                "filecounters",
                "설치 원본",
                "원본 경로",
                "제거 명령",
                "삭제 명령",
                "설치일",
                "hotfix",
                "kb",
                "ubr",
                "서명",
                "signature",
                "게시자",
                "publisher",
                "위치",
                "경로",
                "path",
                "패치",
                "구버전",
                "버전",
                "상태",
                "cpu",
                "메모리",
                "memory",
                "io",
                "핸들",
                "부하지수",
                "장애",
                "탐지",
                "백신",
                "보안 제품",
                "윈도우",
                "제조사",
                "모델",
                "설치",
            ]
            if item in text
        ),
        "inventory",
    )
    if metric == "inventory" and has_os_word:
        metric = "os"
    grouping = next((item for item in ["프로세스별", "제품별", "제품명별", "회사별", "게시자별", "장비별", "사용자별", "요일별", "시간대별", "노드별", "버전별", "드라이버별", "포트별", "hotfixid별", "ubr별", "빌드별", "서명별", "상태별", "설치일별", "제조사별", "모델별", "경로", "상태", "서명", "설치일", "원본", "제거", "삭제", "기본"] if item in text), "none")
    if grouping == "none" and has_os_word:
        grouping = "os"
    inventory_area = next(
        (
            item
            for item in [
                "hotfix",
                "kb",
                "ubr",
                "백신",
                "보안 제품",
                "프린터",
                "네트워크",
                "어댑터",
                "디스크",
                "비디오",
                "그래픽",
                "에이전트",
                "설치",
                "프로그램",
                "게시자",
                "경로",
                "설치일",
                "게시자",
                "원본",
                "위치",
                "제거",
                "삭제",
                "pathcounters",
                "filecounters",
                "윈도우",
            ]
            if item in text
        ),
        "general",
    )
    if inventory_area == "general" and has_os_word:
        inventory_area = "os"
    period = next((item for item in ["오늘", "최근 3일", "최근 7일", "최근 30일", "최근 1년"] if item in text), "none")
    if any(item in text for item in ["분포", "비중", "현황"]):
        action = "distribution"
    elif any(item in text for item in ["평균", "계산"]):
        action = "average"
    elif any(item in text for item in ["많은", "높은", "순서"]):
        action = "rank"
    elif any(item in text for item in ["목록", "찾아", "보여"]):
        action = "list"
    else:
        action = "generic"
    return "|".join([inventory_area, vendor, metric, grouping, period, action])


def inventory_product_key(question: str) -> str:
    """Return a coarse installed-product key for suppressing stale profile variants.

    Autoresearch profile rows can create many questions from one product name
    by changing only grouping fields. If a product-specific UNINSTALL probe is
    already confirmed zero-result, block the product family instead of allowing
    adjacent variants to churn through the stream.
    """
    text = normalize_question(question)
    if low_value_install_question(text):
        return "low-value-install-product"
    return inventory_product_key_without_low_value_check(text)


def nodeinfo_value_key(question: str) -> str:
    """Return a profiled nodeinfo value key for suppressing stale value churn."""
    text = normalize_question(question)
    for canonical_label, aliases in NODEINFO_VALUE_LABEL_ALIASES:
        label_key = normalize_question(canonical_label)
        for alias in aliases:
            alias_key = normalize_question(alias)
            if alias_key not in text:
                continue
            value = text.split(alias_key, 1)[0]
            value = re.sub(r"\s+(?:및|와|과)\s+.+$", "", value)
            value = re.sub(r"\s+", " ", value).strip(" -:")
            if value and value not in {"전체", "모든", "기본", "노드별", "장비별", "회사 전체"} and len(value) >= 2:
                return f"{label_key}:{value[:100]}"
    hotfix = re.search(r"\bkb\d{6,8}\b", text)
    if hotfix and any(token in text for token in ["hotfix", "패치", "설치 장비", "적용 장비"]):
        return f"hotfix:{hotfix.group(0)}"
    os_build = re.search(r"os\s*빌드\s*(\d{4,6})", text)
    if os_build:
        return f"os-build:{os_build.group(1)}"
    return ""


def filelist_value_key(question: str) -> str:
    """Return a file inventory value key for suppressing stale no-data variants.

    File inventory no-data reviews usually prove that the extracted value is
    stale in the current filelist profile, not merely that one phrasing failed.
    Use one value-level key across signer/company/product/file variants so the
    stream does not keep probing the same absent value through nearby questions.
    """
    text = normalize_question(question)
    if "파일" not in text:
        return ""
    for label, pattern in FILELIST_VALUE_PATTERNS:
        match = pattern.search(text)
        if not match:
            continue
        if label == "unsigned-file":
            return label
        value = match.group(1)
        value = re.sub(r"^(?:파일\s*인벤토리\s*(?:에서|의|를|을)?|file\s*inventory(?:\s+from|\s+in)?)\s*", "", value, flags=re.IGNORECASE)
        value = re.sub(r"\s+(?:제품\s*버전|파일\s*버전|설치\s*경로|경로|분포|기준|장비별|제품명|회사명|서명).*$", "", value, flags=re.IGNORECASE)
        value = re.sub(r"\s+", " ", value).strip(" ,_-:")
        if filelist_field_list_only(value):
            return ""
        if value and value not in {"최근", "오늘", "새로", "신규", "파일", "인벤토리"}:
            return f"value:{value[:100]}"
    return ""


def filelist_field_list_only(value: str) -> bool:
    """True when a captured filelist "value" is only requested metadata fields."""
    tokens = [token for token in re.split(r"\s*(?:와|과|및|,|/|\+|\s)\s*", normalize_question(value)) if token]
    if not tokens:
        return False
    ignored = {"조합별", "기준", "분포", "목록", "파일", "수", "보여줘", "집계"}
    meaningful = [token for token in tokens if token not in ignored]
    return bool(meaningful) and all(token in FILELIST_FIELD_TOKENS for token in meaningful)


def load_seen_questions(base_url: str, timeout: float, limit: int = SEEN_LOOKUP_LIMIT) -> set[str]:
    try:
        data = request_json("GET", f"{base_url}/api/llm-query/recent?limit={limit}", timeout=timeout)
    except Exception as exc:
        print(f"seen question lookup failed: {exc}", file=sys.stderr, flush=True)
        return set()
    return {
        normalize_question(row.get("question") or "")
        for row in data.get("rows", [])
        if row.get("question")
    }


def load_claimed_questions(base_url: str, timeout: float, limit: int = SEEN_LOOKUP_LIMIT) -> set[str]:
    try:
        data = request_json("GET", f"{base_url}/api/autotest/seen-questions?limit={limit}", timeout=timeout)
    except Exception as exc:
        print(f"claimed question lookup failed: {exc}", file=sys.stderr, flush=True)
        return set()
    return {
        normalize_question(row.get("question") or "")
        for row in data.get("rows", [])
        if row.get("question")
    }


def load_seen_intents(base_url: str, timeout: float, limit: int = SEEN_LOOKUP_LIMIT) -> set[str]:
    try:
        data = request_json("GET", f"{base_url}/api/llm-query/recent?limit={limit}", timeout=timeout)
    except Exception as exc:
        print(f"seen intent lookup failed: {exc}", file=sys.stderr, flush=True)
        return set()
    return {
        intent_fingerprint(row.get("question") or "")
        for row in data.get("rows", [])
        if row.get("question")
    }


def load_blocked_intents(base_url: str, timeout: float, limit: int = 2000) -> set[str]:
    try:
        data = request_json("GET", f"{base_url}/api/llm-query/recent?limit={limit}", timeout=timeout)
    except Exception as exc:
        print(f"blocked intent lookup failed: {exc}", file=sys.stderr, flush=True)
        return set()
    blocked: set[str] = set()
    for row in data.get("rows", []):
        quality_status = row.get("quality_status")
        failure_reason = row.get("failure_reason")
        errors = [str(error) for error in (row.get("errors") or [])]
        candidate_fix = row.get("candidate_fix") or {}
        blocks_repeat = (
            quality_status == "needs_review"
            and failure_reason == "zero_result"
            and candidate_fix.get("type") == "needs_human_review"
        ) or (
            quality_status in {"failed", None}
            and (
                failure_reason == "runtime_error"
                or any("field not allowed" in error for error in errors)
            )
        )
        if not blocks_repeat:
            continue
        question = row.get("question") or ""
        if question:
            blocked.add(intent_fingerprint(question))
    return blocked


def load_blocked_product_keys(base_url: str, timeout: float, limit: int = 2000) -> set[str]:
    try:
        data = request_json("GET", f"{base_url}/api/llm-query/recent?limit={limit}", timeout=timeout)
    except Exception as exc:
        print(f"blocked product lookup failed: {exc}", file=sys.stderr, flush=True)
        return set()
    blocked: set[str] = set()
    for row in data.get("rows", []):
        candidate_fix = row.get("candidate_fix") or {}
        if (
            row.get("quality_status") == "needs_review"
            and row.get("failure_reason") == "zero_result"
            and candidate_fix.get("type") == "needs_human_review"
            and "no probe found data" in str(candidate_fix.get("reason") or "")
            and (
                row.get("collection") == "nodeinfo"
                or ((row.get("plan") or {}).get("collection") == "nodeinfo")
                or inventory_product_key(row.get("question") or "")
            )
        ):
            product_key = inventory_product_key(row.get("question") or "")
            if product_key:
                blocked.add(product_key)
    return blocked


def load_blocked_nodeinfo_value_keys(base_url: str, timeout: float, limit: int = 2000) -> set[str]:
    try:
        data = request_json("GET", f"{base_url}/api/llm-query/recent?limit={limit}", timeout=timeout)
    except Exception as exc:
        print(f"blocked nodeinfo value lookup failed: {exc}", file=sys.stderr, flush=True)
        return set()
    blocked: set[str] = set()
    for row in data.get("rows", []):
        candidate_fix = row.get("candidate_fix") or {}
        if (
            row.get("quality_status") == "needs_review"
            and row.get("failure_reason") == "zero_result"
            and candidate_fix.get("type") == "needs_human_review"
            and "no probe found data" in str(candidate_fix.get("reason") or "")
            and (
                row.get("collection") == "nodeinfo"
                or ((row.get("plan") or {}).get("collection") == "nodeinfo")
            )
        ):
            value_key = nodeinfo_value_key(row.get("question") or "")
            if value_key:
                blocked.add(value_key)
    return blocked


def load_blocked_filelist_value_keys(base_url: str, timeout: float, limit: int = 2000) -> set[str]:
    try:
        data = request_json("GET", f"{base_url}/api/llm-query/recent?limit={limit}", timeout=timeout)
    except Exception as exc:
        print(f"blocked filelist value lookup failed: {exc}", file=sys.stderr, flush=True)
        return set()
    blocked: set[str] = set()
    for row in data.get("rows", []):
        candidate_fix = row.get("candidate_fix") or {}
        if (
            row.get("quality_status") == "needs_review"
            and row.get("failure_reason") == "zero_result"
            and candidate_fix.get("type") == "needs_human_review"
            and "no probe found data" in str(candidate_fix.get("reason") or "")
            and (
                row.get("collection") == "filelist"
                or ((row.get("plan") or {}).get("collection") == "filelist")
            )
        ):
            value_key = filelist_value_key(row.get("question") or "")
            if value_key:
                blocked.add(value_key)
    return blocked


def unique_scenarios(scenarios: list[Scenario], seen: set[str], seen_intents: set[str]) -> list[Scenario]:
    selected: list[Scenario] = []
    for scenario in scenarios:
        key = normalize_question(scenario.question)
        intent_key = intent_fingerprint(scenario.question)
        if not key or key in seen or intent_key in seen_intents:
            continue
        seen.add(key)
        seen_intents.add(intent_key)
        selected.append(scenario)
    return selected


def uses_question_dedupe(source: str) -> bool:
    return str(source).startswith(QUESTION_DEDUPE_SOURCE_PREFIXES)


def claim_question(base_url: str, scenario: Scenario, timeout: float) -> bool:
    dedupe_mode = "question" if uses_question_dedupe(str(scenario.source)) else "intent"
    try:
        data = request_json(
            "POST",
            f"{base_url}/api/autotest/claim-question",
            {
                "question": scenario.question,
                "intent_key": intent_fingerprint(scenario.question),
                "source": scenario.source,
                "category": scenario.category,
                "dedupe_mode": dedupe_mode,
            },
            timeout=min(timeout, 10.0),
        )
    except Exception as exc:
        print(f"question claim failed; using local seen set only: {exc}", file=sys.stderr, flush=True)
        return True
    return bool(data.get("claimed"))


def scenario_pool(
    base_url: str,
    timeout: float,
    seed: int,
    generated: int,
    pool_size: int,
    seen: set[str],
    seen_intents: set[str],
    blocked_intents: set[str],
    blocked_product_keys: set[str],
    blocked_nodeinfo_value_keys: set[str],
    blocked_filelist_value_keys: set[str],
    profile_product_terms: set[str],
    profile_nodeinfo_value_terms: set[str],
    include_base: bool,
) -> list[Scenario]:
    raw: list[Scenario] = []
    try:
        raw.extend(profile_scenarios(base_url, min(timeout, 30.0), seed, max(pool_size * 20, generated * 5, 500)))
    except Exception as exc:
        print(f"autoresearch profile scenario generation failed: {exc}", file=sys.stderr, flush=True)
    if include_base:
        raw.extend(base_scenarios())
    raw.extend(generated_scenarios(seed, max(generated * 5, pool_size * 20, 500)))
    rng = random.Random(seed)
    rng.shuffle(raw)
    selected: list[Scenario] = []
    profiled_product_counts: dict[str, int] = {}
    profiled_nodeinfo_value_counts: dict[str, int] = {}
    for scenario in raw:
        key = normalize_question(scenario.question)
        intent_key = intent_fingerprint(scenario.question)
        if not key or key in seen:
            continue
        if low_value_install_question(scenario.question):
            continue
        # Profile-mined questions are already grounded in source data and are
        # claimed by exact question in the API. Keep the local filter aligned
        # so a broad product/hotfix/software pool is not exhausted by one
        # coarse intent bucket. However, if the same specific intent recently
        # produced a zero-result needs_review case, suppress it until new source
        # data/profile evidence changes the stream.
        exact_question_dedupe = uses_question_dedupe(str(scenario.source))
        if exact_question_dedupe and intent_key in blocked_intents:
            continue
        product_key = inventory_product_key(scenario.question)
        if product_key_matches_any(product_key, blocked_product_keys):
            continue
        value_key = nodeinfo_value_key(scenario.question)
        # Stale profiled inventory values can also enter through controlled
        # variants, not only autoresearch rows. Suppress any exact nodeinfo
        # value that recently produced a no-data needs_review result.
        if value_key in blocked_nodeinfo_value_keys:
            continue
        if not nodeinfo_value_allowed_by_profile(value_key, profile_nodeinfo_value_terms):
            continue
        if exact_question_dedupe and value_key:
            value_count = profiled_nodeinfo_value_counts.get(value_key, 0)
            if value_count >= MAX_PROFILED_NODEINFO_VALUE_VARIANTS_PER_POOL:
                continue
            profiled_nodeinfo_value_counts[value_key] = value_count + 1
        file_value_key = filelist_value_key(scenario.question)
        if file_value_key in blocked_filelist_value_keys:
            continue
        if not product_key_allowed_by_profile(product_key, profile_product_terms):
            continue
        if exact_question_dedupe and product_key:
            product_count = profiled_product_counts.get(product_key, 0)
            if product_count >= MAX_PROFILED_INSTALL_PRODUCT_VARIANTS_PER_POOL:
                continue
            profiled_product_counts[product_key] = product_count + 1
        if not exact_question_dedupe and intent_key in seen_intents:
            continue
        seen.add(key)
        if not exact_question_dedupe:
            seen_intents.add(intent_key)
        selected.append(scenario)
    return selected[:pool_size] if pool_size > 0 else selected


def wait_for_server(base_url: str, timeout: float) -> None:
    while True:
        try:
            data = request_json("GET", f"{base_url}/health", timeout=timeout)
            if data.get("ok"):
                return
        except Exception as exc:
            print(f"q2 health wait: {exc}", file=sys.stderr, flush=True)
        time.sleep(2)


def write_stream_status(
    base_url: str,
    run_id: str,
    rows: list[dict[str, Any]],
    current: str = "",
    reference_time: str = "",
    total: int = 0,
) -> None:
    status = {
        "run_id": run_id,
        "state": "streaming",
        "updated_at": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
        "total": total,
        "completed": len(rows),
        "current_question": current,
        "summary": summarize(rows),
        "recent": rows[-20:],
        "reference_time": reference_time,
        "mode": "autoresearch",
    }
    try:
        request_json("POST", f"{base_url}/api/autotest/status", status, timeout=10.0)
    except Exception as exc:
        print(f"autotest stream status update failed: {exc}", file=sys.stderr, flush=True)


def write_autoresearch_result(base_url: str, run_id: str, row: dict[str, Any], reference_time: str) -> None:
    try:
        request_json(
            "POST",
            f"{base_url}/api/autoresearch/results",
            {"run_id": run_id, "row": row, "reference_time": reference_time},
            timeout=10.0,
        )
    except Exception as exc:
        print(f"autoresearch result db write failed: {exc}", file=sys.stderr, flush=True)


def main() -> int:
    parser = argparse.ArgumentParser(description="q2 continuous autonomous autoresearch stream")
    parser.add_argument("--server", default=DEFAULT_SERVER)
    parser.add_argument("--timeout", type=float, default=220.0)
    parser.add_argument("--rate", type=float, default=1.0, help="questions per second")
    parser.add_argument("--pool-size", type=int, default=100)
    parser.add_argument("--generated", type=int, default=59)
    parser.add_argument("--seed", type=int, default=3184)
    parser.add_argument("--reference-time", default="dataset", help="dataset, empty, or ISO timestamp")
    parser.add_argument("--output", default="llm/q2/output/autotest_stream_latest.json")
    parser.add_argument("--max-events", type=int, default=0, help="0 means run forever")
    args = parser.parse_args()

    base_url = args.server.rstrip("/")
    wait_for_server(base_url, args.timeout)

    if args.reference_time == "dataset":
        try:
            reference_time = dataset_reference_time(base_url, args.timeout) or ""
        except Exception as exc:
            print(f"dataset reference time lookup failed: {exc}", file=sys.stderr, flush=True)
            reference_time = ""
    else:
        reference_time = args.reference_time or ""
    if reference_time:
        print(f"reference_time={reference_time}", flush=True)

    seen_questions = load_seen_questions(base_url, min(args.timeout, 30.0))
    seen_questions.update(load_claimed_questions(base_url, min(args.timeout, 30.0)))
    seen_intents = load_seen_intents(base_url, min(args.timeout, 30.0))
    blocked_intents = load_blocked_intents(base_url, min(args.timeout, 30.0))
    blocked_product_keys = load_blocked_product_keys(base_url, min(args.timeout, 30.0))
    blocked_nodeinfo_value_keys = load_blocked_nodeinfo_value_keys(base_url, min(args.timeout, 30.0))
    blocked_filelist_value_keys = load_blocked_filelist_value_keys(base_url, min(args.timeout, 30.0))
    profile_product_terms = load_profile_product_terms(base_url, min(args.timeout, 30.0))
    profile_nodeinfo_value_terms = load_profile_nodeinfo_value_terms(base_url, min(args.timeout, 30.0))
    scenarios = scenario_pool(
        base_url,
        args.timeout,
        args.seed,
        args.generated,
        args.pool_size,
        seen_questions,
        seen_intents,
        blocked_intents,
        blocked_product_keys,
        blocked_nodeinfo_value_keys,
        blocked_filelist_value_keys,
        profile_product_terms,
        profile_nodeinfo_value_terms,
        include_base=True,
    )
    if not scenarios:
        print("no fresh scenarios to run; waiting without repeating", file=sys.stderr, flush=True)

    run_id = uuid.uuid4().hex[:12]
    rows: list[dict[str, Any]] = []
    interval = 1.0 / max(args.rate, 0.001)
    output_path = Path(args.output)
    output_path.parent.mkdir(parents=True, exist_ok=True)
    write_stream_status(base_url, run_id, rows, reference_time=reference_time, total=len(rows) + len(scenarios))

    index = 0
    seed = args.seed
    include_base = False
    while True:
        if args.max_events and index >= args.max_events:
            break
        if not scenarios:
            seed += 1
            blocked_intents = load_blocked_intents(base_url, min(args.timeout, 30.0))
            blocked_product_keys = load_blocked_product_keys(base_url, min(args.timeout, 30.0))
            blocked_nodeinfo_value_keys = load_blocked_nodeinfo_value_keys(base_url, min(args.timeout, 30.0))
            blocked_filelist_value_keys = load_blocked_filelist_value_keys(base_url, min(args.timeout, 30.0))
            profile_product_terms = load_profile_product_terms(base_url, min(args.timeout, 30.0))
            profile_nodeinfo_value_terms = load_profile_nodeinfo_value_terms(base_url, min(args.timeout, 30.0))
            scenarios = scenario_pool(
                base_url,
                args.timeout,
                seed,
                args.generated,
                args.pool_size,
                seen_questions,
                seen_intents,
                blocked_intents,
                blocked_product_keys,
                blocked_nodeinfo_value_keys,
                blocked_filelist_value_keys,
                profile_product_terms,
                profile_nodeinfo_value_terms,
                include_base=include_base,
            )
            include_base = False
            if not scenarios:
                if args.max_events:
                    write_stream_status(
                        base_url,
                        run_id,
                        rows,
                        current="autoresearch profile question pool exhausted; stopping bounded run",
                        reference_time=reference_time,
                        total=len(rows),
                    )
                    break
                write_stream_status(
                    base_url,
                    run_id,
                    rows,
                    current="autoresearch profile question pool exhausted; waiting",
                    reference_time=reference_time,
                    total=len(rows),
                )
                time.sleep(20)
                continue
        scenario = scenarios.pop(0)
        next_index = index + 1
        if low_value_install_question(scenario.question):
            print(f"{next_index:06d} skipped_low_value {'-':10s} {'0.0':>9s}ms {scenario.question}", flush=True)
            continue
        intent_key = intent_fingerprint(scenario.question)
        if uses_question_dedupe(str(scenario.source)) and intent_key in blocked_intents:
            print(f"{next_index:06d} skipped_blocked {'-':10s} {'0.0':>9s}ms {scenario.question}", flush=True)
            continue
        product_key = inventory_product_key(scenario.question)
        if product_key_matches_any(product_key, blocked_product_keys):
            print(f"{next_index:06d} skipped_blocked_product {'-':10s} {'0.0':>9s}ms {scenario.question}", flush=True)
            continue
        if not product_key_allowed_by_profile(product_key, profile_product_terms):
            print(f"{next_index:06d} skipped_unprofiled_product {'-':10s} {'0.0':>9s}ms {scenario.question}", flush=True)
            continue
        value_key = nodeinfo_value_key(scenario.question)
        if value_key in blocked_nodeinfo_value_keys:
            print(f"{next_index:06d} skipped_blocked_value {'-':10s} {'0.0':>9s}ms {scenario.question}", flush=True)
            continue
        if not nodeinfo_value_allowed_by_profile(value_key, profile_nodeinfo_value_terms):
            print(f"{next_index:06d} skipped_unprofiled_value {'-':10s} {'0.0':>9s}ms {scenario.question}", flush=True)
            continue
        file_value_key = filelist_value_key(scenario.question)
        if file_value_key in blocked_filelist_value_keys:
            print(f"{next_index:06d} skipped_blocked_file_value {'-':10s} {'0.0':>9s}ms {scenario.question}", flush=True)
            continue
        if not claim_question(base_url, scenario, args.timeout):
            print(f"{next_index:06d} skipped_repeat {'-':10s} {'0.0':>9s}ms {scenario.question}", flush=True)
            continue
        index = next_index
        started = time.monotonic()
        write_stream_status(base_url, run_id, rows, current=scenario.question, reference_time=reference_time, total=len(rows) + len(scenarios) + 1)
        row = run_scenario(base_url, scenario, timeout=args.timeout, reference_time=reference_time or None)
        row["stream_index"] = index
        rows.append(row)
        candidate_fix = row.get("candidate_fix") or {}
        if (
            uses_question_dedupe(str(scenario.source))
            and row.get("quality_status") == "needs_review"
            and row.get("failure_reason") == "zero_result"
            and candidate_fix.get("type") == "needs_human_review"
            and "no probe found data" in str(candidate_fix.get("reason") or "")
        ):
            blocked_intents.add(intent_key)
            product_key = inventory_product_key(scenario.question)
            if product_key:
                blocked_product_keys.add(product_key)
            value_key = nodeinfo_value_key(scenario.question)
            if value_key:
                blocked_nodeinfo_value_keys.add(value_key)
            file_value_key = filelist_value_key(scenario.question)
            if file_value_key:
                blocked_filelist_value_keys.add(file_value_key)
        if len(rows) > 1000:
            rows = rows[-1000:]
        status = row.get("quality_status") or ("ok" if row.get("ok") else "failed")
        if row.get("expected_match") is False:
            status = "mismatch"
        write_autoresearch_result(base_url, run_id, row, reference_time)
        print(
            f"{index:06d} {status:12s} {str(row.get('collection') or '-'):10s} "
            f"{row.get('elapsed_ms'):9.1f}ms {scenario.question}",
            flush=True,
        )
        write_stream_status(base_url, run_id, rows, reference_time=reference_time, total=len(rows) + len(scenarios))
        output_path.write_text(
            json.dumps(
                {
                    "run_id": run_id,
                    "updated_at": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
                    "reference_time": reference_time,
                    "summary": summarize(rows),
                    "recent": rows[-100:],
                },
                ensure_ascii=False,
                indent=2,
            ),
            encoding="utf-8",
        )
        elapsed = time.monotonic() - started
        if elapsed < interval:
            time.sleep(interval - elapsed)

    write_stream_status(base_url, run_id, rows, reference_time=reference_time, total=len(rows) + len(scenarios))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
