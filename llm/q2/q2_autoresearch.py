#!/usr/bin/env python3
from __future__ import annotations

import random
import re
from typing import Any

from q2_autotest import Scenario, request_json


def clean_value(value: Any, max_len: int = 90) -> str:
    text = re.sub(r"\s+", " ", str(value or "").strip())
    if not text or text.lower() in {"none", "null", "unknown", "n/a", "-"}:
        return ""
    return text[:max_len]


def top_values(section: dict[str, Any], key: str, limit: int = 8) -> list[str]:
    rows = section.get(key) or []
    values: list[str] = []
    for row in rows[:limit]:
        value = clean_value(row.get("value") if isinstance(row, dict) else row)
        if value and value not in values:
            values.append(value)
    return values


def top_counted_values(section: dict[str, Any], key: str, limit: int = 8, min_count: int = 1) -> list[str]:
    return [row["value"] for row in top_counted_entries(section, key, limit=limit, min_count=min_count)]


def top_counted_entries(section: dict[str, Any], key: str, limit: int = 8, min_count: int = 1) -> list[dict[str, Any]]:
    rows = section.get(key) or []
    values: list[dict[str, Any]] = []
    for row in rows[:limit]:
        value = clean_value(row.get("value") if isinstance(row, dict) else row)
        count = row.get("count") if isinstance(row, dict) else None
        if count is not None:
            try:
                count = int(count)
                if count < min_count:
                    continue
            except (TypeError, ValueError):
                continue
        if value and value not in {item["value"] for item in values}:
            values.append({"value": value, "count": int(count or 0)})
    return values


def has_profile_empty_value(section: dict[str, Any], key: str) -> bool:
    """True when a profiled top-value bucket shows missing metadata."""
    for row in section.get(key) or []:
        raw_value = row.get("value") if isinstance(row, dict) else row
        if raw_value is None or str(raw_value).strip().lower() in {"", "none", "null", "n/a", "-"}:
            try:
                return int(row.get("count") if isinstance(row, dict) else 0) > 0
            except (TypeError, ValueError):
                return True
    return False


def version_values(section: dict[str, Any], key: str, limit: int = 8) -> list[str]:
    return [
        value
        for value in top_values(section, key, limit=limit * 3)
        if re.fullmatch(r"\d+(?:\.\d+){1,4}", value)
    ][:limit]


def value_pairs(values: list[str], limit: int = 6) -> list[tuple[str, str]]:
    pairs: list[tuple[str, str]] = []
    for index in range(0, min(len(values) - 1, limit * 2), 2):
        left = values[index]
        right = values[index + 1]
        if left != right:
            pairs.append((left, right))
    return pairs[:limit]


def latest_profiles(base_url: str, timeout: float) -> dict[str, dict[str, Any]]:
    data = request_json("GET", f"{base_url}/api/data-profile?limit=100", timeout=timeout)
    profiles: dict[str, dict[str, Any]] = {}
    for row in data.get("profiles", []):
        collection = row.get("collection")
        if collection and collection not in profiles:
            profiles[collection] = row.get("profile") or {}
    return profiles


def by_section(profile: dict[str, Any]) -> dict[str, dict[str, Any]]:
    return {
        str(section.get("name")): section
        for section in profile.get("sections", [])
        if section.get("name")
    }


def add(
    scenarios: list[Scenario],
    category: str,
    question: str,
    expected: tuple[str, ...] = ("nodeinfo",),
    *,
    contextual: bool = True,
) -> None:
    text = clean_value(question, max_len=180)
    if not text:
        return
    variants = [text]
    contextual_text = clean_value(f"{text} 인벤토리 기준", max_len=180) if contextual else ""
    if contextual_text and contextual_text != text:
        variants.append(contextual_text)
    for variant in variants:
        scenarios.append(
            Scenario(
                category=category,
                question=variant,
                source="autoresearch:data-profile",
                expected_collections=expected,
            )
        )


COMMON_ENDPOINT_PRODUCT_TERMS = (
    "nprotect",
    "v3",
    "ahnlab",
    "안랩",
    "알약",
    "inisafe",
    "magicline",
    "touchen",
    "office",
    "teams",
    "chrome",
    "edge",
    "firefox",
    "adobe",
    "zoom",
    "java",
    ".net",
    "한컴",
    "hancom",
    "windows defender",
)

MIN_PROFILED_INSTALL_PRODUCT_NODES = 2
MIN_FULL_INSTALL_PRODUCT_NODES = 10

LOW_VALUE_INSTALL_PRODUCT_TERMS = (
    "wdk",
    "adk",
    "windows adk",
    "windows driver kit",
    "windows driver development kit",
    "driver kit",
    "windows kits",
    "software development kit",
    "developer kit",
    "windows sdk",
    "sdk",
    "assessment and deployment kit",
    "deployment kit",
    "debugging tools for windows",
    "windows debugging tools",
)

SOFTWARE_STATE_VARIANTS = (
    "{product} 설치 노드의 OS 빌드 분포를 보여줘",
    "{product} 설치 노드의 사용자와 장비 상태를 보여줘",
    "{product} 설치 항목을 설치 위치 기준으로 보여줘",
    "{product} 설치 항목을 게시자와 설치일 기준으로 점검해줘",
)

ENDPOINT_PRODUCT_STATE_VARIANTS = (
    "{product} 설치 여부와 버전을 노드별로 보여줘",
    "{product} 설치 노드의 패치 준비 상태를 OS 빌드 기준으로 보여줘",
    "{product} 설치 상태를 사용자와 장비명 기준으로 보여줘",
)

MAX_PROFILED_SOFTWARE_VARIANTS = 4
MAX_ENDPOINT_SOFTWARE_VARIANTS = 8
MIN_PROFILED_HARDWARE_VALUE_NODES = 2


def install_question_product(value: str) -> str:
    """Return a stable product family term for generated UNINSTALL questions."""
    product = clean_value(value)
    if not product:
        return ""
    if re.search(r"\bvisual\s+c\+\+.*\bredistributable\b", product, flags=re.IGNORECASE):
        product = re.sub(r"\s+(?:x86|x64|32-bit|64-bit)(?=\s+redistributable\b)", "", product, flags=re.IGNORECASE)
        product = re.sub(r"\s+-\s+(?:x86|x64|32-bit|64-bit)\b.*$", "", product, flags=re.IGNORECASE)
        product = re.sub(r"\s+(?:x86|x64|32-bit|64-bit)\b.*$", "", product, flags=re.IGNORECASE)
        product = re.sub(r"\s+\d+(?:[\.,]\d+){1,5}\s*$", "", product)
        product = re.sub(r"\s*\((?:x86|x64|32-bit|64-bit)\)\s*-?\s*$", "", product, flags=re.IGNORECASE)
    product = re.sub(r"\s+-\s+windows\s+\d+(?:[\.,]\d+){1,5}\s*$", "", product, flags=re.IGNORECASE)
    product = re.sub(r"\s+-\s+\d+(?:[\.,]\d+){1,5}\s*$", "", product)
    product = re.sub(r"\s+(?:version|ver\.?|v)\s*\d+(?:[\.,]\d+){1,5}\s*$", "", product, flags=re.IGNORECASE)
    product = re.sub(r"\s+\d+(?:[\.,]\d+){1,5}\s*$", "", product)
    product = re.sub(r"\s*\((?:x86|x64|32-bit|64-bit)\)\s*-?\s*$", "", product, flags=re.IGNORECASE)
    product = product.strip(" -_")
    return product if len(product) >= 3 else clean_value(value)


def install_product_family_key(value: str) -> str:
    """Stable key used to avoid repeating x86/x64/version product variants."""
    product = install_question_product(value)
    product = re.sub(r"\s*\((?:x86|x64|32-bit|64-bit)\)\s*", " ", product, flags=re.IGNORECASE)
    product = re.sub(r"\s+-\s*(?:x86|x64|32-bit|64-bit)\b.*$", " ", product, flags=re.IGNORECASE)
    product = re.sub(r"\b\d+(?:[\.,]\d+){1,5}\b", " ", product)
    product = re.sub(r"\s+", " ", product).strip(" -_").lower()
    return product[:120]


def endpoint_product(value: str) -> bool:
    text = value.lower()
    return any(term in text for term in COMMON_ENDPOINT_PRODUCT_TERMS)


def low_value_install_product(value: str) -> bool:
    text = value.lower()
    for term in LOW_VALUE_INSTALL_PRODUCT_TERMS:
        if term in {"wdk", "adk"}:
            if re.search(rf"(?<![a-z0-9]){term}(?![a-z0-9])", text):
                return True
        elif term in text:
            return True
    return False


def blocked_install_product(value: str) -> bool:
    """Suppress installer families that repeatedly produce absent-product probes."""
    product = install_question_product(value)
    return low_value_install_product(value) or low_value_install_product(product)


def nodeinfo_profile_scenarios(profile: dict[str, Any]) -> list[Scenario]:
    sections = by_section(profile)
    scenarios: list[Scenario] = []

    uninstall = sections.get("UNINSTALL", {})
    install_location_products = set(top_values(uninstall, "top_install_location_products", limit=80))
    path_products = set(top_values(uninstall, "top_path_products", limit=80))
    install_path_products = install_location_products | path_products
    install_source_products = set(top_values(uninstall, "top_install_source_products", limit=80))
    uninstall_string_products = set(top_values(uninstall, "top_uninstall_string_products", limit=80))
    install_location_publishers = set(top_values(uninstall, "top_install_location_publishers", limit=80))
    path_publishers = set(top_values(uninstall, "top_path_publishers", limit=80))
    install_path_publishers = install_location_publishers | path_publishers
    seen_install_product_keys: set[str] = set()
    for product_row in top_counted_entries(
        uninstall,
        "top_products",
        limit=60,
        min_count=MIN_PROFILED_INSTALL_PRODUCT_NODES,
    ):
        raw_product = product_row["value"]
        product_count = int(product_row.get("count") or 0)
        if blocked_install_product(raw_product):
            continue
        product = install_question_product(raw_product)
        if not product:
            continue
        product_key = install_product_family_key(raw_product)
        if product_key in seen_install_product_keys:
            continue
        seen_install_product_keys.add(product_key)
        has_install_path = raw_product in install_path_products
        full_coverage = product_count >= MIN_FULL_INSTALL_PRODUCT_NODES or endpoint_product(raw_product)
        variant_limit = MAX_ENDPOINT_SOFTWARE_VARIANTS if endpoint_product(raw_product) else MAX_PROFILED_SOFTWARE_VARIANTS
        if not full_coverage:
            variant_limit = 2
        variant_count = 0

        def add_product_variant(category: str, question: str) -> None:
            nonlocal variant_count
            if variant_count >= variant_limit:
                return
            add(scenarios, category, question, contextual=False)
            variant_count += 1

        add_product_variant("autoresearch-software", f"{product} 설치 노드를 사용자별로 보여줘")
        add_product_variant("autoresearch-software", f"{product} 설치 버전별 장비 수를 보여줘")
        if not full_coverage:
            continue
        add_product_variant("autoresearch-software", f"{product} 설치일과 버전을 장비별로 보여줘")
        add_product_variant("autoresearch-software", f"{product} 제품 버전과 게시자 분포를 보여줘")
        if raw_product in install_source_products:
            add_product_variant("autoresearch-software", f"{product} 설치 원본 경로를 장비별로 보여줘")
        if raw_product in uninstall_string_products:
            add_product_variant("autoresearch-software", f"{product} 제거 명령 문자열을 버전별로 보여줘")
        add_product_variant("autoresearch-software", f"{product} 설치 항목의 제품명 버전 게시자를 장비별로 보여줘")
        if has_install_path:
            add_product_variant("autoresearch-software", f"{product} 설치 경로와 게시자를 장비별로 보여줘")
        for template in SOFTWARE_STATE_VARIANTS:
            add_product_variant("autoresearch-software-state", template.format(product=product))
        if endpoint_product(raw_product):
            add_product_variant("autoresearch-endpoint-product", f"{product} 설치 여부를 노드별로 점검해줘")
            add_product_variant("autoresearch-endpoint-product", f"{product} 구버전 설치 장비를 찾아줘")
            add_product_variant("autoresearch-endpoint-product", f"{product} 설치 게시자와 버전을 사용자별로 보여줘")
            for template in ENDPOINT_PRODUCT_STATE_VARIANTS:
                add_product_variant("autoresearch-endpoint-product", template.format(product=product))
    for publisher in top_values(uninstall, "top_publishers", limit=50):
        add(scenarios, "autoresearch-software", f"{publisher} 게시자 프로그램 설치 장비 수를 보여줘")
        add(scenarios, "autoresearch-software", f"{publisher} 게시자 제품을 버전별로 보여줘")
        add(scenarios, "autoresearch-software", f"{publisher} 게시자 프로그램을 사용자별로 보여줘")
        if publisher in install_path_publishers:
            add(scenarios, "autoresearch-software", f"{publisher} 게시자 프로그램 설치 경로를 장비별로 보여줘")
        add(scenarios, "autoresearch-software", f"{publisher} 게시자 프로그램 제품명별 장비 수를 보여줘")
        add(scenarios, "autoresearch-publisher", f"{publisher} 게시자 메타데이터가 있는 설치 항목을 장비별로 보여줘")

    install_products = [install_question_product(value) for value in top_values(uninstall, "top_products", limit=40)]
    install_products = [value for value in install_products if value and not blocked_install_product(value)]
    for left, right in value_pairs(list(dict.fromkeys(install_products)), limit=10):
        add(scenarios, "autoresearch-software-compare", f"{left} 및 {right} 설치 버전 분포를 비교해줘", ("nodeinfo",), contextual=False)
        add(scenarios, "autoresearch-software-compare", f"{left} 및 {right} 설치 사용자를 장비별로 보여줘", ("nodeinfo",), contextual=False)

    vaccine = sections.get("VACCINE", {})
    for product in top_values(vaccine, "top_products", limit=40):
        add(scenarios, "autoresearch-security", f"{product} 백신 설치 장비를 보여줘")
        add(scenarios, "autoresearch-security", f"{product} 백신 상태와 서명을 사용자별로 보여줘")
        add(scenarios, "autoresearch-security", f"{product} 보안 제품 서명별 장비 수를 보여줘")
        add(scenarios, "autoresearch-security", f"{product} 보안 제품 상태 분포를 보여줘")
        add(scenarios, "autoresearch-security", f"{product} 보안 제품 상태별 장비 수를 보여줘")
        add(scenarios, "autoresearch-security", f"{product} 보안 제품 상태와 서명을 장비별로 보여줘")
        add(scenarios, "autoresearch-security", f"{product} 보안 제품이 설치된 노드와 사용자를 보여줘")
        add(scenarios, "autoresearch-security", f"{product} 보안 제품 표시 이름과 상태를 장비별로 보여줘")
    for status in top_values(vaccine, "top_statuses", limit=20) or top_values(vaccine, "top_states", limit=20):
        add(scenarios, "autoresearch-security", f"백신 Status {status} 상태인 장비를 보여줘")
        add(scenarios, "autoresearch-security", f"보안 제품 상태가 {status}인 장비와 서명을 보여줘")
    for signature in top_values(vaccine, "top_signatures", limit=20):
        add(scenarios, "autoresearch-security", f"백신 Signature {signature} 서명 상태인 장비를 보여줘")
    for left, right in value_pairs(top_values(vaccine, "top_products", limit=20), limit=5):
        add(scenarios, "autoresearch-security-compare", f"{left} 및 {right} 보안 제품 상태를 장비별로 비교해줘", ("nodeinfo",))
        add(scenarios, "autoresearch-security-compare", f"{left} 및 {right} 보안 제품 서명별 장비 수를 비교해줘", ("nodeinfo",))

    update = sections.get("UPDATE", {})
    for hotfix in top_values(update, "top_hotfixes", limit=60):
        add(scenarios, "autoresearch-patch", f"{hotfix} 설치 장비를 사용자별로 보여줘", contextual=False)
        add(scenarios, "autoresearch-patch", f"{hotfix} HotFixID별 설치 장비 수를 보여줘", contextual=False)
        add(scenarios, "autoresearch-patch", f"{hotfix} 패치 설치일을 장비별로 보여줘", contextual=False)
        add(scenarios, "autoresearch-patch", f"{hotfix} InstalledBy 기준 설치 장비 수를 보여줘", contextual=False)
        add(scenarios, "autoresearch-patch", f"{hotfix} Status 기준 패치 장비 수를 보여줘", contextual=False)
        add(scenarios, "autoresearch-patch", f"{hotfix} Caption과 설치일을 노드별로 보여줘", contextual=False)

    os_section = sections.get("OS", {})
    for os_name in top_values(os_section, "top_os", limit=30):
        add(scenarios, "autoresearch-os", f"{os_name} 장비를 사용자별로 보여줘")
        add(scenarios, "autoresearch-os", f"{os_name} 장비의 UBR 분포를 보여줘")
        add(scenarios, "autoresearch-os", f"{os_name} 장비의 빌드 분포를 보여줘")
    for build in top_values(os_section, "top_builds", limit=40):
        add(scenarios, "autoresearch-os", f"OS 빌드 {build} 장비를 사용자별로 보여줘")

    system = sections.get("SYSTEM", {})
    for manufacturer in top_values(system, "top_manufacturers", limit=50):
        add(scenarios, "autoresearch-asset", f"{manufacturer} 제조사 장비를 모델별로 보여줘", ("nodeinfo",))
        add(scenarios, "autoresearch-asset", f"{manufacturer} 제조사 장비를 사용자별로 보여줘", ("nodeinfo",))
        add(scenarios, "autoresearch-asset", f"{manufacturer} 제조사 장비 수를 보여줘", ("nodeinfo",))
    for model in top_values(system, "top_models", limit=60):
        add(scenarios, "autoresearch-asset", f"{model} 모델 장비를 사용자별로 보여줘", ("nodeinfo",))
        add(scenarios, "autoresearch-asset", f"{model} 모델 장비 수를 보여줘", ("nodeinfo",))

    for section_name, label in [
        ("CPU", "CPU 모델"),
        ("MEMORY", "메모리 제조사"),
        ("DISK", "디스크 모델"),
        ("DISKDRIVE", "디스크 파일시스템"),
        ("NETWORKADAPTER", "네트워크 어댑터"),
        ("PRINTER", "프린터 드라이버"),
        ("VIDEOCARD", "그래픽 카드"),
        ("AGENT", "에이전트 버전"),
    ]:
        section = sections.get(section_name, {})
        expected = ("nodeinfo",)
        values = (
            version_values(section, "top_values", limit=60)
            if section_name == "AGENT"
            else top_counted_values(
                section,
                "top_values",
                limit=60,
                min_count=MIN_PROFILED_HARDWARE_VALUE_NODES,
            )
        )
        for value in values:
            add(scenarios, f"autoresearch-{section_name.lower()}", f"{value} {label} 장비를 사용자별로 보여줘", expected)
            add(scenarios, f"autoresearch-{section_name.lower()}", f"{value} {label} 장비 수를 보여줘", expected)
            add(scenarios, f"autoresearch-{section_name.lower()}", f"{value} {label} 장비 목록을 보여줘", expected)
            if section_name == "CPU":
                add(scenarios, "autoresearch-cpu-os", f"{value} CPU 모델 장비의 윈도우 버전을 보여줘", expected)
            if section_name == "VIDEOCARD":
                add(scenarios, "autoresearch-videocard-os", f"{value} 그래픽 카드 장비의 윈도우 버전을 보여줘", expected)
        if section_name in {"PRINTER", "VIDEOCARD", "NETWORKADAPTER", "MEMORY"}:
            for left, right in value_pairs(values, limit=8):
                add(scenarios, f"autoresearch-{section_name.lower()}-compare", f"{left} 및 {right} {label} 장비 수를 비교해줘", expected)

    generic_questions = [
        ("회사 전체 프로그램 설치 목록을 제품명 버전 게시자별로 보여줘", ("nodeinfo",)),
        ("회사 전체 설치 프로그램을 제품명별로 집계해줘", ("nodeinfo",)),
        ("회사 전체 소프트웨어 인벤토리를 버전과 게시자 기준으로 보여줘", ("nodeinfo",)),
        ("회사 전체 설치 프로그램을 제품명 게시자 버전 노드 수 기준으로 정렬해줘", ("nodeinfo",)),
        ("설치 프로그램별 사용자 수와 장비 수를 보여줘", ("nodeinfo",)),
        ("노드별 설치 소프트웨어 상태를 제품명 버전 게시자 설치 위치 기준으로 보여줘", ("nodeinfo",)),
        ("설치 원본과 제거 명령이 있는 프로그램을 제품명별로 보여줘", ("nodeinfo",)),
        ("설치일이 오래된 프로그램을 장비별로 보여줘", ("nodeinfo",)),
        ("구버전으로 보이는 설치 프로그램을 제품별로 찾아줘", ("nodeinfo",)),
        ("동일 제품의 여러 버전이 설치된 현황을 보여줘", ("nodeinfo",)),
        ("프로그램 설치 경로와 게시자 메타데이터를 장비별로 보여줘", ("nodeinfo",)),
        ("프로그램이 가장 많이 설치된 노드를 찾아줘", ("nodeinfo",)),
        ("프로그램이 가장 적게 설치된 노드를 찾아줘", ("nodeinfo",)),
        ("회사 전체 설치 프로그램을 게시자와 제품 버전 기준으로 분포를 보여줘", ("nodeinfo",)),
        ("노드별 소프트웨어 설치 상태를 제품명 버전 설치일 기준으로 보여줘", ("nodeinfo",)),
        ("노드별 소프트웨어 설치 상태를 제품명 버전 게시자 설치키 기준으로 보여줘", ("nodeinfo",)),
        ("회사 전체 설치 프로그램의 제품 버전 분포를 게시자별로 보여줘", ("nodeinfo",)),
        ("회사 전체 설치 프로그램의 게시자 분포와 장비 수를 보여줘", ("nodeinfo",)),
        ("회사 전체 소프트웨어를 제품명 게시자 버전 노드수 기준으로 재고화해줘", ("nodeinfo",)),
        ("설치 프로그램 인벤토리를 제품명별 사용자수와 장비수로 요약해줘", ("nodeinfo",)),
        ("설치 프로그램 게시자 분포를 제품 버전과 함께 점검해줘", ("nodeinfo",)),
        ("노드별 설치 소프트웨어 상태를 제품명 게시자 설치일 설치위치 기준으로 보여줘", ("nodeinfo",)),
        ("설치 위치가 기록된 프로그램을 제품명 버전 장비별로 보여줘", ("nodeinfo",)),
        ("설치 프로그램에서 설치 원본이 기록된 항목을 제품명 게시자별로 보여줘", ("nodeinfo",)),
        ("설치 프로그램 중 설치 원본이 있는 항목을 설치 원본 경로와 제품명별로 보여줘", ("nodeinfo",)),
        ("설치 프로그램 중 설치 원본이 기록된 항목을 사용자와 장비명 기준으로 보여줘", ("nodeinfo",)),
        ("설치 프로그램에서 제거 명령 문자열이 기록된 항목을 제품명 버전별로 보여줘", ("nodeinfo",)),
        ("설치 프로그램 중 제거 명령 문자열이 있는 항목을 게시자별로 집계해줘", ("nodeinfo",)),
        ("설치 프로그램 중 설치 위치가 없는 항목을 장비와 사용자 기준으로 보여줘", ("nodeinfo",)),
        ("회사 전체 설치 프로그램을 installKey 기준 제품명 버전 분포로 보여줘", ("nodeinfo",)),
        ("설치 프로그램 중 installKey가 있는 항목을 제품명 버전 게시자 기준으로 보여줘", ("nodeinfo",)),
        ("설치 프로그램 중 installLocation이 있는 항목을 장비명 제품명 기준으로 보여줘", ("nodeinfo",)),
        ("설치 프로그램 중 installSource가 있는 항목을 제품명과 설치 원본 기준으로 보여줘", ("nodeinfo",)),
        ("설치 프로그램 중 uninstallString이 있는 항목을 제품명과 제거 명령 기준으로 보여줘", ("nodeinfo",)),
        ("설치 프로그램 중 installedTime이 기록된 항목을 최근 설치일 순서로 보여줘", ("nodeinfo",)),
        ("설치 프로그램 중 Path가 기록된 항목을 제품명 경로 기준으로 보여줘", ("nodeinfo",)),
        ("설치 프로그램 중 제품명 버전 installKey 조합별 장비 수를 보여줘", ("nodeinfo",)),
        ("설치 프로그램 중 게시자 제품명 installLocation 조합별 장비 수를 보여줘", ("nodeinfo",)),
        ("제품명과 설치키 기준으로 설치 프로그램 중복 후보를 보여줘", ("nodeinfo",)),
        ("설치 소스가 기록된 프로그램을 게시자와 제품명 기준으로 보여줘", ("nodeinfo",)),
        ("제거 명령이 기록된 프로그램을 제품명 버전별로 보여줘", ("nodeinfo",)),
        ("설치키가 기록된 프로그램을 사용자와 장비명 기준으로 보여줘", ("nodeinfo",)),
        ("제품명 버전 게시자가 모두 있는 설치 항목을 노드별로 보여줘", ("nodeinfo",)),
        ("제품명은 같고 게시자가 다른 설치 프로그램 분포를 보여줘", ("nodeinfo",)),
        ("Microsoft Edge 설치 노드의 제품 버전 게시자 상태를 사용자별로 보여줘", ("nodeinfo",)),
        ("Microsoft OneDrive 설치 노드의 OS 빌드와 제품 버전을 보여줘", ("nodeinfo",)),
        ("Microsoft Update Health Tools 설치 장비의 OS Caption BuildNumber UBR 분포를 보여줘", ("nodeinfo",)),
        ("Google Chrome 설치 장비의 사용자 제품버전 게시자 현황을 보여줘", ("nodeinfo",)),
        ("Microsoft Visual C++ 2015-2022 Redistributable 설치 버전과 게시자 분포를 보여줘", ("nodeinfo",)),
        ("캡처 도구 설치 항목의 제품 버전과 사용자별 장비 목록을 보여줘", ("nodeinfo",)),
        ("Orange The Client 1.6 설치 항목과 AGENT build Path를 장비별로 점검해줘", ("nodeinfo",)),
        ("Edge Chrome OneDrive 설치 버전을 제품별 장비수 기준으로 비교해줘", ("nodeinfo",)),
        ("AhnLab V3 Lite 보안 제품 설치 상태를 사용자별로 보여줘", ("nodeinfo",)),
        ("알약 보안 제품 설치 상태를 사용자별로 보여줘", ("nodeinfo",)),
        ("백신 보안 제품 displayName Signature Status를 노드별로 보여줘", ("nodeinfo",)),
        ("백신 displayName Status Signature 조합별 장비 수를 보여줘", ("nodeinfo",)),
        ("VACCINE 인벤토리에서 Status 값이 있는 보안 제품을 장비별로 보여줘", ("nodeinfo",)),
        ("보안 제품 Status별 displayName 장비 수를 보여줘", ("nodeinfo",)),
        ("보안 제품 Signature별 displayName 장비 수를 보여줘", ("nodeinfo",)),
        ("백신 제품 displayName Status Signature를 사용자별로 보여줘", ("nodeinfo",)),
        ("VACCINE 인벤토리에서 displayName Status Owner 기준 장비 수를 보여줘", ("nodeinfo",)),
        ("VACCINE 인벤토리에서 Signature Status 조합별 보안 제품 수를 보여줘", ("nodeinfo",)),
        ("보안 제품 displayName Owner Signature를 장비별로 보여줘", ("nodeinfo",)),
        ("백신 제품 Status가 기록된 항목을 displayName별로 집계해줘", ("nodeinfo",)),
        ("Windows Defender 보안 제품 서명 상태를 사용자별로 보여줘", ("nodeinfo",)),
        ("AhnLab V3 Lite 보안 제품 서명 상태를 사용자별로 보여줘", ("nodeinfo",)),
        ("알약 보안 제품 서명 상태를 사용자별로 보여줘", ("nodeinfo",)),
        ("VACCINE 보안 제품이 없는 관리 대상 장비 후보를 보여줘", ("nodeinfo",)),
        ("OS 빌드별 설치 프로그램 수와 보안 제품 상태를 보여줘", ("nodeinfo",)),
        ("HotFixID별 설치 장비와 OS BuildNumber UBR 분포를 보여줘", ("nodeinfo",)),
        ("UPDATE 인벤토리에서 InstalledBy별 HotFixID 장비 수를 보여줘", ("nodeinfo",)),
        ("HotFixID InstalledBy별 설치 장비 수를 보여줘", ("nodeinfo",)),
        ("패치 InstalledBy와 Status별 HotFixID 분포를 보여줘", ("nodeinfo",)),
        ("UPDATE 인벤토리에서 HotFixID Caption Status 조합별 장비 수를 보여줘", ("nodeinfo",)),
        ("UPDATE 인벤토리에서 InstalledOn이 기록된 HotFixID를 장비별로 보여줘", ("nodeinfo",)),
        ("패치 HotFixID InstalledBy Status를 사용자별 장비별로 보여줘", ("nodeinfo",)),
        ("윈도우 업데이트 HotFixID Caption InstalledOn을 장비명 기준으로 보여줘", ("nodeinfo",)),
        ("패치 설치일이 오래된 장비를 HotFixID InstalledOn 기준으로 보여줘", ("nodeinfo",)),
        ("KB5066790 패치 적용 장비를 사용자별로 보여줘", ("nodeinfo",)),
        ("KB5072653 패치 적용 장비를 사용자별로 보여줘", ("nodeinfo",)),
        ("KB5015684 패치 적용 장비를 사용자별로 보여줘", ("nodeinfo",)),
        ("KB5033052 설치 장비의 사용자와 OS 빌드 분포를 보여줘", ("nodeinfo",)),
        ("KB5011048 설치 장비를 HotFixID InstalledBy Status 기준으로 보여줘", ("nodeinfo",)),
        ("KB5011063 패치 설치 장비의 Caption과 InstalledOn을 장비별로 보여줘", ("nodeinfo",)),
        ("KB5066130 적용 장비와 OS BuildNumber UBR을 함께 보여줘", ("nodeinfo",)),
        ("KB4537759 설치 장비의 사용자 장비명 패치 상태를 보여줘", ("nodeinfo",)),
        ("에이전트 build Path DeviceId를 장비별로 보여줘", ("nodeinfo",)),
        ("오프라인 관리 대상 장비 후보의 에이전트 버전과 설치 경로를 보여줘", ("nodeinfo",)),
        ("네트워크 IP MAC 어댑터 정보를 장비별로 보여줘", ("nodeinfo",)),
        ("NETWORK 인벤토리에서 IP MAC gateway subnet 정보를 노드별로 보여줘", ("nodeinfo",)),
        ("NETWORK IP와 MAC 주소를 어댑터 이름별로 보여줘", ("nodeinfo",)),
        ("네트워크 IP MAC gateway subnet을 장비명 기준으로 보여줘", ("nodeinfo",)),
        ("NETWORK 인벤토리에서 dhcp gateway subnet 조합별 장비 수를 보여줘", ("nodeinfo",)),
        ("NETWORK 인벤토리에서 best IP MAC 정보를 사용자별로 보여줘", ("nodeinfo",)),
        ("NETWORKADAPTER 인벤토리에서 NetEnabled Status Speed 기준 장비 수를 보여줘", ("nodeinfo",)),
        ("NETWORKADAPTER 인벤토리에서 MACAddress Manufacturer Name을 장비별로 보여줘", ("nodeinfo",)),
        ("MAC 주소와 네트워크 어댑터 제조사 분포를 보여줘", ("nodeinfo",)),
        ("Intel 82574L Gigabit 네트워크 어댑터 장비를 사용자별로 보여줘", ("nodeinfo",)),
        ("Bluetooth Device Personal Area Network 어댑터 장비 상태를 보여줘", ("nodeinfo",)),
        ("Realtek PCIe GbE 네트워크 어댑터 장비 수를 사용자별로 보여줘", ("nodeinfo",)),
        ("CPU 메모리 디스크 OS 하드웨어 인벤토리를 장비별로 보여줘", ("nodeinfo",)),
        ("제조사 모델 BIOS BaseBoard 메타데이터를 장비별로 보여줘", ("nodeinfo",)),
        ("파일시스템별 디스크 여유 공간 부족 장비를 찾아줘", ("nodeinfo",)),
        ("NTFS 디스크 파일시스템 장비의 여유공간과 상태를 사용자별로 보여줘", ("nodeinfo",)),
        ("VMware Virtual NVMe Disk 모델 장비의 OS 버전과 사용자 정보를 보여줘", ("nodeinfo",)),
        ("SAMSUNG MZAL81T0HDLB-00BL2 디스크 모델 장비 수를 보여줘", ("nodeinfo",)),
        ("그래픽 카드 드라이버와 OS 빌드를 장비별로 보여줘", ("nodeinfo",)),
        ("VMware SVGA 3D 그래픽 카드 장비의 OS 빌드 분포를 보여줘", ("nodeinfo",)),
        ("Intel Iris Xe Graphics 그래픽 카드 장비를 사용자별로 보여줘", ("nodeinfo",)),
        ("메모리 제조사 파트번호 용량을 장비별로 보여줘", ("nodeinfo",)),
        ("Samsung 메모리 제조사 장비의 OS 빌드와 사용자 정보를 보여줘", ("nodeinfo",)),
        ("VMware Virtual RAM 메모리 장비 수를 모델별로 보여줘", ("nodeinfo",)),
        ("CPU 코어 수 논리 프로세서 수 OS 빌드를 장비별로 보여줘", ("nodeinfo",)),
        ("11th Gen Intel Core i5-1135G7 CPU 모델 장비의 메모리 용량과 OS를 보여줘", ("nodeinfo",)),
        ("Intel Core Ultra 7 255H CPU 모델 장비 수를 사용자별로 보여줘", ("nodeinfo",)),
        ("제품 버전이 오래된 설치 프로그램 후보를 게시자별로 보여줘", ("nodeinfo",)),
        ("백신 제품과 상태를 노드별로 보여줘", ("nodeinfo",)),
        ("백신 제품이 여러 개 설치된 장비를 찾아줘", ("nodeinfo",)),
        ("보안 제품 상태와 서명 분포를 보여줘", ("nodeinfo",)),
        ("백신 보안 제품 상태와 서명을 장비별로 보여줘", ("nodeinfo",)),
        ("주요 보안 제품 상태 분포를 보여줘", ("nodeinfo",)),
        ("주요 백신 제품 버전과 서명 상태를 장비별로 보여줘", ("nodeinfo",)),
        ("보안 제품 displayName 상태 Signature 기준 장비 수를 보여줘", ("nodeinfo",)),
        ("윈도우 업데이트 HotFixID별 설치 장비 수를 보여줘", ("nodeinfo",)),
        ("윈도우 업데이트 설치일별 패치 현황을 보여줘", ("nodeinfo",)),
        ("패치 적용 장비와 미적용 가능 장비를 HotFixID 기준으로 점검해줘", ("nodeinfo",)),
        ("패치 적용 준비 상태를 HotFixID와 OS 빌드 기준으로 보여줘", ("nodeinfo",)),
        ("HotFixID InstalledOn Status 기준 패치 분포를 보여줘", ("nodeinfo",)),
        ("프린터 드라이버별 설치 장비 수를 보여줘", ("nodeinfo",)),
        ("네트워크 어댑터 제조사별 장비 수를 보여줘", ("nodeinfo",)),
        ("OS 빌드와 UBR 기준으로 장비 분포를 보여줘", ("nodeinfo",)),
        ("OS Caption Version BuildNumber UBR 기준 장비 목록을 보여줘", ("nodeinfo",)),
        ("OS 인벤토리에서 Caption Version BuildNumber UBR 조합별 장비 수를 보여줘", ("nodeinfo",)),
        ("OS 인벤토리에서 InstallDate LastBootUpTime BuildNumber를 장비별로 보여줘", ("nodeinfo",)),
        ("OS 인벤토리에서 OSArchitecture Caption Version 기준 장비 수를 보여줘", ("nodeinfo",)),
        ("OS 인벤토리에서 SystemDirectory와 BuildNumber를 장비명 기준으로 보여줘", ("nodeinfo",)),
        ("Microsoft Windows 11 Pro Insider Preview 장비의 빌드 분포를 보여줘", ("nodeinfo",)),
        ("하드웨어 인벤토리를 제조사 모델 CPU 메모리 기준으로 보여줘", ("nodeinfo",)),
        ("오프라인 관리 대상 장비의 OS 빌드와 에이전트 버전을 보여줘", ("nodeinfo",)),
        ("서명 없는 파일의 회사명과 제품 버전 분포를 보여줘", ("filelist",)),
        ("디스크 부족한 노드들 출력", ("nodeinfo",)),
        ("C 드라이브 여유 공간이 적은 노드를 찾아줘", ("nodeinfo",)),
        ("물리 메모리 크기가 제일 큰 장비와 사용자를 보여줘", ("node", "nodeinfo")),
        ("삼성 장비를 가진 사용자를 찾아줘", ("nodeinfo",)),
        ("Mirage 그래픽 드라이버 설치한 노드", ("nodeinfo",)),
    ]
    generic_questions.extend(
        [
            ("설치 프로그램 중 제품명 버전 게시자 installLocation 조합별 장비 수를 보여줘", ("nodeinfo",)),
            ("설치 프로그램 중 installKey 제품명 게시자 조합별 장비 수를 보여줘", ("nodeinfo",)),
            ("설치 프로그램 중 installedTime이 기록된 항목을 제품명 버전 게시자 기준으로 보여줘", ("nodeinfo",)),
            ("설치 프로그램 중 installSource와 uninstallString이 기록된 항목을 제품명별로 보여줘", ("nodeinfo",)),
            ("회사 전체 설치 프로그램의 publisher version 누락 현황을 제품명별로 보여줘", ("nodeinfo",)),
            ("VACCINE 인벤토리에서 displayName Signature Status Owner 조합별 장비 수를 보여줘", ("nodeinfo",)),
            ("백신 보안 제품 Signature가 비어 있는 항목을 displayName별로 보여줘", ("nodeinfo",)),
            ("백신 보안 제품 Owner와 Status를 장비명 사용자 기준으로 보여줘", ("nodeinfo",)),
            ("UPDATE 인벤토리에서 HotFixID InstalledOn InstalledBy Status를 장비별로 보여줘", ("nodeinfo",)),
            ("UPDATE 인벤토리에서 Caption Description HotFixID 조합별 장비 수를 보여줘", ("nodeinfo",)),
            ("NETWORK 인벤토리에서 best type dhcp gateway subnet 조합별 장비 수를 보여줘", ("nodeinfo",)),
            ("NETWORK 인벤토리에서 IP MAC type best 정보를 장비명 사용자 기준으로 보여줘", ("nodeinfo",)),
            ("NETWORKADAPTER 인벤토리에서 Manufacturer AdapterType NetEnabled Status 조합별 장비 수를 보여줘", ("nodeinfo",)),
            ("NETWORKADAPTER 인벤토리에서 NetConnectionID MACAddress Speed Status를 장비별로 보여줘", ("nodeinfo",)),
            ("SYSTEM 인벤토리에서 Manufacturer Model SystemType PCSystemType 조합별 장비 수를 보여줘", ("nodeinfo",)),
            ("SYSTEM 인벤토리에서 BIOS 제조사 BaseBoard 제조사 제품 조합별 장비 수를 보여줘", ("nodeinfo",)),
            ("OS 인벤토리에서 Caption Version BuildNumber UBR OSArchitecture 조합별 장비 수를 보여줘", ("nodeinfo",)),
            ("OS 인벤토리에서 InstallDate LastBootUpTime LocalDateTime을 장비별로 보여줘", ("nodeinfo",)),
            ("CPU 인벤토리에서 Name NumberOfCores NumberOfLogicalProcessors MaxClockSpeed를 장비별로 보여줘", ("nodeinfo",)),
            ("MEMORY 인벤토리에서 Manufacturer PartNumber Capacity Speed를 장비별로 보여줘", ("nodeinfo",)),
            ("DISKDRIVE 인벤토리에서 Name FileSystem DriveType FreeSpace Size를 장비별로 보여줘", ("nodeinfo",)),
            ("DISK 인벤토리에서 Model MediaType Size Status를 장비별로 보여줘", ("nodeinfo",)),
            ("VIDEOCARD 인벤토리에서 Name AdapterCompatibility AdapterRAM Status 조합별 장비 수를 보여줘", ("nodeinfo",)),
            ("프린터 인벤토리에서 DriverName PortName Default Status 조합별 장비 수를 보여줘", ("nodeinfo",)),
            ("에이전트 인벤토리에서 build와 설치 경로를 장비별로 보여줘", ("nodeinfo",)),
            ("회사 전체 설치 프로그램을 제품명 게시자 설치위치 유무 기준으로 요약해줘", ("nodeinfo",)),
            ("설치 프로그램 중 버전과 게시자가 모두 기록된 항목을 제품명별 장비 수로 보여줘", ("nodeinfo",)),
            ("설치 프로그램 중 publisher가 비어 있지 않은 항목을 게시자 제품명 버전 기준으로 보여줘", ("nodeinfo",)),
            ("설치 프로그램 중 installLocation과 Path가 모두 있는 항목을 장비별로 보여줘", ("nodeinfo",)),
            ("설치 프로그램 중 installSource가 비어 있지 않은 항목을 설치 원본과 제품명 기준으로 보여줘", ("nodeinfo",)),
            ("설치 프로그램 중 uninstallString이 비어 있지 않은 항목을 제거 명령과 게시자 기준으로 보여줘", ("nodeinfo",)),
            ("설치 프로그램별 설치된 노드 수와 사용자 수를 제품명 버전 기준으로 보여줘", ("nodeinfo",)),
            ("소프트웨어 인벤토리에서 제품명 게시자 버전 조합이 많은 항목을 보여줘", ("nodeinfo",)),
            ("주요 엔드포인트 설치 제품 상태를 제품명 버전 게시자 기준으로 점검해줘", ("nodeinfo",)),
            ("국내 보안 모듈 설치 후보를 제품명 게시자 기준으로 점검해줘", ("nodeinfo",)),
            ("업무용 소프트웨어 설치 후보를 제품명 버전 게시자 기준으로 점검해줘", ("nodeinfo",)),
            ("VACCINE 인벤토리에서 displayName별 Status Signature Owner 분포를 보여줘", ("nodeinfo",)),
            ("VACCINE 인벤토리에서 보안 제품이 여러 상태로 보고된 장비를 찾아줘", ("nodeinfo",)),
            ("VACCINE 인벤토리에서 FileCounters CounterCount가 있는 보안 제품을 장비별로 보여줘", ("nodeinfo",)),
            ("Windows Defender AhnLab V3 Lite 알약 보안 제품 상태를 displayName 기준으로 비교해줘", ("nodeinfo",)),
            ("UPDATE 인벤토리에서 최근 InstalledOn 기준 HotFixID와 InstalledBy 분포를 보여줘", ("nodeinfo",)),
            ("UPDATE 인벤토리에서 Status가 기록된 패치를 HotFixID Caption 기준으로 보여줘", ("nodeinfo",)),
            ("패치 준비 상태를 OS BuildNumber UBR와 HotFixID 기준으로 요약해줘", ("nodeinfo",)),
            ("OS 인벤토리에서 Caption BuildNumber UBR별 관리 대상 장비 수를 보여줘", ("nodeinfo",)),
            ("OS 인벤토리에서 LastBootUpTime이 오래된 장비를 Caption BuildNumber 기준으로 보여줘", ("nodeinfo",)),
            ("OS 인벤토리에서 LocalDateTime과 LastBootUpTime을 장비별로 비교해줘", ("nodeinfo",)),
            ("에이전트 인벤토리에서 build와 설치 경로별 장비 수를 보여줘", ("nodeinfo",)),
            ("NETWORK 인벤토리에서 best true인 IP MAC gateway subnet 정보를 장비별로 보여줘", ("nodeinfo",)),
            ("NETWORK 인벤토리에서 dhcp 값과 type 기준으로 IP 대역 분포를 보여줘", ("nodeinfo",)),
            ("NETWORKADAPTER 인벤토리에서 NetEnabled true 장비를 Manufacturer Name 기준으로 보여줘", ("nodeinfo",)),
            ("NETWORKADAPTER 인벤토리에서 NetConnectionStatus와 Speed 분포를 어댑터별로 보여줘", ("nodeinfo",)),
            ("프린터 인벤토리에서 기본 프린터를 드라이버명과 포트명 기준으로 보여줘", ("nodeinfo",)),
            ("프린터 인벤토리에서 드라이버명과 상태별 장비 수를 보여줘", ("nodeinfo",)),
            ("SYSTEM 인벤토리에서 BIOS ReleaseDate와 Manufacturer Model을 장비별로 보여줘", ("nodeinfo",)),
            ("SYSTEM 인벤토리에서 PCSystemType SystemType별 제조사 모델 분포를 보여줘", ("nodeinfo",)),
            ("CPU 인벤토리에서 코어 수와 논리 프로세서 수가 다른 장비를 보여줘", ("nodeinfo",)),
            ("메모리 인벤토리에서 제조사 파트번호 용량 속도별 장비 수를 보여줘", ("nodeinfo",)),
            ("DISK 인벤토리에서 SerialNumber가 있는 디스크를 Model Size Status 기준으로 보여줘", ("nodeinfo",)),
            ("DISKDRIVE 인벤토리에서 DriveType FileSystem FreeSpace Size 기준 저장공간 현황을 보여줘", ("nodeinfo",)),
            ("VIDEOCARD 인벤토리에서 AdapterCompatibility Name Status별 장비 수를 보여줘", ("nodeinfo",)),
            ("파일 인벤토리에서 PFilePath와 ProductVersion이 있는 항목을 제품명 기준으로 보여줘", ("filelist",)),
            ("파일 인벤토리에서 ZoneId HostUrl이 있는 파일을 최근 생성 순서로 보여줘", ("filelist",)),
            ("Microsoft Edge 설치 장비를 버전 게시자 사용자 기준으로 보여줘", ("nodeinfo",)),
            ("Microsoft OneDrive 설치 버전 분포를 사용자와 장비명 기준으로 보여줘", ("nodeinfo",)),
            ("Orange The Client 설치 경로와 버전을 장비별로 보여줘", ("nodeinfo",)),
            ("Microsoft Update Health Tools 설치 장비를 버전별로 집계해줘", ("nodeinfo",)),
            ("VMware Tools 설치 장비의 제품 버전과 게시자를 보여줘", ("nodeinfo",)),
            ("Visual C++ 2015-2022 재배포 패키지 설치 버전을 x64 x86 기준으로 보여줘", ("nodeinfo",)),
            ("Windows Defender 보안 제품 상태와 서명을 장비별로 보여줘", ("nodeinfo",)),
            ("AhnLab V3 Lite와 알약 보안 제품 상태를 사용자별로 비교해줘", ("nodeinfo",)),
            ("KB5066790과 KB5015684 패치 설치 장비 수를 비교해줘", ("nodeinfo",)),
            ("KB5072653 패치가 설치된 장비의 OS 빌드와 사용자를 보여줘", ("nodeinfo",)),
            ("Microsoft Print To PDF 기본 프린터 상태를 장비별로 보여줘", ("nodeinfo",)),
            ("SINDOH 프린터 드라이버 설치 장비를 드라이버명 포트명 기준으로 보여줘", ("nodeinfo",)),
            ("VMware SVGA 3D 그래픽 장비의 드라이버 상태와 비디오 모드를 보여줘", ("nodeinfo",)),
            ("Microsoft Basic Display Adapter 장비 수를 OS 빌드 기준으로 보여줘", ("nodeinfo",)),
            ("Intel Wi-Fi 6 AX201 네트워크 어댑터 장비를 MAC 주소와 속도 기준으로 보여줘", ("nodeinfo",)),
            ("Realtek PCIe 네트워크 어댑터 장비 수를 상태별로 보여줘", ("nodeinfo",)),
            ("NTFS 파일시스템 드라이브의 여유 공간과 크기를 장비별로 보여줘", ("nodeinfo",)),
            ("Samsung 메모리 제조사 장비를 파트번호와 속도 기준으로 보여줘", ("nodeinfo",)),
            ("VMware Virtual NVMe Disk 장비의 디스크 크기와 상태를 보여줘", ("nodeinfo",)),
        ]
    )
    if has_profile_empty_value(uninstall, "top_install_location_products"):
        generic_questions.append(("설치 프로그램에서 설치 위치가 비어 있는 항목을 제품명 게시자별로 보여줘", ("nodeinfo",)))
    if has_profile_empty_value(uninstall, "top_publishers"):
        generic_questions.append(("설치 프로그램 중 publisher가 비어 있는 항목을 장비명 제품명 기준으로 보여줘", ("nodeinfo",)))
    if has_profile_empty_value(uninstall, "top_versions"):
        generic_questions.append(("설치 프로그램 중 version이 비어 있는 항목을 게시자 제품명 기준으로 보여줘", ("nodeinfo",)))
    for question, expected in generic_questions:
        add(scenarios, "autoresearch-generic", question, expected)
    return scenarios


def filelist_profile_scenarios(profile: dict[str, Any]) -> list[Scenario]:
    scenarios: list[Scenario] = []
    for product in top_counted_values(profile, "top_product_names", limit=50, min_count=2):
        add(scenarios, "autoresearch-file-product", f"{product} 파일 경로와 파일 버전을 보여줘", ("filelist",), contextual=False)
        add(scenarios, "autoresearch-file-product", f"{product} 파일 서명과 회사명을 보여줘", ("filelist",), contextual=False)
        add(scenarios, "autoresearch-file-product", f"{product} 제품 파일을 버전별로 집계해줘", ("filelist",), contextual=False)
        add(scenarios, "autoresearch-file-product", f"{product} 파일이 생성된 장비와 경로를 보여줘", ("filelist",), contextual=False)
    for company in top_counted_values(profile, "top_company_names", limit=50, min_count=2):
        add(scenarios, "autoresearch-file-company", f"{company} 회사 파일을 제품명과 버전별로 보여줘", ("filelist",), contextual=False)
        add(scenarios, "autoresearch-file-company", f"{company} 서명 파일의 설치 경로 분포를 보여줘", ("filelist",), contextual=False)
        add(scenarios, "autoresearch-file-company", f"{company} 파일을 장비별로 보여줘", ("filelist",), contextual=False)
    for signer in top_counted_values(profile, "top_signers", limit=40, min_count=2):
        add(scenarios, "autoresearch-file-signer", f"{signer} 서명 파일을 제품 버전별로 보여줘", ("filelist",), contextual=False)
        add(scenarios, "autoresearch-file-signer", f"{signer} 서명 파일 경로를 장비별로 보여줘", ("filelist",), contextual=False)
    for file_name in top_counted_values(profile, "top_file_names", limit=60, min_count=2):
        add(scenarios, "autoresearch-file-name", f"{file_name} 파일이 있는 장비와 경로를 보여줘", ("filelist",), contextual=False)
        add(scenarios, "autoresearch-file-name", f"{file_name} 파일 버전 분포를 보여줘", ("filelist",), contextual=False)
    for install_path in top_counted_values(profile, "top_install_paths", limit=40, min_count=2):
        add(scenarios, "autoresearch-file-path", f"{install_path} 경로의 파일을 회사명 제품명 기준으로 보여줘", ("filelist",), contextual=False)
    for extension in top_counted_values(profile, "top_extensions", limit=20, min_count=2):
        if extension and len(extension) <= 8:
            add(scenarios, "autoresearch-file-extension", f"{extension} 확장자 파일을 제품명과 서명 기준으로 보여줘", ("filelist",), contextual=False)
    generic = [
        "최근 생성된 파일을 제품명 회사명 서명 기준으로 보여줘",
        "파일 서명이 없는 항목을 장비별로 보여줘",
        "다운로드 출처가 있는 파일을 최근 생성 순서로 보여줘",
        "시스템 파일이 아닌 exe 파일을 회사명별로 보여줘",
    ]
    if has_profile_empty_value(profile, "top_company_names"):
        generic.append("회사명 없는 실행 파일을 경로별로 보여줘")
    if has_profile_empty_value(profile, "top_product_versions"):
        generic.append("제품 버전이 비어 있는 파일을 제품명별로 보여줘")
    for question in generic:
        add(scenarios, "autoresearch-file-generic", question, ("filelist",), contextual=False)
    return scenarios


def sprocess_profile_scenarios(profile: dict[str, Any]) -> list[Scenario]:
    scenarios: list[Scenario] = []
    for proc_name in top_counted_values(profile, "top_proc_names", limit=80, min_count=2):
        add(scenarios, "autoresearch-process", f"{proc_name} 프로세스 평균 CPU Memory IO Handle 부하지수를 보여줘", ("sprocess",), contextual=False)
        add(scenarios, "autoresearch-process", f"{proc_name} 프로세스가 실행된 장비와 명령줄을 보여줘", ("sprocess",), contextual=False)
        add(scenarios, "autoresearch-process", f"{proc_name} 프로세스 파일 버전과 회사명을 보여줘", ("sprocess",), contextual=False)
        add(scenarios, "autoresearch-process", f"{proc_name} 프로세스 측정 횟수와 평균 부하지수를 보여줘", ("sprocess",), contextual=False)
    for company in top_counted_values(profile, "top_company_names", limit=50, min_count=2):
        add(scenarios, "autoresearch-process-company", f"{company} 회사 프로세스 중 평균 부하지수가 높은 순서로 보여줘", ("sprocess",), contextual=False)
        add(scenarios, "autoresearch-process-company", f"{company} 회사 프로세스의 CPU 평균 상위 항목을 보여줘", ("sprocess",), contextual=False)
    for product in top_counted_values(profile, "top_product_names", limit=50, min_count=2):
        add(scenarios, "autoresearch-process-product", f"{product} 제품 프로세스의 평균 CPU와 메모리를 보여줘", ("sprocess",), contextual=False)
        add(scenarios, "autoresearch-process-product", f"{product} 제품 프로세스 부하지수 분포를 보여줘", ("sprocess",), contextual=False)
    for signer in top_counted_values(profile, "top_signers", limit=40, min_count=2):
        add(scenarios, "autoresearch-process-signer", f"{signer} 서명 프로세스 중 평균 IO가 높은 항목을 보여줘", ("sprocess",), contextual=False)
    generic = [
        "최근 프로세스 평균 부하지수 상위 20개를 보여줘",
        "평균 CPU가 높은 프로세스를 회사명 제품명 기준으로 보여줘",
        "평균 메모리를 많이 쓰는 프로세스를 장비별로 보여줘",
        "평균 IO가 높은 프로세스를 경로별로 보여줘",
        "측정 횟수가 많은 프로세스를 평균 부하지수와 함께 보여줘",
        "서명 없는 프로세스 중 평균 부하지수가 높은 항목을 보여줘",
        "crash가 발생한 프로세스를 회사명과 파일버전 기준으로 보여줘",
    ]
    for question in generic:
        add(scenarios, "autoresearch-process-generic", question, ("sprocess",), contextual=False)
    return scenarios


def node_profile_scenarios(profile: dict[str, Any]) -> list[Scenario]:
    scenarios: list[Scenario] = []
    for user in top_counted_values(profile, "top_users", limit=60, min_count=1):
        add(scenarios, "autoresearch-node-user", f"{user}의 모든 장비 상태와 사양을 보여줘", ("node",), contextual=False)
        add(scenarios, "autoresearch-node-user", f"{user}은 장비 몇개를 가지고 있지?", ("node",), contextual=False)
        add(scenarios, "autoresearch-node-user", f"{user} 장비의 현재 CPU 메모리 사용률을 보여줘", ("node",), contextual=False)
    for manufacturer in top_counted_values(profile, "top_manufacturers", limit=40, min_count=1):
        add(scenarios, "autoresearch-node-hardware", f"{manufacturer} 제조사 장비를 사용자별로 보여줘", ("node",), contextual=False)
        add(scenarios, "autoresearch-node-hardware", f"{manufacturer} 제조사 장비의 OS 버전 분포를 보여줘", ("node",), contextual=False)
    for model in top_counted_values(profile, "top_models", limit=50, min_count=1):
        add(scenarios, "autoresearch-node-hardware", f"{model} 모델 장비를 사용자별로 보여줘", ("node",), contextual=False)
    for os_name in top_counted_values(profile, "top_os_names", limit=40, min_count=1):
        add(scenarios, "autoresearch-node-os", f"{os_name} 장비의 현재 상태와 에이전트 버전을 보여줘", ("node",), contextual=False)
    generic = [
        "현재 온라인 장비와 오프라인 장비 수를 보여줘",
        "현재 CPU 사용률이 높은 장비를 사용자별로 보여줘",
        "현재 메모리 사용률이 높은 장비를 사용자별로 보여줘",
        "물리적인 메모리 크기가 제일 큰 장비를 보여줘",
        "현재 health 값이 높은 장비를 사용자별로 보여줘",
        "에이전트 버전별 장비 수를 보여줘",
    ]
    for question in generic:
        add(scenarios, "autoresearch-node-generic", question, ("node",), contextual=False)
    return scenarios


def system_profile_scenarios(profile: dict[str, Any]) -> list[Scenario]:
    scenarios: list[Scenario] = []
    generic = [
        "최근 회사 전체 CPU 사용률 추이를 보여줘",
        "최근 회사 전체 메모리 사용률 추이를 보여줘",
        "최근 회사 전체 IO 사용량 추이를 보여줘",
        "최근 회사 전체 handle 사용량 추이를 보여줘",
        "최근 회사 전체 프로세스 crash 추이를 보여줘",
        "최근 회사 전체 탐지 건수 추이를 보여줘",
        "최근 회사 전체 설치 이벤트 추이를 보여줘",
        "CPU high가 높은 시간대를 보여줘",
        "메모리 high가 높은 시간대를 보여줘",
        "IO high가 높은 시간대를 보여줘",
    ]
    for question in generic:
        add(scenarios, "autoresearch-system-generic", question, ("system",), contextual=False)
    return scenarios


def profile_scenarios(base_url: str, timeout: float, seed: int, count: int) -> list[Scenario]:
    profiles = latest_profiles(base_url, timeout)
    scenarios: list[Scenario] = []
    if "nodeinfo" in profiles:
        scenarios.extend(nodeinfo_profile_scenarios(profiles["nodeinfo"]))
    if "filelist" in profiles:
        scenarios.extend(filelist_profile_scenarios(profiles["filelist"]))
    if "sprocess" in profiles:
        scenarios.extend(sprocess_profile_scenarios(profiles["sprocess"]))
    if "node" in profiles:
        scenarios.extend(node_profile_scenarios(profiles["node"]))
    if "system" in profiles:
        scenarios.extend(system_profile_scenarios(profiles["system"]))

    seen: set[str] = set()
    unique: list[Scenario] = []
    for scenario in scenarios:
        key = re.sub(r"\s+", " ", scenario.question.strip()).lower()
        if key in seen:
            continue
        seen.add(key)
        unique.append(scenario)

    rng = random.Random(seed)
    rng.shuffle(unique)
    return unique[:count] if count > 0 else unique
