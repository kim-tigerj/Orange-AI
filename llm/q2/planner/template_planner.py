from __future__ import annotations

import re
from typing import Optional

from .models import FilterSpec, Metric, QueryPlan, SortSpec, TimeRange


PUBLISHER_PRODUCT_VERSION_TERMS = {
    "voidtools",
    "node.js foundation",
    "igor pavlov",
    "martin prikryl",
    "nullsoft and contributors",
    "the git development community",
    "notepad++ team",
    "hancom",
    "logitech",
    "interezen",
    "교보문고",
    "코스콤",
}


def relative_range(question: str, timezone: str) -> TimeRange:
    if "오늘" in question:
        return TimeRange(type="relative", hours=24, timezone=timezone)
    match = re.search(r"최근\s*(\d+)\s*일|지난\s*(\d+)\s*일", question)
    if match:
        days = int(next(value for value in match.groups() if value))
        return TimeRange(type="relative", days=days, timezone=timezone)
    if "일주일" in question or "7일" in question:
        return TimeRange(type="relative", days=7, timezone=timezone)
    if "3일" in question:
        return TimeRange(type="relative", days=3, timezone=timezone)
    year_match = re.search(r"최근\s*(\d+)\s*년|지난\s*(\d+)\s*년", question)
    if year_match:
        years = int(next(value for value in year_match.groups() if value))
        return TimeRange(type="relative", days=max(1, years) * 365, timezone=timezone)
    if "1년" in question or "일년" in question:
        return TimeRange(type="relative", days=365, timezone=timezone)
    return TimeRange(type="relative", days=7, timezone=timezone)


def has_explicit_time_range(question: str) -> bool:
    return bool(re.search(r"오늘|최근|지난|일주일|\d+\s*일|\d+\s*시간|\d+\s*개월|\d+\s*년", question))


def inventory_time_range(question: str, timezone: str, default_days: int = 365) -> TimeRange:
    if has_explicit_time_range(question):
        return relative_range(question, timezone)
    return TimeRange(type="relative", days=default_days, timezone=timezone)


def has_os_term(question: str) -> bool:
    q_lower = question.lower()
    return (
        "윈도" in question
        or "windows" in q_lower
        or "운영체제" in question
        or bool(re.search(r"(?<![a-z0-9])(?:os|win(?:dows)?\s*1[01])(?![a-z0-9])", q_lower))
    )


def has_node_inventory_process_constraint(question: str) -> bool:
    """True when a process question is constrained by node inventory attributes."""
    q_lower = question.lower()
    return (
        "윈도 10" in question
        or "윈도우 10" in question
        or "windows 10" in q_lower
        or "윈도 11" in question
        or "윈도우 11" in question
        or "windows 11" in q_lower
        or "운영체제" in question
        or re.search(r"(?<![a-z0-9])os(?![a-z0-9])", q_lower) is not None
        or "cpu 코어" in q_lower
        or "코어" in question
    )


def requested_limit(question: str, default: int = 100) -> int:
    match = re.search(r"상위\s*(\d+)\s*개|top\s*(\d+)", question, flags=re.IGNORECASE)
    if not match:
        return default
    value = int(next(value for value in match.groups() if value))
    return max(1, min(value, 1000))


def version_group_fields(question: str) -> list[str]:
    q_lower = question.lower()
    fields: list[str] = []
    if "회사" in question or "company" in q_lower:
        fields.append("CompanyName")
    if "제품" in question or "product" in q_lower:
        fields.append("ProductName")
    if "제품버전" in question or "제품 버전" in question or "productversion" in q_lower:
        fields.append("ProductVersion")
    if "설명" in question or "파일설명" in question or "filedescription" in q_lower:
        fields.append("FileDescription")
    if "파일버전" in question or "파일 버전" in question or "fileversion" in q_lower or "버전" in question:
        fields.append("FileVersion")
    if "파일명" in question or "파일 이름" in question or "filename" in q_lower:
        fields.append("FileName")
    if not fields:
        fields = ["CompanyName", "ProductName", "ProductVersion", "FileDescription", "FileVersion"]
    return fields


def requested_filelist_group_fields(question: str) -> list[str]:
    q_lower = question.lower()
    aliases = [
        ("IsSystem", ("issystem", "시스템 값", "시스템값")),
        ("Codesign", ("codesign", "코드서명")),
        ("Signer", ("signer", "서명자")),
        ("HostUrl", ("hosturl", "다운로드 출처", "출처 url")),
        ("ReferrerUrl", ("referrerurl", "참조 url")),
        ("ProductVersion", ("productversion", "제품 버전", "제품버전")),
        ("ProductName", ("productname", "제품명", "제품 이름")),
        ("CompanyName", ("companyname", "회사명", "회사")),
        ("FileVersion", ("fileversion", "파일 버전", "파일버전")),
        ("FileName", ("filename", "파일명", "파일 이름")),
        ("FilePath", ("filepath", "파일 경로", "경로")),
        ("ZoneId", ("zoneid",)),
    ]
    fields: list[str] = []
    for field, names in aliases:
        if any(name in q_lower for name in names):
            fields.append(field)
    return fields


NODEINFO_SECTION_FIELDS = {
    "AGENT": {
        "build": "data.build",
        "path": "data.Path",
        "deviceguid": "data.DeviceGuid",
        "deviceid": "data.DeviceId",
        "deviceticket": "data.DeviceTicket",
        "totalcounters": "data.TotalCounters.CounterCount",
        "countercount": "data.TotalCounters.CounterCount",
    },
    "MEMORY": {
        "capacity": "data.Capacity",
        "speed": "data.Speed",
        "manufacturer": "data.Manufacturer",
        "partnumber": "data.PartNumber",
        "banklabel": "data.BankLabel",
        "devicelocator": "data.DeviceLocator",
        "status": "data.Status",
    },
    "PRINTER": {
        "status": "data.Status",
        "drivername": "data.DriverName",
        "name": "data.Name",
        "portname": "data.PortName",
        "default": "data.Default",
    },
    "VACCINE": {
        "displayname": "data.displayName",
        "status": "data.Status",
        "signature": "data.Signature",
        "owner": "data.Owner",
        "countercount": "data.TotalCounters.CounterCount",
    },
    "UPDATE": {
        "hotfixid": "data.HotFixID",
        "caption": "data.Caption",
        "installedon": "data.InstalledOn",
        "installedby": "data.InstalledBy",
        "status": "data.Status",
    },
}


def explicit_nodeinfo_section(question: str) -> Optional[str]:
    q_upper = question.upper()
    for section in NODEINFO_SECTION_FIELDS:
        if section in q_upper and ("인벤토리" in question or "inventory" in question.lower()):
            return section
    return None


def requested_nodeinfo_section_fields(question: str, section: str) -> list[str]:
    q_lower = question.lower()
    fields: list[str] = []
    for token, field in NODEINFO_SECTION_FIELDS.get(section, {}).items():
        if re.search(rf"(?<![a-z0-9]){re.escape(token)}(?![a-z0-9])", q_lower) and field not in fields:
            fields.append(field)
    return fields


def installed_program_term(question: str) -> Optional[str]:
    q = question.strip()
    product_chars = r"A-Za-z가-힣0-9_.+()/\-"
    generic_term = re.compile(
        r"(?:"
        r"프로그램\s*(?:목록|현황|리스트|게시자|버전|제품명|많은|적은)|"
        r"특정\s*프로그램|"
        r"설치\s*프로그램|"
        r"소프트웨어\s*(?:목록|현황|리스트|게시자|버전|제품명)|"
        r"제품명|동일\s*(?:프로그램|제품|소프트웨어)|제거\s*명령|설치\s*원본|설치\s*경로|설치일|게시자|회사명|"
        r"노드별|장비별|사용자별"
        r")",
        flags=re.IGNORECASE,
    )
    patterns = [
        rf"([{product_chars} ][{product_chars} ]*?)\s*설치\s*항목",
        rf"([{product_chars} ][{product_chars} ]*?)\s*설치\s*버전",
        rf"([{product_chars} ][{product_chars} ]*?)\s*설치\s*(?:노드|장비|PC|pc|컴퓨터|사용자)",
        rf"([{product_chars} ][{product_chars} ]*?)\s*설치\s*(?:경로|위치|키|원본|소스|일)",
        rf"([{product_chars} ][{product_chars} ]*?)\s*(?:제거|삭제)\s*명령",
        rf"([{product_chars} ][{product_chars} ]*?)\s*(?:그래픽\s*)?드라이버\s*(?:설치|깔)",
        rf"([{product_chars} ][{product_chars} ]*?)\s*(?:프로그램|제품|소프트웨어)?\s*(?:설치한|설치된|깔린)",
        rf"(?:설치한|설치된|깔린)\s*([{product_chars} ][{product_chars} ]*)",
    ]
    stop_words = {"node", "nodes", "endpoint"}
    for pattern in patterns:
        match = re.search(pattern, q, flags=re.IGNORECASE)
        if not match:
            continue
        term = match.group(1).strip(" ,_-")
        if not term:
            continue
        pieces = [piece for piece in re.split(r"\s+", term) if piece.lower() not in stop_words]
        term = " ".join(pieces).strip()
        term = re.sub(r"\b(?:최근|새로|신규|구버전|최신\s*버전|최신버전|오래된|낡은)\b", "", term, flags=re.IGNORECASE).strip()
        term = re.sub(r"(?:은|는|이|가|을|를)$", "", term).strip()
        term = re.sub(r"\b(?:최근|새로|신규|구버전|최신\s*버전|최신버전|오래된|낡은)\b", "", term, flags=re.IGNORECASE).strip()
        term = normalize_installed_program_search_term(term)
        if generic_term.search(term):
            continue
        if re.search(r"프로그램(?:이|가)?\s*(?:가장\s*)?(?:많|적|목록|현황|리스트|찾)", term):
            continue
        if re.match(
            r"^(?:제품명|버전|동일\s*(?:프로그램|제품|소프트웨어)|제거\s*명령|설치\s*원본|설치\s*경로|설치일|게시자|회사명|노드|장비|사용자)(?:\s|과|가|이|을|를|$)",
            term,
            flags=re.IGNORECASE,
        ):
            continue
        if re.match(r"^(?:프로그램|소프트웨어|제품)?\s*(?:목록|현황|리스트|많은|적은|전체)?(?:을|를)?(?:\s|$)", term):
            continue
        if term:
            return term
    return None


def normalize_installed_program_search_term(term: str) -> str:
    """Remove Korean inventory descriptors accidentally captured as product names."""
    cleaned = term.strip(" ,_-")
    descriptor_pattern = re.compile(
        r"(?:\s*(?:제품|프로그램|소프트웨어|설치\s*항목)?\s*(?:버전별|버전|게시자별|게시자|제품명별|제품명|상태별|상태|현황|목록|리스트))+$",
        flags=re.IGNORECASE,
    )
    previous = None
    while cleaned and cleaned != previous:
        previous = cleaned
        cleaned = descriptor_pattern.sub("", cleaned).strip(" ,_-")
    return cleaned


def installed_program_vendor_alias(term: str) -> Optional[str]:
    cleaned = normalize_installed_program_search_term(term).lower()
    exact_aliases = {
        "안랩": "AhnLab|안랩",
        "ahnlab": "AhnLab|안랩",
        "v3": "AhnLab|안랩|V3",
    }
    if cleaned in exact_aliases:
        return exact_aliases[cleaned]
    vendor_terms = {
        "알약",
        "alyac",
        "nprotect",
        "inisafe",
        "magicline",
        "touchen",
        "한컴",
        "한컴오피스",
    }
    if cleaned in vendor_terms:
        return product_company_term(cleaned)
    return None


def installed_program_regex_term(question: str) -> Optional[str]:
    term = installed_program_term(question)
    if term:
        alias = installed_program_vendor_alias(term)
        if alias:
            return alias
        known_sequence = regex_for_known_installed_product_sequence(term)
        if known_sequence:
            return known_sequence
        if re.search(r"\s*(?:와|과|및|,|/|\band\b|\bor\b)\s*", term, flags=re.IGNORECASE):
            return regex_for_installed_program_names(term)
        return regex_with_flexible_spaces(term)
    windows_update_product = re.search(
        r"Update for (?:Windows \d+\s+for\s+)?x64-based (?:Windows )?Systems\s*\(?(KB\d{4,})\)?",
        question,
        flags=re.IGNORECASE,
    )
    if windows_update_product:
        return rf"Update for (?:Windows \d+\s+for\s+)?x64-based (?:Windows )?Systems.*{windows_update_product.group(1)}"
    return product_company_term(question)


def old_version_installed_program_regex(question: str) -> Optional[str]:
    match = re.search(r"(.+?)\s*구버전", question, flags=re.IGNORECASE)
    if not match:
        return installed_program_regex_term(question)
    term = strip_inventory_prefix(match.group(1))
    term = re.sub(r"(?:의심|후보|설치|프로그램|소프트웨어|제품|장비|노드|사용자)$", "", term, flags=re.IGNORECASE).strip(" ,_-")
    if not term:
        return installed_program_regex_term(question)
    known_sequence = regex_for_known_installed_product_sequence(term)
    if known_sequence:
        return known_sequence
    alias = installed_program_vendor_alias(term)
    if alias:
        return alias
    q_lower = term.lower()
    specific_products = [
        ("edge", "Microsoft Edge|Edge"),
        ("chrome", "Google Chrome|Chrome|크롬"),
        ("onedrive", "Microsoft OneDrive|OneDrive"),
        ("teams", "Microsoft Teams|Teams"),
        ("office", "Microsoft Office|Microsoft 365|Office"),
        ("firefox", "Mozilla Firefox|Firefox"),
        ("adobe", "Adobe|Acrobat|Reader"),
        ("zoom", "Zoom"),
        ("java", "Java|Oracle|OpenJDK"),
        (".net", r"\.NET|Microsoft \.NET|DotNet"),
        ("한컴", "한컴|Hancom|Hnc|Hwp"),
    ]
    for key, value in specific_products:
        if key in q_lower:
            return value
    return product_company_term(term) or regex_with_flexible_spaces(term)


def regex_with_flexible_spaces(term: str) -> str:
    if re.match(r"^Microsoft\s+Visual\s+C\+\+\s+Redistributable$", term.strip(), flags=re.IGNORECASE):
        return r"Microsoft\s+Visual\s+C\+\+(?:\s+\d{4}(?:-\d{4})?)?\s+(?:(?:x86|x64|32-bit|64-bit)\s+)?Redistributable"
    visual_cpp = re.match(
        r"^(Microsoft\s+Visual\s+C\+\+\s+\d{4})\s+Redistributable$",
        term.strip(),
        flags=re.IGNORECASE,
    )
    if visual_cpp:
        return rf"{regex_with_flexible_spaces(visual_cpp.group(1))}\s+(?:(?:x86|x64|32-bit|64-bit)\s+)?Redistributable"
    parts = re.split(r"\s+", term.strip())
    return r"\s+".join(re.escape(part) for part in parts if part)


def regex_for_installed_program_names(term: str) -> str:
    known_sequence = regex_for_known_installed_product_sequence(term)
    if known_sequence:
        return known_sequence
    parts = [
        part.strip(" ,_-")
        for part in re.split(r"\s*(?:와|과|및|,|/|\band\b|\bor\b)\s*", term.strip(), flags=re.IGNORECASE)
        if part.strip(" ,_-")
    ]
    if len(parts) <= 1:
        return regex_with_flexible_spaces(term)
    return "|".join(f"(?:{regex_with_flexible_spaces(part)})" for part in parts)


def regex_for_known_installed_product_sequence(term: str) -> Optional[str]:
    """Split adjacent well-known product names in terse admin inventory questions."""
    q_lower = term.lower()
    known_products = [
        ("edge", "Microsoft Edge|Edge"),
        ("chrome", "Google Chrome|Chrome|크롬"),
        ("onedrive", "Microsoft OneDrive|OneDrive"),
        ("teams", "Microsoft Teams|Teams"),
        ("office", "Microsoft Office|Microsoft 365|Office"),
        ("firefox", "Mozilla Firefox|Firefox"),
        ("adobe", "Adobe|Acrobat|Reader"),
        ("zoom", "Zoom"),
        ("java", "Java|Oracle|OpenJDK"),
        (".net", r"\.NET|Microsoft \.NET|DotNet"),
        ("한컴", "한컴|Hancom|Hnc|Hwp"),
    ]
    found = [regex for key, regex in known_products if key in q_lower]
    if len(found) <= 1:
        return None
    return "|".join(f"(?:{regex})" for regex in dict.fromkeys(found))


def regex_with_flexible_name_separators(term: str) -> str:
    parts = [part for part in re.split(r"[\s\\/]+", term.strip()) if part]
    return r"[\s\\/]+".join(re.escape(part) for part in parts)


def strip_inventory_prefix(term: str) -> str:
    return re.sub(
        r"^(?:인벤토리\s*기준(?:으로)?|inventory\s*basis|inventory)\s*",
        "",
        term.strip(),
        flags=re.IGNORECASE,
    ).strip(" ,_-")


def strip_file_inventory_prefix(term: str) -> str:
    return re.sub(
        r"^(?:파일\s*인벤토리\s*(?:에서|의|를|을)?|file\s*inventory(?:\s+from|\s+in)?)\s*",
        "",
        strip_inventory_prefix(term),
        flags=re.IGNORECASE,
    ).strip(" ,_-")


def looks_like_publisher_name(term: str) -> bool:
    term_lower = term.strip().lower()
    return term_lower in PUBLISHER_PRODUCT_VERSION_TERMS or bool(
        re.search(
            r"(?:\.[a-z]{2,})(?:\s|$)|\b(?:inc\.?|corp\.?|corporation|co\.?|company|ltd\.?|llc|gmbh|pte\.?|limited|foundation|team|contributors)\b",
            term,
            flags=re.IGNORECASE,
        )
    )


def split_installed_program_name_version(term: str) -> tuple[str, str | None]:
    """Split product names that embed a trailing version into name/version filters."""
    cleaned = term.strip()
    version_pattern = r"\d+(?:[\.,]\d+){1,4}"
    if re.search(
        rf"\bvisual\s+c\+\+.*\bredistributable\b.*\b(?:x86|x64|32-bit|64-bit)\b.*\b{version_pattern}$",
        cleaned,
        flags=re.IGNORECASE,
    ):
        return cleaned, None
    if re.search(rf"\s+-\s+Windows\s+{version_pattern}$", cleaned, flags=re.IGNORECASE):
        return cleaned, None
    match = re.match(rf"^(.+?)\s+-\s+({version_pattern})$", cleaned)
    if match:
        return match.group(1).strip(), match.group(2)
    match = re.match(rf"^(.+?\bRedistributable(?:\s*\([^)]*\))?)\s+({version_pattern})$", cleaned, flags=re.IGNORECASE)
    if match:
        return match.group(1).strip(), match.group(2)
    match = re.match(rf"^(.+?)\s+({version_pattern})$", cleaned)
    if match:
        return match.group(1).strip(), match.group(2)
    return cleaned, None


def installed_program_version_regex(version: str) -> str:
    return r"[\.,]".join(re.escape(part) for part in re.split(r"[\.,]", version) if part)


def manufacturer_term(question: str) -> Optional[str]:
    terms = {
        "삼성": "Samsung|삼성",
        "samsung": "Samsung|삼성",
        "애플": "Apple|애플",
        "apple": "Apple|애플",
        "델": "Dell|델",
        "dell": "Dell|델",
        "레노버": "Lenovo|레노버",
        "lenovo": "Lenovo|레노버",
        "hp": "HP|Hewlett",
        "엘지": "LG|엘지",
        "lg": "LG|엘지",
    }
    q_lower = question.lower()
    for key, value in terms.items():
        if key in {"hp", "lg"}:
            if re.search(rf"(?<![a-z0-9]){re.escape(key)}(?![a-z0-9])", q_lower):
                return value
            continue
        if key in q_lower:
            return value
    match = re.search(r"([A-Za-z가-힣0-9_.,+ -]+?)\s*제조사\s*(?:장비|pc|노트북|데스크탑|컴퓨터)", question, flags=re.IGNORECASE)
    if match:
        term = match.group(1).strip(" ,_-")
        if term and term not in {"전체", "장비별", "노드별"}:
            return re.escape(term)
    match = re.search(r"([A-Za-z가-힣0-9_.,+ -]+?)\s*(?:장비|pc|노트북|데스크탑|컴퓨터)\s*(?:가진|찾|보여)", question, flags=re.IGNORECASE)
    if match:
        return match.group(1).strip()
    return None


def person_name_for_node_count(question: str) -> Optional[str]:
    if re.search(
        r"설치|버전|HotFix|핫픽스|\bKB\d{4,}\b|업데이트|패치|어댑터|프린터|드라이버|그래픽|"
        r"백신|보안\s*제품|소프트웨어|프로그램|인벤토리|디스크|드라이브|파일\s*시스템|파일시스템|"
        r"온라인|오프라인|현재\s*상태|상태별|빌드|build",
        question,
        flags=re.IGNORECASE,
    ):
        return None
    match = re.search(
        r"([A-Za-z가-힣0-9_.-]{2,32})(?:은|는|이|가|의)?\s*(?:장비|노드|pc|컴퓨터)\s*(?:몇\s*개|몇개|몇\s*대|몇대|수|개수|대수|얼마나|가지)",
        question,
        flags=re.IGNORECASE,
    )
    if match:
        name = match.group(1)
        name = re.sub(r"(?:은|는|이|가|의)$", "", name)
        if name not in {"우리회사", "우리", "장비", "노드"}:
            return name
    return None


def node_owner_or_device_term(question: str) -> Optional[str]:
    if not (
        ("장비" in question or "노드" in question or "pc" in question.lower() or "컴퓨터" in question)
        and ("상태" in question or "사양" in question or "스펙" in question or "spec" in question.lower())
    ):
        return None
    if re.search(
        r"설치|버전|HotFix|핫픽스|\bKB\d{4,}\b|업데이트|패치|어댑터|프린터|드라이버|그래픽|"
        r"백신|보안\s*제품|소프트웨어|프로그램|인벤토리|디스크|드라이브|파일\s*시스템|파일시스템|"
        r"온라인|오프라인|현재\s*상태|상태별",
        question,
        flags=re.IGNORECASE,
    ):
        return None
    match = re.search(
        r"([A-Za-z가-힣0-9_.-]{2,32})(?:의|은|는|이|가)?\s*(?:모든\s*)?(?:장비|노드|pc|컴퓨터)",
        question,
        flags=re.IGNORECASE,
    )
    if not match:
        return None
    term = re.sub(r"(?:의|은|는|이|가)$", "", match.group(1)).strip()
    if term in {"우리회사", "우리", "전체", "모든", "장비", "노드", "현재", "온라인", "오프라인", "상태"}:
        return None
    return re.escape(term)


def file_metadata_search_term(term: str) -> str:
    mapped = product_company_term(term)
    if mapped and re.search(r"[A-Za-z0-9]\.net\b", term, flags=re.IGNORECASE):
        mapped = None
    return mapped or re.escape(term)


def file_inventory_filters(question: str) -> tuple[list[FilterSpec], str]:
    q = question.strip()
    q_lower = q.lower()
    if ("서명 없는" in q or "서명이 없는" in q or "미서명" in q) and "파일" in q:
        return [
            FilterSpec(field="Signer", op="empty", value=None),
            FilterSpec(field="Codesign", op="empty", value=None),
        ], "any"
    if asks_empty_value(q) and ("제품 버전" in q or "제품버전" in q or "productversion" in q_lower):
        return [FilterSpec(field="ProductVersion", op="empty", value=None)], "all"
    if asks_empty_value(q) and ("파일 버전" in q or "파일버전" in q or "fileversion" in q_lower):
        return [FilterSpec(field="FileVersion", op="empty", value=None)], "all"
    if asks_empty_value(q) and ("회사명" in q or "회사" in q or "companyname" in q_lower):
        return [FilterSpec(field="CompanyName", op="empty", value=None)], "all"
    if asks_empty_value(q) and ("제품명" in q or "제품 이름" in q or "productname" in q_lower):
        return [FilterSpec(field="ProductName", op="empty", value=None)], "all"
    match = re.search(r"(?:파일\s*인벤토리\s*(?:에서|의)?\s*)?(.+?)\s*회사\s*파일", q, flags=re.IGNORECASE)
    if match:
        return [FilterSpec(field="CompanyName", op="regex", value=re.escape(strip_file_inventory_prefix(match.group(1))))], "all"
    match = re.search(r"(?:파일\s*인벤토리\s*(?:에서|의)?\s*)?(.+?)\s*제품\s*파일", q, flags=re.IGNORECASE)
    if match:
        term = strip_file_inventory_prefix(match.group(1))
        return [FilterSpec(field="ProductName", op="regex", value=file_metadata_search_term(term))], "all"
    match = re.search(r"(?:파일\s*인벤토리\s*(?:에서|의)?\s*)?(.+?)\s*서명\s*파일", q, flags=re.IGNORECASE)
    if match:
        term = strip_file_inventory_prefix(match.group(1))
        if not term:
            return [], "all"
        value = file_metadata_search_term(term)
        return [
            FilterSpec(field="Signer", op="regex", value=value),
            FilterSpec(field="CompanyName", op="regex", value=value),
        ], "any"
    match = re.search(r"(.+?)\s*파일(?:을|이|은|는)?", q, flags=re.IGNORECASE)
    if not match:
        return [], "all"
    term = strip_file_inventory_prefix(match.group(1).strip(" ,"))
    if not term or term in {"최근", "오늘", "새로", "신규", "시스템", "비시스템"}:
        return [], "all"
    if re.search(r"\.(?:exe|dll|sys|msi|msp|bat|cmd|ps1|com|scr|jar|ocx|drv)$", term, flags=re.IGNORECASE):
        return [FilterSpec(field="FileName", op="regex", value=re.escape(term))], "all"
    value = file_metadata_search_term(term)
    return [
        FilterSpec(field="FileName", op="regex", value=value),
        FilterSpec(field="ProductName", op="regex", value=value),
        FilterSpec(field="CompanyName", op="regex", value=value),
    ], "any"


def process_perf_metrics() -> list[Metric]:
    return [
        Metric(name="sample_count", op="sum", field="CounterCount"),
        Metric(name="pscore_avg", op="avg_by_count", field="pscore"),
        Metric(name="cpu_avg", op="avg_by_count", field="CPU"),
        Metric(name="memory_avg_mb", op="avg_by_count", field="Memory"),
        Metric(name="io_avg_mbps", op="avg_by_count", field="IO"),
        Metric(name="handle_avg", op="avg_by_count", field="Handle"),
    ]


def node_count_metric() -> list[Metric]:
    return [Metric(name="row_count", op="count", field="id")]


def file_count_metrics() -> list[Metric]:
    return [
        Metric(name="file_count", op="count", field="FilePath"),
        Metric(name="install_count", op="sum", field="InstallCount"),
        Metric(name="file_size_max", op="max", field="FileSize"),
    ]


def requested_process_sort_field(question: str, default: str = "pscore_avg") -> str:
    q_lower = question.lower()
    if "cpu" in q_lower or "CPU" in question:
        return "cpu_avg"
    if "메모리" in question or "memory" in q_lower:
        return "memory_avg_mb"
    if re.search(r"(?<![a-z0-9])io(?![a-z0-9])", q_lower) is not None or "IO" in question:
        return "io_avg_mbps"
    if "핸들" in question or re.search(r"(?<![a-z0-9])handle(?![a-z0-9])", q_lower) is not None:
        return "handle_avg"
    return default


def event_count_metrics(prefix: str = "event") -> list[Metric]:
    return [
        Metric(name=f"{prefix}_count", op="sum", field="count"),
        Metric(name="row_count", op="count", field="id"),
        Metric(name="cpu_max", op="max", field="CPU"),
        Metric(name="memory_max", op="max", field="Memory"),
        Metric(name="io_max", op="max", field="IO"),
    ]


def event_collection_from_question(question: str) -> Optional[str]:
    if security_inventory_question(question) and not ("이벤트" in question or "장애" in question or "리포트" in question or "보고서" in question):
        return None
    if "타임라인" in question or "timeline" in question.lower():
        return "timeline"
    if "리포트" in question or "보고서" in question or "원인" in question:
        return "report"
    if "증상" in question or "장애" in question or "이벤트" in question:
        return "detect"
    if "탐지" in question and ("노드" in question or "장비" in question or "사용자" in question or "사람" in question or "제품" in question or "프로세스" in question):
        return "detect"
    return None


def command_search_term(question: str) -> Optional[str]:
    terms = {
        "bitlocker": "BitLocker|암호화",
        "비트락커": "BitLocker|암호화",
        "암호화": "BitLocker|암호화",
        "rdp": "RDP|원격 데스크톱|Remote Desktop",
        "원격 데스크톱": "RDP|원격 데스크톱|Remote Desktop",
        "방화벽": "방화벽|Firewall",
        "firewall": "방화벽|Firewall",
        "크롬": "Chrome|크롬",
        "chrome": "Chrome|크롬",
        "백신": "VACCINE|백신|보안 제품",
        "관리자": "관리자|Administrators",
        "업데이트": "Update|업데이트|WUA",
        "프로세스": "프로세스|Process",
        "파워쉘": "PowerShell|파워쉘",
        "powershell": "PowerShell|파워쉘",
    }
    q_lower = question.lower()
    for key, value in terms.items():
        if key in q_lower:
            return value
    return None


def product_company_term(question: str) -> Optional[str]:
    q_lower = question.lower()
    terms = {
        "안랩": "AhnLab|안랩|V3|ASTx|Ahn",
        "ahnlab": "AhnLab|안랩|V3|ASTx|Ahn",
        "v3": "AhnLab|안랩|V3",
        "알약": "ESTsoft|알약|ALYac",
        "alyac": "ESTsoft|알약|ALYac",
        "nprotect": "nProtect|INCA|잉카",
        "inisafe": "INISAFE|INITECH|이니텍",
        "magicline": "MagicLine|DreamSecurity|드림시큐리티",
        "touchen": "TouchEn|RaonSecure|라온시큐어",
        "마이크로소프트": "Microsoft|마이크로소프트|Windows",
        "microsoft": "Microsoft|마이크로소프트|Windows",
        "office": "Microsoft Office|Microsoft 365|Office",
        "teams": "Microsoft Teams|Teams",
        "edge": "Microsoft Edge|Edge",
        "크롬": "Google|Chrome|크롬",
        "chrome": "Google|Chrome|크롬",
        "firefox": "Mozilla|Firefox",
        "adobe": "Adobe|Acrobat|Reader|Creative Cloud",
        "zoom": "Zoom",
        "java": "Java|Oracle|OpenJDK",
        ".net": "\\.NET|Microsoft .NET|DotNet",
        "한컴": "한컴|Hancom|Hnc|Hwp",
        "한컴오피스": "한컴|Hancom|Hnc|Hwp",
    }
    for key, value in terms.items():
        if key in q_lower:
            return value
    return None


def product_process_term(question: str) -> Optional[str]:
    match = re.search(r"(.+?)\s*제품\s*프로세스", question, flags=re.IGNORECASE)
    if not match:
        return None
    term = strip_inventory_prefix(match.group(1))
    if not term or re.match(r"^(?:전체|프로세스|제품)$", term, flags=re.IGNORECASE):
        return None
    mapped = product_company_term(term)
    if mapped:
        return mapped
    return re.escape(term).replace("®", r"(?:®)?")


def process_company_term(question: str) -> Optional[str]:
    match = re.search(r"(.+?)\s*회사\s*프로세스", question, flags=re.IGNORECASE)
    if not match:
        return None
    term = strip_inventory_prefix(match.group(1))
    if not term or re.match(r"^(?:전체|프로세스|회사)$", term, flags=re.IGNORECASE):
        return None
    mapped = product_company_term(term)
    if mapped:
        return mapped
    return re.escape(term).replace(r"\ ", r"\s+").replace("®", r"(?:®)?")


def security_inventory_product_term(question: str) -> Optional[str]:
    terms = security_inventory_product_terms(question)
    if terms:
        return terms[0]
    return None


def security_inventory_product_terms(question: str) -> list[str]:
    q_lower = question.lower()
    security_terms = {
        "안랩": "AhnLab|안랩|V3|ASTx|Ahn",
        "ahnlab": "AhnLab|안랩|V3|ASTx|Ahn",
        "v3": "AhnLab|안랩|V3",
        "알약": "ESTsoft|알약|ALYac",
        "alyac": "ESTsoft|알약|ALYac",
        "windows defender": "Windows Defender|Defender",
        "defender": "Windows Defender|Defender",
        "symantec": "Symantec Endpoint Protection|Symantec",
    }
    terms: list[str] = []
    for key, value in security_terms.items():
        if key in q_lower:
            terms.append(value)
    return list(dict.fromkeys(terms))


def security_inventory_product_regex(question: str) -> Optional[str]:
    terms = security_inventory_product_terms(question)
    if not terms:
        return None
    return "|".join(f"(?:{term})" for term in terms)


def installed_program_version_inventory_question(question: str) -> bool:
    """True for installed software version requests backed by UNINSTALL data."""
    return (
        "설치" in question
        and "버전" in question
        and ("장비" in question or "노드" in question or "사용자" in question)
        and "백신" not in question
        and "보안 제품" not in question
        and security_inventory_product_term(question) is None
        and "상태" not in question
        and "서명" not in question
    )


def security_inventory_question(question: str) -> bool:
    q_lower = question.lower()
    if installed_program_version_inventory_question(question):
        return False
    return (
        "백신" in question
        or "vaccine" in q_lower
        or "보안 제품" in question
        or security_inventory_product_term(question) is not None
    )


def installed_program_inventory_question(question: str) -> bool:
    q_lower = question.lower()
    software_term = "소프트웨어" in question or "프로그램" in question or "installed program" in q_lower or "software inventory" in q_lower
    return (
        software_term
        and (
            "인벤토리" in question
            or "재고" in question
            or "inventory" in q_lower
            or "제품명" in question
            or "게시자" in question
            or "버전" in question
            or "노드수" in question
            or "노드 수" in question
            or "장비수" in question
            or "장비 수" in question
            or "목록" in question
            or "리스트" in question
            or "현황" in question
        )
    )


def nodeinfo_inventory_status_question(question: str) -> bool:
    q_lower = question.lower()
    return (
        "프린터" in question
        or "드라이버" in question
        or "그래픽" in question
        or "비디오" in question
        or "디스크" in question
        or "드라이브" in question
        or "네트워크" in question
        or "어댑터" in question
        or "에이전트" in question
        or "패치" in question
        or "핫픽스" in question
        or "hotfix" in q_lower
        or "printer" in q_lower
        or "driver" in q_lower
        or "network adapter" in q_lower
    )


def multiple_installed_program_versions_question(question: str) -> bool:
    return bool(
        re.search(
            r"(?:동일|같은)\s*(?:제품|프로그램|소프트웨어).*(?:여러|복수|다중)\s*버전|"
            r"(?:여러|복수|다중)\s*버전.*(?:제품|프로그램|소프트웨어)",
            question,
        )
    )


def asks_fewer_installed_programs(question: str) -> bool:
    return bool(re.search(r"(?:적은|적게|가장\s*적)", question))


def hotfix_inventory_question(question: str) -> bool:
    q_lower = question.lower()
    return (
        "hotfix" in q_lower
        or re.search(r"\bkb\d{4,}\b", question, flags=re.IGNORECASE) is not None
        or "핫픽스" in question
        or "윈도우 업데이트" in question
    )


def os_caption_filter_value(question: str) -> Optional[str]:
    q_lower = question.lower()
    if "윈도 10" in question or "윈도우 10" in question or "windows 10" in q_lower or re.search(r"\bwin\s*10\b", q_lower):
        suffix = ""
        if "home" in q_lower or "홈" in question:
            suffix = ".*Home"
        elif "pro" in q_lower or "프로" in question:
            suffix = ".*Pro"
        return f"Windows 10{suffix}"
    if "윈도 11" in question or "윈도우 11" in question or "windows 11" in q_lower or re.search(r"\bwin\s*11\b", q_lower):
        suffix = ""
        if "home" in q_lower or "홈" in question:
            suffix = ".*Home"
        elif "pro" in q_lower or "프로" in question:
            suffix = ".*Pro"
        return f"Windows 11{suffix}"
    os_match = re.search(
        r"((?:Microsoft\s+)?Windows\s+[A-Za-z0-9가-힣 ._-]+?)\s*(?:장비|노드|PC|pc|컴퓨터)",
        question,
        flags=re.IGNORECASE,
    )
    if os_match:
        return re.escape(os_match.group(1).strip())
    return None


def os_build_filter_value(question: str) -> Optional[str]:
    q_lower = question.lower()
    release_to_build = {
        "1507": "10240",
        "1511": "10586",
        "1607": "14393",
        "1703": "15063",
        "1709": "16299",
        "1803": "17134",
        "1809": "17763",
        "1903": "18362",
        "1909": "18363",
        "2004": "19041",
        "20h2": "19042",
        "21h1": "19043",
        "21h2": "19044",
        "22h2": "19045",
        "23h2": "22631",
        "24h2": "26100",
    }
    release_match = re.search(r"(?:win(?:dows)?\s*1[01]|윈도우?\s*1[01])\s*\(?\s*(\d{4}|2[0-9]h[12])\s*\)?", q_lower)
    if release_match:
        release = release_match.group(1).lower()
        if release in release_to_build:
            return release_to_build[release]
    build_match = re.search(r"(?:OS\s*)?빌드\s*(\d{5})|build\s*(\d{5})", question, flags=re.IGNORECASE)
    if build_match:
        return next(value for value in build_match.groups() if value)
    return None


def os_build_filter_values(question: str) -> list[str]:
    values = re.findall(r"(?:OS\s*)?빌드\s*(\d{5})|build\s*(\d{5})", question, flags=re.IGNORECASE)
    builds = [next(value for value in match if value) for match in values]
    if len(builds) <= 1 and re.search(r"(?:OS\s*)?빌드|build", question, flags=re.IGNORECASE):
        builds = re.findall(r"(?<!\d)\d{5}(?!\d)", question)
    if not builds:
        return []
    return list(dict.fromkeys(builds))


def windows_named_installed_program_question(question: str) -> bool:
    q_lower = question.lower()
    return (
        ("설치" in question or "installed" in q_lower)
        and (
            "windows pc 상태 검사" in q_lower
            or "pc health check" in q_lower
            or "windows sdk addon" in q_lower
            or "windows sdk add-on" in q_lower
        )
    )


def disk_filesystem_question(question: str) -> bool:
    q_lower = question.lower()
    return (
        "파일시스템" in question
        or "파일 시스템" in question
        or "filesystem" in q_lower
    ) and (
        "디스크" in question
        or "드라이브" in question
        or "disk" in q_lower
        or "drive" in q_lower
    )


def explicit_security_inventory_question(question: str) -> bool:
    q_lower = question.lower()
    if installed_program_version_inventory_question(question):
        return False
    return (
        "백신" in question
        or "vaccine" in q_lower
        or "보안 제품" in question
        or "windows defender" in q_lower
        or "defender" in q_lower
        or "symantec endpoint protection" in q_lower
        or (
            security_inventory_product_term(question) is not None
            and ("제품" in question or "설치" in question or "상태" in question or "서명" in question)
        )
    )


def asks_process_metric(question: str) -> bool:
    q_lower = question.lower()
    return (
        "사용량" in question
        or "부하지수" in question
        or re.search(r"(?<![a-z0-9])cpu(?![a-z0-9])", q_lower) is not None
        or "메모리" in question
        or re.search(r"(?<![a-z0-9])io(?![a-z0-9])", q_lower) is not None
        or "핸들" in question
        or re.search(r"(?<![a-z0-9])handle(?![a-z0-9])", q_lower) is not None
    )


def process_executable_term(question: str) -> Optional[str]:
    match = re.search(r"([A-Za-z0-9_.-]+\.exe)\s*프로세스", question, flags=re.IGNORECASE)
    if match:
        return match.group(1)
    match = re.search(r"프로세스\s*([A-Za-z0-9_.-]+\.exe)", question, flags=re.IGNORECASE)
    if match:
        return match.group(1)
    return None


def network_adapter_question(question: str) -> bool:
    q_lower = question.lower()
    return (
        ("네트워크" in question and ("어댑터" in question or "adapter" in q_lower or "mac" in q_lower))
        or "network adapter" in q_lower
        or "netconnection" in q_lower
    )


def network_interface_question(question: str) -> bool:
    q_lower = question.lower()
    return (
        ("ip 주소" in q_lower or "ip주소" in q_lower or "아이피" in question)
        or "mac 주소" in q_lower
        or "mac별" in q_lower
        or "mac 별" in q_lower
        or (
            ("네트워크" in question or "network" in q_lower)
            and (
                "ip" in q_lower
                or "mac" in q_lower
                or "게이트웨이" in question
                or "gateway" in q_lower
                or "subnet" in q_lower
                or "dhcp" in q_lower
                or "best" in q_lower
            )
        )
    )


def network_adapter_term(question: str) -> Optional[str]:
    q = question.strip()
    q_lower = q.lower()
    known_terms = {
        "realtek": "Realtek",
        "intel": "Intel",
        "vmware": "VMware",
        "bluetooth": "Bluetooth",
        "nexg": "NexG",
        "vpn": "VPN",
    }
    found = [value for key, value in known_terms.items() if key in q_lower]
    if found:
        return "|".join(regex_with_flexible_spaces(term) for term in dict.fromkeys(found))
    match = re.search(r"([A-Za-z0-9_.+#()/ -]+?)\s*네트워크\s*어댑터", q, flags=re.IGNORECASE)
    if match:
        term = match.group(1).strip(" ,_-")
        if term and not re.match(r"^(?:전체|장비별|노드별|네트워크)$", term, flags=re.IGNORECASE):
            return regex_for_inventory_alternatives(term)
    return None


def regex_for_inventory_alternatives(term: str) -> str:
    """Build a regex for one or more literal inventory values named in Korean."""
    if re.search(r"\s*(?:와|과|및|,|/|\band\b|\bor\b)\s*", term, flags=re.IGNORECASE):
        return regex_for_installed_program_names(term)
    return regex_with_flexible_inventory_tokens(term)


def regex_with_flexible_inventory_tokens(term: str) -> str:
    parts = re.split(r"\s+", term.strip())
    token_patterns = [
        rf"{re.escape(part)}(?:\s*\((?:R|TM)\))?"
        for part in parts
        if part
    ]
    return r"\s+".join(token_patterns)


def inventory_value_before_label(question: str, label: str) -> Optional[str]:
    match = re.search(rf"(.+?)\s*{label}\s*(?:장비|노드|PC|pc|컴퓨터)", question, flags=re.IGNORECASE)
    if not match:
        return None
    term = match.group(1).strip(" ,_-")
    term = re.sub(r"^(?:인벤토리\s*기준(?:으로)?|inventory\s*basis|inventory)\s*", "", term, flags=re.IGNORECASE).strip(" ,_-")
    if not term or re.match(r"^(?:전체|노드별|장비별|사용자별)$", term, flags=re.IGNORECASE):
        return None
    return regex_for_inventory_alternatives(term)


def memory_manufacturer_regex(question: str) -> Optional[str]:
    if "메모리" not in question and "memory" not in question.lower() and "ram" not in question.lower():
        return None
    terms = {
        "samsung": "Samsung",
        "삼성": "Samsung",
        "vmware virtual ram": "VMware Virtual RAM",
        "vmware": "VMware Virtual RAM",
        "micron technology": "Micron Technology",
        "micron": "Micron",
        "hynix": "Hynix",
        "하이닉스": "Hynix",
        "ramaxel": "Ramaxel Technology",
        "0x80ad": "0x80AD",
    }
    q_lower = question.lower()
    found = []
    for key, value in terms.items():
        if key in q_lower and value not in found:
            found.append(value)
    if not found:
        return None
    return "|".join(regex_with_flexible_spaces(term) for term in found)


def installed_product_version_term(question: str) -> Optional[str]:
    match = re.search(
        r"(.+?)\s*제품(?:을|를)?\s*버전\s*(?:(?:과|와)\s*게시자\s*)?(?:별|분포)",
        question,
        flags=re.IGNORECASE,
    )
    if not match:
        return None
    term = strip_inventory_prefix(match.group(1))
    if not term or term in {"회사", "전체", "설치된", "설치"}:
        return None
    if re.search(r"(?:오늘|최근|지난|일주일|\d+\s*일|\d+\s*시간|\d+\s*개월|\d+\s*년)", term):
        return None
    if re.search(r"(?:업데이트\s*대상|패치\s*대상|구버전|많은|적은|측정|평균|프로세스)", term):
        return None
    return term


def installed_product_version_filter_field(question: str) -> str:
    match = re.search(
        r"(.+?)\s*제품(?:을|를)?\s*버전\s*(?:(?:과|와)\s*게시자\s*)?(?:별|분포)",
        question,
        flags=re.IGNORECASE,
    )
    if not match:
        return "data.name"
    term = strip_inventory_prefix(match.group(1))
    return "data.publisher" if looks_like_publisher_name(term) else "data.name"


def installed_program_metadata_term(question: str) -> Optional[str]:
    if "메타데이터" not in question:
        return None
    if "게시자" not in question and "publisher" not in question.lower():
        return None
    if "버전" not in question and "version" not in question.lower():
        return None
    match = re.search(
        r"(.+?)\s*게시자\s*메타데이터(?:와|과)?\s*버전",
        question,
        flags=re.IGNORECASE,
    )
    if not match:
        return None
    term = strip_inventory_prefix(match.group(1))
    if not term or re.match(r"^(?:전체|설치|프로그램|소프트웨어|제품)$", term, flags=re.IGNORECASE):
        return None
    return term


def installed_publisher_term(question: str) -> Optional[str]:
    match = re.search(r"(.+?)\s*게시자\s*(?:프로그램|제품|소프트웨어|설치\s*항목)", question, flags=re.IGNORECASE)
    if not match:
        return None
    term = strip_inventory_prefix(match.group(1))
    term = re.sub(r"(?:중|에서|의|을|를|이|가|은|는)$", "", term).strip(" ,_-")
    if (
        not term
        or term in {"전체", "설치", "프로그램", "소프트웨어"}
        or re.search(r"설치\s*프로그램|설치된\s*프로그램|소프트웨어|제품명|버전|version|비어|없는|누락|항목", term, flags=re.IGNORECASE)
    ):
        return None
    return re.escape(term)


def agent_version_term(question: str) -> Optional[str]:
    match = re.search(r"\b(\d+(?:\.\d+){2,})\b\s*에이전트\s*버전", question, flags=re.IGNORECASE)
    return re.escape(match.group(1)) if match else None


def asks_empty_value(question: str) -> bool:
    return "비어" in question or "없는" in question or "누락" in question or "empty" in question.lower()


def wants_per_node_rows(question: str) -> bool:
    if "장비 수" in question or "장비수" in question or "노드 수" in question or "노드수" in question:
        return False
    return (
        "사용자" in question
        or "장비별" in question
        or "노드별" in question
        or "장비를" in question
        or "노드를" in question
        or "목록" in question
        or "리스트" in question
        or "찾" in question
    )


def nodeinfo_counter_metrics(prefix: str) -> list[Metric]:
    return [
        Metric(name="node_count", op="count_distinct", field="id"),
        Metric(name="pscore_max", op="max", field=f"data.{prefix}.pscore"),
        Metric(name="cpu_max", op="max", field=f"data.{prefix}.CPU"),
        Metric(name="memory_max", op="max", field=f"data.{prefix}.Memory"),
        Metric(name="io_max", op="max", field=f"data.{prefix}.IO"),
        Metric(name="handle_max", op="max", field=f"data.{prefix}.Handle"),
    ]


def current_node_metrics() -> list[Metric]:
    return [
        Metric(name="memory_rate", op="max", field="System.Memory.rate"),
        Metric(name="memory_mb", op="max", field="System.Memory.value"),
        Metric(name="memory_total_mb", op="max", field="System.Memory.total"),
        Metric(name="cpu_rate", op="max", field="System.CPU.rate"),
        Metric(name="io_mb", op="max", field="System.IO.value"),
        Metric(name="handle_k", op="max", field="System.Handle.value"),
    ]


def asks_physical_memory_size(question: str) -> bool:
    q_lower = question.lower()
    return (
        "물리" in question
        or "총 메모리" in question
        or "전체 메모리" in question
        or "메모리 크기" in question
        or "메모리크기" in question
        or "메모리 용량" in question
        or "메모리용량" in question
        or "ram 크기" in q_lower
        or "ram size" in q_lower
        or "total memory" in q_lower
    )


def asks_memory_used_amount(question: str) -> bool:
    q_lower = question.lower()
    return (
        "사용량" in question
        or "쓴" in question
        or "쓰는" in question
        or "먹" in question
        or "mb" in q_lower
        or "used memory" in q_lower
    )


def plan_from_template(question: str, timezone: str = "Asia/Seoul") -> Optional[QueryPlan]:
    q = question.strip()
    time_range = relative_range(q, timezone)

    version_metadata_question = (
        "버전 정보" in q
        or "파일 버전" in q
        or "파일버전" in q
        or "제품 버전" in q
        or "제품버전" in q
        or "파일 설명" in q
        or "파일설명" in q
        or "fileversion" in q.lower()
        or "productversion" in q.lower()
        or "filedescription" in q.lower()
    )
    os_inventory_field_question = has_os_term(q) and bool(
        re.search(r"\b(?:caption|version|buildnumber|ubr)\b|빌드|버전", q, flags=re.IGNORECASE)
    )

    current_node_question = (
        "우리회사" in q
        or "우리 회사" in q
        or "누구" in q
        or "녀석" in q
        or "놈" in q
        or "사람" in q
        or "장비" in q
        or "노드" in q
        or "pc" in q.lower()
        or "endpoint" in q.lower()
    ) and not ("프로세스" in q or "설치" in q or has_os_term(q))

    filelist_field_group_by = requested_filelist_group_fields(q)
    if (
        "파일" in q
        and "프로세스" not in q
        and filelist_field_group_by
        and (
            "파일 인벤토리" in q
            or "file inventory" in q.lower()
            or "조합별" in q
            or "기준" in q
            or "분포" in q
            or "파일 수" in q
            or "파일수" in q
        )
    ):
        filters: list[FilterSpec] = []
        if re.search(r"(?<![a-z0-9])exe(?:cutable)?(?![a-z0-9])|실행\s*파일|\.exe\b", q, flags=re.IGNORECASE):
            filters.append(FilterSpec(field="FileName", op="regex", value=r"\.exe$"))
        if ("비시스템" in q or "시스템 파일이 아닌" in q) and "IsSystem" not in filelist_field_group_by:
            filters.append(FilterSpec(field="IsSystem", op="eq", value=0))
        elif "시스템 파일" in q and ("만" in q or "중" in q) and "IsSystem" not in filelist_field_group_by:
            filters.append(FilterSpec(field="IsSystem", op="eq", value=1))
        return QueryPlan(
            collection="filelist",
            time_range=inventory_time_range(q, timezone),
            group_by=filelist_field_group_by,
            metrics=file_count_metrics(),
            filters=filters,
            sort=[SortSpec(field="file_count", direction="desc")],
            limit=requested_limit(q, 100),
            output="table",
            notes=["template_planner", "file inventory explicit field distribution"],
        )

    section = explicit_nodeinfo_section(q)
    if section:
        group_by = requested_nodeinfo_section_fields(q, section)
        if not group_by:
            group_by = list(NODEINFO_SECTION_FIELDS.get(section, {}).values())[:3]
        if wants_per_node_rows(q):
            group_by = ["id", "target.ComputerName", "target.UserName", "target.ip", *group_by]
        filters = [FilterSpec(field="name", op="eq", value=section)]
        for field in group_by:
            if field.endswith(".CounterCount") and ("있는" in q or "기록" in q):
                filters.append(FilterSpec(field=field, op="exists", value=True))
                break
        return QueryPlan(
            collection="nodeinfo",
            time_range=inventory_time_range(q, timezone),
            group_by=group_by,
            metrics=[Metric(name="node_count", op="count_distinct", field="id")],
            filters=filters,
            sort=[SortSpec(field="node_count", direction="desc")],
            limit=requested_limit(q, 200),
            output="table",
            notes=["template_planner", f"nodeinfo {section} explicit inventory"],
        )

    restapi_model_term = inventory_value_before_label(q, "모델")
    if restapi_model_term and all(part in restapi_model_term.lower() for part in ["restapi", "test", "model"]):
        return QueryPlan(
            collection="node",
            time_range=time_range,
            group_by=[
                "id",
                "status",
                "System.ComputerName",
                "User.user.name",
                "User.user.role",
                "System.ip",
                "Product",
                "System.Model",
                "System.Manufacturer",
            ],
            metrics=[Metric(name="node_count", op="count_distinct", field="id")],
            filters=[FilterSpec(field="Product", op="regex", value=restapi_model_term)],
            sort=[SortSpec(field="System.ComputerName", direction="asc")],
            limit=requested_limit(q, 100),
            output="table",
            notes=["template_planner", "node product model inventory"],
        )

    disk_model_term = inventory_value_before_label(q, "디스크\\s*모델")
    if disk_model_term:
        group_by = ["data.Model", "data.Caption", "data.Size", "data.Status"]
        if has_os_term(q):
            group_by = [
                "id",
                "target.ComputerName",
                "target.UserName",
                "target.ip",
                "data.Model",
                "node.OSCaption",
                "node.BuildNumber",
                "node.OSArchitecture",
            ]
        if wants_per_node_rows(q):
            group_by = [
                "id",
                "target.ComputerName",
                "target.UserName",
                "target.ip",
                "data.Model",
                "data.Caption",
                "data.Size",
                "data.Status",
            ]
            if has_os_term(q):
                group_by.extend(["node.OSCaption", "node.BuildNumber", "node.OSArchitecture"])
        return QueryPlan(
            collection="nodeinfo",
            time_range=time_range,
            group_by=group_by,
            metrics=[Metric(name="node_count", op="count_distinct", field="id")],
            filters=[
                FilterSpec(field="name", op="eq", value="DISK"),
                FilterSpec(field="data.Model", op="regex", value=disk_model_term),
            ],
            sort=[SortSpec(field="node_count", direction="desc")],
            limit=requested_limit(q, 200),
            output="table",
            notes=["template_planner", "nodeinfo disk model inventory"],
        )

    model_term = inventory_value_before_label(q, "모델")
    if model_term and "cpu 모델" not in q.lower() and "디스크 모델" not in q:
        if re.search(r"disk|ssd|nvme|harddisk|storage|디스크", model_term, flags=re.IGNORECASE):
            group_by = ["data.Model", "data.Caption", "data.Size", "data.Status"]
            if has_os_term(q) or wants_per_node_rows(q):
                group_by = [
                    "id",
                    "target.ComputerName",
                    "target.UserName",
                    "target.ip",
                    "data.Model",
                    "data.Caption",
                    "data.Size",
                    "data.Status",
                ]
                if has_os_term(q):
                    group_by.extend(["node.OSCaption", "node.BuildNumber", "node.OSArchitecture"])
            return QueryPlan(
                collection="nodeinfo",
                time_range=time_range,
                group_by=group_by,
                metrics=[Metric(name="node_count", op="count_distinct", field="id")],
                filters=[
                    FilterSpec(field="name", op="eq", value="DISK"),
                    FilterSpec(field="data.Model", op="regex", value=model_term),
                ],
                sort=[SortSpec(field="node_count", direction="desc")],
                limit=requested_limit(q, 200),
                output="table",
                notes=["template_planner", "nodeinfo disk model inventory"],
            )
        return QueryPlan(
            collection="nodeinfo",
            time_range=time_range,
            group_by=[
                "id",
                "target.ComputerName",
                "target.UserName",
                "target.UserRole",
                "target.ip",
                "data.Manufacturer",
                "data.Model",
            ],
            metrics=[Metric(name="node_count", op="count_distinct", field="id")],
            filters=[
                FilterSpec(field="name", op="eq", value="SYSTEM"),
                FilterSpec(field="data.Model", op="regex", value=model_term),
            ],
            sort=[SortSpec(field="target.ComputerName", direction="asc")],
            limit=requested_limit(q, 200),
            output="table",
            notes=["template_planner", "nodeinfo system model inventory"],
        )

    cpu_model_term = inventory_value_before_label(q, "CPU\\s*모델")
    if cpu_model_term:
        if has_os_term(q):
            return QueryPlan(
                collection="nodeinfo",
                time_range=time_range,
                group_by=[
                    "id",
                    "target.ComputerName",
                    "target.UserName",
                    "target.ip",
                    "data.Name",
                    "node.OSCaption",
                    "node.BuildNumber",
                    "node.OSArchitecture",
                ],
                metrics=[
                    Metric(name="node_count", op="count_distinct", field="id"),
                    Metric(name="core_max", op="max", field="data.NumberOfCores"),
                    Metric(name="logical_processor_max", op="max", field="data.NumberOfLogicalProcessors"),
                ],
                filters=[
                    FilterSpec(field="name", op="eq", value="CPU"),
                    FilterSpec(field="data.Name", op="regex", value=cpu_model_term),
                ],
                sort=[SortSpec(field="target.ComputerName", direction="asc")],
                limit=requested_limit(q, 200),
                output="table",
                notes=["template_planner", "nodeinfo CPU model with OS inventory"],
            )
        group_by = ["data.Name"]
        if wants_per_node_rows(q):
            group_by = ["id", "target.ComputerName", "target.UserName", "target.ip", "data.Name"]
        return QueryPlan(
            collection="nodeinfo",
            time_range=time_range,
            group_by=group_by,
            metrics=[
                Metric(name="node_count", op="count_distinct", field="id"),
                Metric(name="core_max", op="max", field="data.NumberOfCores"),
                Metric(name="logical_processor_max", op="max", field="data.NumberOfLogicalProcessors"),
            ],
            filters=[
                FilterSpec(field="name", op="eq", value="CPU"),
                FilterSpec(field="data.Name", op="regex", value=cpu_model_term),
            ],
            sort=[SortSpec(field="node_count", direction="desc")],
            limit=requested_limit(q, 200),
            output="table",
            notes=["template_planner", "nodeinfo CPU model inventory"],
        )

    video_card_term = inventory_value_before_label(q, "그래픽\\s*카드")
    if video_card_term:
        group_by = ["data.Name"]
        if wants_per_node_rows(q):
            group_by = ["id", "target.ComputerName", "target.UserName", "target.ip", "data.Name"]
        if has_os_term(q):
            if wants_per_node_rows(q):
                group_by.extend(["node.OSCaption", "node.BuildNumber", "node.OSArchitecture"])
            else:
                group_by = ["data.Name", "node.OSCaption", "node.BuildNumber", "node.OSArchitecture"]
            if "ubr" in q.lower():
                group_by.append("node.UBR")
        return QueryPlan(
            collection="nodeinfo",
            time_range=time_range,
            group_by=group_by,
            metrics=[Metric(name="node_count", op="count_distinct", field="id")],
            filters=[
                FilterSpec(field="name", op="eq", value="VIDEOCARD"),
                FilterSpec(field="data.Name", op="regex", value=video_card_term),
            ],
            sort=[SortSpec(field="node_count", direction="desc")],
            limit=requested_limit(q, 200),
            output="table",
            notes=["template_planner", "nodeinfo video card inventory"],
        )

    memory_maker_term = inventory_value_before_label(q, "메모리\\s*제조사")
    if not memory_maker_term and (
        ("메모리" in q or "memory" in q.lower() or "ram" in q.lower())
        and ("제조사" in q or "manufacturer" in q.lower())
    ):
        memory_maker_term = memory_manufacturer_regex(q)
    if memory_maker_term:
        group_by = ["data.Manufacturer"]
        sort_field = "node_count"
        if wants_per_node_rows(q):
            group_by = ["id", "target.ComputerName", "target.UserName", "target.ip", "data.Manufacturer", "data.Capacity"]
            sort_field = "target.ComputerName"
        return QueryPlan(
            collection="nodeinfo",
            time_range=time_range,
            group_by=group_by,
            metrics=[Metric(name="node_count", op="count_distinct", field="id")],
            filters=[
                FilterSpec(field="name", op="eq", value="MEMORY"),
                FilterSpec(field="data.Manufacturer", op="regex", value=memory_maker_term),
            ],
            sort=[SortSpec(field=sort_field, direction="desc" if sort_field == "node_count" else "asc")],
            limit=requested_limit(q, 200),
            output="table",
            notes=["template_planner", "nodeinfo memory manufacturer inventory"],
        )

    memory_value_term = memory_manufacturer_regex(q)
    if (
        memory_value_term
        and ("메모리" in q or "memory" in q.lower() or "ram" in q.lower())
        and (
            "모델" in q
            or "장비 수" in q
            or "장비수" in q
            or "사용자" in q
            or "인벤토리" in q
            or "파트번호" in q
            or "partnumber" in q.lower()
            or "용량" in q
        )
    ):
        group_by = ["data.Manufacturer", "data.PartNumber", "data.Capacity"]
        sort = [SortSpec(field="node_count", direction="desc")]
        if wants_per_node_rows(q):
            group_by = [
                "id",
                "target.ComputerName",
                "target.UserName",
                "target.ip",
                "data.Manufacturer",
                "data.PartNumber",
                "data.Capacity",
            ]
            sort = [SortSpec(field="target.ComputerName", direction="asc")]
        return QueryPlan(
            collection="nodeinfo",
            time_range=time_range,
            group_by=group_by,
            metrics=[Metric(name="node_count", op="count_distinct", field="id")],
            filters=[
                FilterSpec(field="name", op="eq", value="MEMORY"),
                FilterSpec(field="data.Manufacturer", op="regex", value=memory_value_term),
            ],
            sort=sort,
            limit=requested_limit(q, 200),
            output="table",
            notes=["template_planner", "nodeinfo named memory inventory"],
        )

    if (
        "오프라인" in q
        and has_os_term(q)
        and "에이전트" in q
        and "버전" in q
        and ("장비" in q or "노드" in q or "pc" in q.lower())
    ):
        return QueryPlan(
            collection="nodeinfo",
            time_range=time_range,
            group_by=[
                "id",
                "target.status",
                "target.ComputerName",
                "target.UserName",
                "target.UserRole",
                "target.ip",
                "node.OSCaption",
                "node.BuildNumber",
                "node.UBR",
                "node.OSArchitecture",
                "data.data",
                "data.build",
                "data.Path",
            ],
            metrics=[Metric(name="node_count", op="count_distinct", field="id")],
            filters=[
                FilterSpec(field="name", op="eq", value="AGENT"),
                FilterSpec(field="target.status", op="eq", value="offline"),
            ],
            sort=[SortSpec(field="target.ComputerName", direction="asc")],
            limit=requested_limit(q, 200),
            output="table",
            notes=["template_planner", "nodeinfo offline OS build and agent version inventory"],
        )

    if (
        has_os_term(q)
        and ("상태" in q or "에이전트" in q)
        and ("장비" in q or "노드" in q or "pc" in q.lower())
        and not windows_named_installed_program_question(q)
        and not security_inventory_question(q)
        and not (
            "패치" in q
            or re.search(r"\bKB\d{4,}\b", q, flags=re.IGNORECASE)
            or ("설치" in q and ("노드" in q or "장비" in q or "사용자" in q))
        )
    ):
        filters = []
        os_filter = os_caption_filter_value(q)
        if os_filter:
            filters.append(FilterSpec(field="System.name", op="regex", value=os_filter))
        if "온라인" in q:
            filters.append(FilterSpec(field="status", op="eq", value="online"))
        elif "오프라인" in q:
            filters.append(FilterSpec(field="status", op="eq", value="offline"))
        return QueryPlan(
            collection="node",
            time_range=time_range,
            group_by=["id", "status", "System.ComputerName", "User.user.name", "User.user.role", "System.ip", "System.name", "Agent.version"],
            metrics=[Metric(name="node_count", op="count", field="id")],
            filters=filters,
            sort=[SortSpec(field="System.ComputerName", direction="asc")],
            limit=requested_limit(q, 100),
            output="table",
            notes=["template_planner", "current node OS status and agent version"],
        )

    agent_term = agent_version_term(q)
    if agent_term:
        group_by = ["data.data", "data.build", "data.Path"]
        if wants_per_node_rows(q):
            group_by = [
                "id",
                "target.ComputerName",
                "target.UserName",
                "target.ip",
                "data.data",
                "data.build",
                "data.Path",
            ]
        return QueryPlan(
            collection="nodeinfo",
            time_range=time_range,
            group_by=group_by,
            metrics=[Metric(name="node_count", op="count_distinct", field="id")],
            filters=[
                FilterSpec(field="name", op="eq", value="AGENT"),
                FilterSpec(field="data.data", op="regex", value=agent_term),
            ],
            sort=[SortSpec(field="node_count", direction="desc")],
            limit=requested_limit(q, 200),
            output="table",
            notes=["template_planner", "nodeinfo agent version inventory"],
        )

    if "에이전트" in q and "버전" in q:
        group_by = ["data.data", "data.build", "data.Path"]
        if wants_per_node_rows(q):
            group_by = [
                "id",
                "target.ComputerName",
                "target.UserName",
                "target.ip",
                "data.data",
                "data.build",
                "data.Path",
            ]
        return QueryPlan(
            collection="nodeinfo",
            time_range=time_range,
            group_by=group_by,
            metrics=[Metric(name="node_count", op="count_distinct", field="id")],
            filters=[FilterSpec(field="name", op="eq", value="AGENT")],
            sort=[SortSpec(field="node_count", direction="desc")],
            limit=requested_limit(q, 200),
            output="table",
            notes=["template_planner", "nodeinfo agent version inventory"],
        )

    if "에이전트" in q and ("경로" in q or "설치" in q or "빌드" in q or "build" in q.lower()):
        group_by = ["data.Path", "data.data", "data.build"]
        if wants_per_node_rows(q):
            group_by = [
                "id",
                "target.ComputerName",
                "target.UserName",
                "target.ip",
                "data.Path",
                "data.data",
                "data.build",
            ]
        metrics = [Metric(name="node_count", op="count_distinct", field="id")]
        return QueryPlan(
            collection="nodeinfo",
            time_range=time_range,
            group_by=group_by,
            metrics=metrics,
            filters=[FilterSpec(field="name", op="eq", value="AGENT")],
            sort=[SortSpec(field="node_count", direction="desc")],
            limit=requested_limit(q, 200),
            output="table",
            notes=["template_planner", "nodeinfo agent install inventory"],
        )

    publisher_term = installed_publisher_term(q)
    if publisher_term:
        inv_time_range = inventory_time_range(q, timezone)
        group_by = ["data.publisher", "data.name", "data.version"]
        if wants_per_node_rows(q):
            group_by = [
                "id",
                "target.ComputerName",
                "target.UserName",
                "target.ip",
                "data.publisher",
                "data.name",
                "data.version",
                "data.installLocation",
            ]
        return QueryPlan(
            collection="nodeinfo",
            time_range=inv_time_range,
            group_by=group_by,
            metrics=[Metric(name="node_count", op="count_distinct", field="id")],
            filters=[
                FilterSpec(field="name", op="eq", value="UNINSTALL"),
                FilterSpec(field="data.publisher", op="regex", value=publisher_term),
            ],
            sort=[SortSpec(field="node_count", direction="desc")],
            limit=requested_limit(q, 200),
            output="table",
            notes=["template_planner", "installed program publisher inventory"],
        )

    if (
        "구버전" in q
        and old_version_installed_program_regex(q)
        and ("장비" in q or "노드" in q or "사용자" in q or "버전" in q)
        and not security_inventory_question(q)
    ):
        group_by = ["data.name", "data.version", "data.publisher"]
        if wants_per_node_rows(q):
            group_by = [
                "id",
                "target.status",
                "target.ComputerName",
                "target.UserName",
                "target.UserRole",
                "target.ip",
                "data.name",
                "data.version",
                "data.publisher",
            ]
        return QueryPlan(
            collection="nodeinfo",
            time_range=time_range,
            group_by=group_by,
            metrics=[Metric(name="node_count", op="count_distinct", field="id")],
            filters=[
                FilterSpec(field="name", op="eq", value="UNINSTALL"),
                FilterSpec(field="data.name", op="regex", value=old_version_installed_program_regex(q) or ""),
            ],
            sort=[SortSpec(field="data.version", direction="asc")],
            limit=requested_limit(q, 200),
            output="table",
            notes=["template_planner", "installed program old-version inventory"],
        )

    if (
        ("온라인" in q or "오프라인" in q or "상태" in q)
        and ("장비 수" in q or "장비수" in q or "노드 수" in q or "노드수" in q or "몇개" in q or "몇 개" in q)
        and ("장비" in q or "노드" in q or "pc" in q.lower())
        and not security_inventory_question(q)
        and not nodeinfo_inventory_status_question(q)
    ):
        filters = []
        if "온라인" in q and "오프라인" not in q:
            filters.append(FilterSpec(field="status", op="eq", value="online"))
        elif "오프라인" in q and "온라인" not in q:
            filters.append(FilterSpec(field="status", op="eq", value="offline"))
        return QueryPlan(
            collection="node",
            time_range=time_range,
            group_by=["status"],
            metrics=[Metric(name="node_count", op="count_distinct", field="id")],
            filters=filters,
            sort=[SortSpec(field="node_count", direction="desc")],
            limit=requested_limit(q, 100),
            output="table",
            notes=["template_planner", "current node status count"],
        )

    os_builds = os_build_filter_values(q)
    if (
        has_os_term(q)
        and len(os_builds) >= 2
        and ("장비 수" in q or "장비수" in q or "노드 수" in q or "노드수" in q or "비교" in q)
    ):
        return QueryPlan(
            collection="nodeinfo",
            time_range=time_range,
            group_by=["data.Caption", "data.BuildNumber", "data.UBR", "data.OSArchitecture"],
            metrics=[Metric(name="node_count", op="count_distinct", field="id")],
            filters=[
                FilterSpec(field="name", op="eq", value="OS"),
                FilterSpec(field="data.BuildNumber", op="regex", value=rf"^(?:{'|'.join(re.escape(build) for build in os_builds)})$"),
            ],
            sort=[SortSpec(field="data.BuildNumber", direction="asc")],
            limit=requested_limit(q, 200),
            output="table",
            notes=["template_planner", "node OS build comparison"],
        )

    if (
        ("제조사" in q or manufacturer_term(q))
        and ("장비 수" in q or "장비수" in q or "노드 수" in q or "노드수" in q)
        and not ("프린터" in q or "그래픽" in q or "비디오" in q or "에이전트" in q or "프로그램" in q or "게시자" in q)
    ):
        vendor = manufacturer_term(q)
        group_by = ["data.Manufacturer", "data.Model"] if vendor else ["data.Manufacturer"]
        filters = [FilterSpec(field="name", op="eq", value="SYSTEM")]
        if vendor:
            filters.append(FilterSpec(field="data.Manufacturer", op="regex", value=vendor))
        return QueryPlan(
            collection="nodeinfo",
            time_range=time_range,
            group_by=group_by,
            metrics=[Metric(name="node_count", op="count_distinct", field="id")],
            filters=filters,
            sort=[SortSpec(field="node_count", direction="desc")],
            limit=requested_limit(q, 200),
            output="table",
            notes=["template_planner", "nodeinfo system manufacturer count"],
        )

    if (
        "하드웨어" in q
        and "인벤토리" in q
        and ("system" in q.lower() or "제조사" in q or "모델" in q)
    ):
        return QueryPlan(
            collection="nodeinfo",
            time_range=time_range,
            group_by=[
                "data.Manufacturer",
                "data.Model",
                "data.SystemType",
                "data.PCSystemType",
            ],
            metrics=[
                Metric(name="node_count", op="count_distinct", field="id"),
                Metric(name="memory_total_max", op="max", field="data.TotalPhysicalMemory"),
            ],
            filters=[FilterSpec(field="name", op="eq", value="SYSTEM")],
            sort=[SortSpec(field="node_count", direction="desc")],
            limit=requested_limit(q, 200),
            output="table",
            notes=["template_planner", "nodeinfo SYSTEM hardware inventory"],
        )

    if (
        ("system" in q.lower() or "bios" in q.lower() or "baseboard" in q.lower())
        and ("인벤토리" in q or "장비" in q or "노드" in q)
        and not ("프로세스" in q or "프로그램" in q or "설치" in q)
    ):
        group_by = [
            "id",
            "target.ComputerName",
            "target.UserName",
            "target.ip",
            "data.Manufacturer",
            "data.Model",
            "data.BIOS.Manufacturer",
            "data.BIOS.Name",
            "data.BaseBoard.Manufacturer",
            "data.BaseBoard.Product",
            "data.SystemType",
        ]
        metrics = node_count_metric()
        sort = [SortSpec(field="target.ComputerName", direction="asc")]
        if "장비 수" in q or "장비수" in q or "노드 수" in q or "노드수" in q or "조합별" in q:
            group_by = [
                "data.Manufacturer",
                "data.Model",
                "data.BIOS.Manufacturer",
                "data.BaseBoard.Manufacturer",
                "data.BaseBoard.Product",
            ]
            metrics = [Metric(name="node_count", op="count_distinct", field="id")]
            sort = [SortSpec(field="node_count", direction="desc")]
        return QueryPlan(
            collection="nodeinfo",
            time_range=time_range,
            group_by=group_by,
            metrics=metrics,
            filters=[FilterSpec(field="name", op="eq", value="SYSTEM")],
            sort=sort,
            limit=requested_limit(q, 200),
            output="table",
            notes=["template_planner", "nodeinfo SYSTEM BIOS BaseBoard inventory"],
        )

    person_name = person_name_for_node_count(q)
    if person_name:
        return QueryPlan(
            collection="node",
            time_range=time_range,
            group_by=["User.user.name", "User.user.role"],
            metrics=[Metric(name="node_count", op="count_distinct", field="id")],
            filters=[FilterSpec(field="User.user.name", op="regex", value=person_name)],
            sort=[SortSpec(field="node_count", direction="desc")],
            limit=requested_limit(q, 100),
            output="table",
            notes=["template_planner", "node count by person"],
        )

    node_scope_term = node_owner_or_device_term(q)
    if node_scope_term:
        return QueryPlan(
            collection="node",
            time_range=time_range,
            group_by=[
                "id",
                "status",
                "System.ComputerName",
                "User.user.name",
                "User.user.role",
                "System.ip",
                "System.name",
                "System.version",
                "System.Model",
                "System.Manufacturer",
                "Agent.version",
            ],
            metrics=current_node_metrics(),
            filters=[
                FilterSpec(field="User.user.name", op="regex", value=node_scope_term),
                FilterSpec(field="System.ComputerName", op="regex", value=node_scope_term),
                FilterSpec(field="id", op="regex", value=node_scope_term),
            ],
            filter_mode="any",
            sort=[SortSpec(field="System.ComputerName", direction="asc")],
            limit=requested_limit(q, 100),
            output="table",
            notes=["template_planner", "node owner/device status and specs"],
        )

    if "파일" in q and "프로세스" not in q and (
        version_metadata_question
        or "제품 파일" in q
        or "회사 파일" in q
        or "서명 파일" in q
        or "파일을 장비별" in q
        or "파일이 있는" in q
    ):
        filters, filter_mode = file_inventory_filters(q)
        group_by = ["id", "FileName", "FilePath", "CompanyName", "ProductName", "ProductVersion", "FileVersion", "Signer"]
        if asks_empty_value(q) and ("제품명별" in q or "제품명 별" in q):
            group_by = ["ProductName", "CompanyName"]
        elif asks_empty_value(q) and ("회사별" in q or "회사 별" in q or "회사명별" in q):
            group_by = ["CompanyName", "ProductName"]
        elif "분포" in q or "집계" in q or "버전별" in q or "버전 별" in q:
            group_by = ["FileName", "CompanyName", "ProductName", "ProductVersion", "FileVersion"]
        elif "회사" in q and "제품" in q:
            group_by = ["CompanyName", "ProductName", "ProductVersion", "FileVersion"]
        return QueryPlan(
            collection="filelist",
            time_range=inventory_time_range(q, timezone),
            group_by=group_by,
            metrics=file_count_metrics(),
            filters=filters,
            filter_mode=filter_mode,
            sort=[SortSpec(field="file_count", direction="desc")],
            limit=requested_limit(q, 100),
            output="table",
            notes=["template_planner", "file inventory metadata"],
        )

    if "파일" in q and "프로세스" not in q and ("시스템 파일" in q or "비시스템" in q or "issystem" in q.lower()):
        filters: list[FilterSpec] = []
        if "비시스템" in q or "아닌" in q:
            filters.append(FilterSpec(field="IsSystem", op="eq", value=0))
        elif "시스템 파일" in q and ("만" in q or "중" in q):
            filters.append(FilterSpec(field="IsSystem", op="eq", value=1))
        if re.search(r"(?<![a-z0-9])exe(?![a-z0-9])|\.exe\b", q, flags=re.IGNORECASE):
            filters.append(FilterSpec(field="FileName", op="regex", value=r"\.exe$"))
        group_by = ["CompanyName"]
        if "제품" in q:
            group_by.append("ProductName")
        if "버전" in q:
            group_by.extend(["ProductVersion", "FileVersion"])
        return QueryPlan(
            collection="filelist",
            time_range=inventory_time_range(q, timezone),
            group_by=group_by,
            metrics=file_count_metrics(),
            filters=filters,
            sort=[SortSpec(field="file_count", direction="desc")],
            limit=requested_limit(q, 100),
            output="table",
            notes=["template_planner", "file inventory system flag"],
        )

    if ("노드별" in q or "장비별" in q) and ("cpu" in q.lower() or "코어" in q or "프로세서" in q) and "프로세스" not in q:
        sort_direction = "asc" if "낮" in q or "적" in q else "desc"
        return QueryPlan(
            collection="nodeinfo",
            time_range=time_range,
            group_by=["id", "target.ComputerName", "target.UserName", "target.ip", "data.Name"],
            metrics=[
                Metric(name="core_max", op="max", field="data.NumberOfCores"),
                Metric(name="logical_processor_max", op="max", field="data.NumberOfLogicalProcessors"),
                Metric(name="clock_max", op="max", field="data.MaxClockSpeed"),
            ],
            filters=[FilterSpec(field="name", op="eq", value="CPU")],
            sort=[SortSpec(field="core_max", direction=sort_direction)],
            limit=requested_limit(q, 200),
            output="table",
            notes=["template_planner", "node CPU inventory"],
        )

    if ("노드별" in q or "장비별" in q) and ("메모리" in q or "memory" in q.lower() or "ram" in q.lower()) and (
        "슬롯" in q or "용량" in q or "구성" in q
    ):
        return QueryPlan(
            collection="nodeinfo",
            time_range=time_range,
            group_by=[
                "id",
                "target.ComputerName",
                "target.UserName",
                "target.ip",
                "data.BankLabel",
                "data.DeviceLocator",
                "data.Capacity",
                "data.Manufacturer",
            ],
            metrics=node_count_metric(),
            filters=[FilterSpec(field="name", op="eq", value="MEMORY")],
            sort=[SortSpec(field="id", direction="asc")],
            limit=requested_limit(q, 200),
            output="table",
            notes=["template_planner", "node memory inventory"],
        )

    if current_node_question and ("메모리" in q or "memory" in q.lower()) and not ("구성" in q or "장비별" in q):
        if asks_physical_memory_size(q):
            sort_field = "memory_total_mb"
        elif asks_memory_used_amount(q):
            sort_field = "memory_mb"
        else:
            sort_field = "memory_rate"
        return QueryPlan(
            collection="node",
            time_range=time_range,
            group_by=[
                "id",
                "status",
                "System.ComputerName",
                "User.user.name",
                "User.user.role",
                "System.ip",
                "System.name",
            ],
            metrics=current_node_metrics(),
            filters=[],
            sort=[SortSpec(field=sort_field, direction="desc")],
            limit=requested_limit(q, 20),
            output="table",
            notes=["template_planner", "current node memory ranking"],
        )

    if disk_filesystem_question(q):
        filters = [FilterSpec(field="name", op="eq", value="DISKDRIVE")]
        filesystem_match = re.search(r"\b(UDF|CDFS|NTFS|FAT32|FAT|exFAT)\b", q, flags=re.IGNORECASE)
        if filesystem_match:
            filesystem = filesystem_match.group(1).upper()
            if filesystem == "FAT":
                filters.append(FilterSpec(field="data.FileSystem", op="regex", value=r"^FAT"))
            else:
                filters.append(FilterSpec(field="data.FileSystem", op="eq", value=filesystem))
        group_by = [
            "id",
            "target.ComputerName",
            "target.UserName",
            "target.ip",
            "data.Name",
            "data.VolumeName",
            "data.FileSystem",
            "data.DriveType",
            "data.Status",
        ]
        metrics = node_count_metric()
        sort = [SortSpec(field="target.ComputerName", direction="asc")]
        if "분포" in q or "별" in q or "장비 수" in q or "장비수" in q:
            group_by = ["data.FileSystem", "data.DriveType", "data.Status"]
            metrics = [Metric(name="node_count", op="count_distinct", field="id")]
            sort = [SortSpec(field="node_count", direction="desc")]
        return QueryPlan(
            collection="nodeinfo",
            time_range=time_range,
            group_by=group_by,
            metrics=metrics,
            filters=filters,
            sort=sort,
            limit=requested_limit(q, 200),
            output="table",
            notes=["template_planner", "disk filesystem inventory"],
        )

    if "파일" in q and not disk_filesystem_question(q) and "프로세스" not in q and ("새로" in q or "생성" in q or "설치" in q or "목록" in q or "서명" in q or "경로" in q):
        filters: list[FilterSpec] = []
        filter_mode = "all"
        if "서명 없는" in q or "서명없" in q or "미서명" in q:
            filters = [
                FilterSpec(field="Signer", op="empty", value=None),
                FilterSpec(field="Codesign", op="empty", value=None),
            ]
            filter_mode = "any"
        if "비시스템" in q:
            filters.append(FilterSpec(field="IsSystem", op="eq", value=0))
        elif "시스템 파일" in q and ("만" in q or "중" in q):
            filters.append(FilterSpec(field="IsSystem", op="eq", value=1))
        return QueryPlan(
            collection="filelist",
            time_range=inventory_time_range(q, timezone),
            group_by=["id", "FileName", "FilePath", "CompanyName", "ProductName", "FileVersion", "Signer"],
            metrics=file_count_metrics(),
            filters=filters,
            filter_mode=filter_mode,
            sort=[SortSpec(field="file_count", direction="desc")],
            limit=requested_limit(q, 100),
            output="table",
            notes=["template_planner", "file inventory"],
        )

    if network_adapter_question(q):
        filters = [FilterSpec(field="name", op="eq", value="NETWORKADAPTER")]
        term = network_adapter_term(q)
        if term:
            filters.append(FilterSpec(field="data.Name", op="regex", value=term))
        group_by = [
            "id",
            "target.ComputerName",
            "target.UserName",
            "target.ip",
            "data.Name",
            "data.Manufacturer",
            "data.NetConnectionID",
            "data.MACAddress",
            "data.AdapterType",
            "data.NetEnabled",
            "data.Status",
        ]
        metrics = node_count_metric()
        sort = [SortSpec(field="target.ComputerName", direction="asc")]
        if "제조사별" in q or "제조사 별" in q:
            group_by = ["data.Manufacturer", "data.AdapterType"]
            metrics = [Metric(name="node_count", op="count_distinct", field="id")]
            sort = [SortSpec(field="node_count", direction="desc")]
        elif "장비 수" in q or "장비수" in q:
            group_by = ["data.Name", "data.Manufacturer", "data.AdapterType"]
            metrics = [Metric(name="node_count", op="count_distinct", field="id")]
            sort = [SortSpec(field="node_count", direction="desc")]
        return QueryPlan(
            collection="nodeinfo",
            time_range=time_range,
            group_by=group_by,
            metrics=metrics,
            filters=filters,
            sort=sort,
            limit=requested_limit(q, 200),
            output="table",
            notes=["template_planner", "network adapter inventory"],
        )

    if network_interface_question(q):
        group_by = [
            "id",
            "target.ComputerName",
            "target.UserName",
            "target.ip",
            "data.name",
            "data.ip",
            "data.mac",
            "data.gateway",
            "data.subnet",
            "data.dhcp",
            "data.type",
        ]
        metrics = node_count_metric()
        sort = [SortSpec(field="target.ComputerName", direction="asc")]
        if "주소별" in q or "ip별" in q.lower() or "ip 별" in q.lower():
            group_by = ["data.ip", "id", "target.ComputerName", "target.UserName", "target.ip", "data.name", "data.type"]
            metrics = [Metric(name="node_count", op="count_distinct", field="id")]
            sort = [SortSpec(field="data.ip", direction="asc")]
        elif "mac별" in q.lower() or "mac 별" in q.lower():
            group_by = ["data.mac", "id", "target.ComputerName", "target.UserName", "target.ip", "data.name", "data.type"]
            metrics = [Metric(name="node_count", op="count_distinct", field="id")]
            sort = [SortSpec(field="data.mac", direction="asc")]
        elif "조합별" in q or "조합 별" in q or "장비 수" in q or "장비수" in q:
            group_by = ["data.dhcp", "data.gateway", "data.subnet", "data.best", "data.type"]
            metrics = [Metric(name="node_count", op="count_distinct", field="id")]
            sort = [SortSpec(field="node_count", direction="desc")]
        return QueryPlan(
            collection="nodeinfo",
            time_range=time_range,
            group_by=group_by,
            metrics=metrics,
            filters=[FilterSpec(field="name", op="eq", value="NETWORK")],
            sort=sort,
            limit=requested_limit(q, 200),
            output="table",
            notes=["template_planner", "network interface inventory"],
        )

    if (
        "회사 전체" in q
        and "설치" in q
        and "이벤트" in q
        and ("추이" in q or "최근" in q or "시간" in q)
    ):
        group_by = ["hour"] if "시간대" in q or "시간별" in q else ["date"]
        return QueryPlan(
            collection="system",
            time_range=time_range,
            group_by=group_by,
            metrics=[Metric(name="install_event_count", op="sum", field="Stat.install")],
            sort=[SortSpec(field=group_by[0], direction="asc")],
            limit=200,
            output="line_chart",
            notes=["template_planner", "system install event trend"],
        )

    event_collection = event_collection_from_question(q)
    if event_collection:
        if "프로세스" in q and ("kill" in q.lower() or "킬" in q):
            return QueryPlan(
                collection="sprocess",
                time_range=time_range,
                group_by=["id", "ProcName", "ProcPath", "CompanyName", "ProductName", "FileVersion"],
                metrics=[
                    Metric(name="kill_sum", op="sum", field="Kill"),
                    Metric(name="killed_sum", op="sum", field="Killed"),
                    Metric(name="sample_count", op="sum", field="CounterCount"),
                    Metric(name="pscore_avg", op="avg_by_count", field="pscore"),
                ],
                filters=[
                    FilterSpec(field="Kill", op="gt", value=0),
                    FilterSpec(field="Killed", op="gt", value=0),
                ],
                filter_mode="any",
                sort=[SortSpec(field="kill_sum", direction="desc")],
                limit=requested_limit(q, 100),
                output="table",
                notes=["template_planner", "process kill events"],
            )
        filters: list[FilterSpec] = []
        group_by = ["RuleId", "Desc", "Name", "FileName", "System.ComputerName", "System.ip", "User.name"]
        q_lower = q.lower()
        product_term = product_company_term(q)
        if product_term and ("장애" in q or "비중" in q or "원인" in q) and "탐지" not in q:
            event_collection = "report"
            if not has_explicit_time_range(q):
                time_range = TimeRange(type="relative", days=365, timezone=timezone)
        if event_collection == "report" and not has_explicit_time_range(q):
            time_range = TimeRange(type="relative", days=365, timezone=timezone)
        if product_term:
            if "안랩" in q or "ahnlab" in q_lower or "v3" in q_lower:
                filters.append(FilterSpec(field="CompanyName", op="regex", value="AhnLab|안랩"))
            else:
                filters.append(FilterSpec(field="ProductName", op="regex", value=product_term))
            group_by = ["CompanyName", "ProductName", "ProductVersion", "RuleId", "Desc"]
        if "cpu" in q_lower or "CPU" in q:
            filters.append(FilterSpec(field="RuleId", op="regex", value="CPU"))
        if "네트워크" in q or "tcp" in q_lower or "timeout" in q_lower or "시간 초과" in q:
            filters.append(FilterSpec(field="Desc", op="regex", value="네트워크|TCP|시간 초과|TIMEOUT|연결"))
        if "서명" in q or "미서명" in q:
            filters.append(FilterSpec(field="Desc", op="regex", value="서명|NoCodesign|Codesign"))
        if "노드별" in q or "장비별" in q:
            group_by = ["id", "System.ComputerName", "System.ip", "RuleId", "Desc"]
        elif "사용자" in q or "사람" in q:
            group_by = ["User.name", "User.deptName", "RuleId", "Desc"]
        elif "프로세스" in q:
            group_by = ["Name", "FileName", "FilePath", "RuleId", "Desc"]
        return QueryPlan(
            collection=event_collection,
            time_range=time_range,
            group_by=group_by,
            metrics=event_count_metrics(event_collection),
            filters=filters,
            sort=[SortSpec(field=f"{event_collection}_count", direction="desc")],
            limit=requested_limit(q, 100),
            output="table",
            notes=["template_planner", f"{event_collection} event summary"],
        )

    manager_command_question = (
        "명령 템플릿" in q
        or "명령템플릿" in q
        or "실행한 명령" in q
        or "명령 상태" in q
        or "명령 이력" in q
        or "수집 명령" in q
        or "조치 명령" in q
        or "command template" in q.lower()
    )
    if manager_command_question:
        term = command_search_term(q)
        filters: list[FilterSpec] = []
        filter_mode = "all"
        if term:
            filters = [
                FilterSpec(field="title", op="regex", value=term),
                FilterSpec(field="memo", op="regex", value=term),
                FilterSpec(field="command_data.query", op="regex", value=term),
            ]
            filter_mode = "any"
        if "템플릿" in q or "가능" in q or "찾" in q or "추천" in q or "어떤" in q:
            return QueryPlan(
                collection="command_template",
                time_range=time_range,
                group_by=["title", "memo", "command_data.command", "command_data.query", "command_data.keyword", "command_data.chart", "is_broadcast"],
                metrics=[Metric(name="template_count", op="count", field="title")],
                filters=filters,
                filter_mode=filter_mode,
                sort=[SortSpec(field="title", direction="asc")],
                limit=requested_limit(q, 100),
                output="table",
                notes=["template_planner", "command template search"],
            )
        return QueryPlan(
            collection="command",
            time_range=time_range,
            group_by=["title", "status", "target_group_code", "command_data.command", "command_data.keyword", "is_broadcast"],
            metrics=[
                Metric(name="command_count", op="count", field="title"),
                Metric(name="target_node_count_max", op="max", field="target_node_count"),
                Metric(name="executed_count_max", op="max", field="executed_count"),
                Metric(name="success_count_max", op="max", field="success_count"),
            ],
            filters=filters,
            filter_mode=filter_mode,
            sort=[SortSpec(field="command_count", direction="desc")],
            limit=requested_limit(q, 100),
            output="table",
            notes=["template_planner", "command execution summary"],
        )

    if (
        current_node_question
        and ("제조사" in q or "모델" in q or "장비" in q or "pc" in q.lower())
        and not ("프린터" in q or "그래픽" in q or "비디오" in q or "에이전트" in q or "프로그램" in q or "게시자" in q)
    ):
        vendor = manufacturer_term(q)
        if vendor:
            return QueryPlan(
                collection="nodeinfo",
                time_range=time_range,
                group_by=[
                    "id",
                    "target.status",
                    "target.ComputerName",
                    "target.UserName",
                    "target.UserRole",
                    "target.ip",
                    "data.Manufacturer",
                    "data.Model",
                ],
                metrics=[Metric(name="node_count", op="count_distinct", field="id")],
                filters=[
                    FilterSpec(field="name", op="eq", value="SYSTEM"),
                    FilterSpec(field="data.Manufacturer", op="regex", value=vendor),
                ],
                sort=[SortSpec(field="target.ComputerName", direction="asc")],
                limit=requested_limit(q, 100),
                output="table",
                notes=["template_planner", "nodeinfo system manufacturer inventory"],
            )

    product_term = product_company_term(q)
    process_product_term = product_process_term(q)
    process_company_filter_term = process_company_term(q)
    manufacturer_os_distribution = (
        ("제조사" in q or "manufacturer" in q.lower())
        and ("장비" in q or "노드" in q or "pc" in q.lower())
        and has_os_term(q)
        and ("분포" in q or "버전" in q or "별" in q)
    )
    if manufacturer_os_distribution:
        vendor = manufacturer_term(q)
        filters = [FilterSpec(field="System.Manufacturer", op="regex", value=vendor)] if vendor else []
        return QueryPlan(
            collection="node",
            time_range=time_range,
            group_by=["System.Manufacturer", "System.name"],
            metrics=[Metric(name="node_count", op="count_distinct", field="id")],
            filters=filters,
            sort=[SortSpec(field="node_count", direction="desc")],
            limit=requested_limit(q, 100),
            output="table",
            notes=["template_planner", "node manufacturer OS distribution"],
        )

    counter_inventory_question = "설치 항목" in q and (
        "pathcounters" in q.lower() or "filecounters" in q.lower()
    )
    if counter_inventory_question:
        asks_file_counters = "filecounters" in q.lower()
        counter_name = "FileCounters" if asks_file_counters else "PathCounters"
        security_term = security_inventory_product_term(q)
        term = installed_program_regex_term(q)
        filters = [FilterSpec(field="name", op="eq", value="VACCINE" if security_term and asks_file_counters else "UNINSTALL")]
        if security_term and asks_file_counters:
            filters.append(FilterSpec(field="data.displayName", op="regex", value=security_term))
        elif term:
            filters.append(FilterSpec(field="data.name", op="regex", value=term))
        if asks_file_counters:
            filters.append(FilterSpec(field="data.FileCounters.ProcName", op="not_empty", value=None))

        group_by = [
            "id",
            "target.status",
            "target.ComputerName",
            "target.UserName",
            "target.UserRole",
            "target.ip",
        ]
        if asks_file_counters:
            group_by.extend([
                "data.FileCounters.ProcName",
                "data.FileCounters.ProcPath",
            ])
        else:
            group_by.extend([
                "data.name",
                "data.version",
                "data.publisher",
            ])

        return QueryPlan(
            collection="nodeinfo",
            time_range=time_range,
            group_by=group_by,
            metrics=nodeinfo_counter_metrics(counter_name),
            filters=filters,
            sort=[SortSpec(field="pscore_max" if not asks_file_counters else "node_count", direction="desc")],
            limit=requested_limit(q, 200),
            output="table",
            notes=["template_planner", f"nodeinfo {counter_name} inventory counters"],
        )

    product_performance_question = (
        (product_term or process_product_term or process_company_filter_term)
        and asks_process_metric(q)
        and not ("장애" in q or "탐지" in q)
        and not ("설치" in q or "제거" in q or "삭제" in q or "게시자" in q or "원본" in q)
    )
    if product_performance_question:
        process_filter_term = process_company_filter_term or process_product_term or product_term or ""
        group_by = ["ProcName"]
        if process_company_filter_term:
            group_by = ["CompanyName", "ProcName"]
        if "요일" in q:
            group_by = ["weekday", "ProcName"]
        elif "시간대" in q or "시간별" in q:
            group_by = ["hour", "ProcName"]
        elif "제품별" in q:
            group_by = ["ProductName"]
        elif "회사별" in q:
            group_by = ["CompanyName"]
        elif "장비별" in q:
            group_by = ["id", "ProcName"]
        elif "사용자별" in q or "사람별" in q:
            group_by = ["id", "ProcName"]
        metrics = process_perf_metrics()
        sort_field = requested_process_sort_field(q)
        return QueryPlan(
            collection="sprocess",
            time_range=time_range,
            group_by=group_by,
            metrics=metrics,
            filters=[
                FilterSpec(field="CompanyName", op="regex", value=process_filter_term),
                FilterSpec(field="ProductName", op="regex", value=process_filter_term),
                FilterSpec(field="ProcName", op="regex", value=process_filter_term),
            ],
            filter_mode="any",
            sort=[SortSpec(field=sort_field, direction="desc")],
            limit=requested_limit(q, 100),
            limit_per_group=10 if len(group_by) > 1 else None,
            output="table",
            notes=["template_planner", "company process performance" if process_company_filter_term else "product process performance"],
        )

    executable_process_term = process_executable_term(q)
    if executable_process_term:
        group_by = ["ProcName"]
        if "회사" in q or "회사명" in q or "company" in q.lower():
            group_by.append("CompanyName")
        if "제품" in q or "제품명" in q or "product" in q.lower():
            group_by.append("ProductName")
        if "버전" in q or "version" in q.lower():
            group_by.extend(["FileVersion", "ProductVersion"])
        if "설명" in q or "description" in q.lower():
            group_by.append("FileDescription")
        if "경로" in q or "path" in q.lower():
            group_by.append("ProcPath")
        if "명령" in q or "커맨드" in q or "command" in q.lower():
            group_by.append("Command")
        if "장비" in q or "노드" in q or "사용자" in q:
            group_by.insert(0, "id")
        group_by = list(dict.fromkeys(group_by))
        sort_field = requested_process_sort_field(q)
        if "측정" in q or "횟수" in q or "countercount" in q.lower() or "샘플" in q:
            sort_field = "sample_count"
        return QueryPlan(
            collection="sprocess",
            time_range=time_range,
            group_by=group_by,
            metrics=process_perf_metrics(),
            filters=[FilterSpec(field="ProcName", op="regex", value=re.escape(executable_process_term))],
            sort=[SortSpec(field=sort_field, direction="desc")],
            limit=requested_limit(q, 100),
            output="table",
            notes=["template_planner", "process executable metrics and metadata"],
        )

    windows_update_installed_product = bool(
        re.search(r"Update for (?:Windows \d+\s+for\s+)?x64-based (?:Windows )?Systems\s*\(?KB\d{4,}\)?", q, flags=re.IGNORECASE)
    )
    if hotfix_inventory_question(q) and not windows_update_installed_product:
        filters = [FilterSpec(field="name", op="eq", value="UPDATE")]
        kb_match = re.search(r"\b(KB\d{4,})\b", q, flags=re.IGNORECASE)
        if kb_match:
            filters.append(FilterSpec(field="data.HotFixID", op="regex", value=kb_match.group(1)))
        group_by = ["data.HotFixID", "data.Caption", "data.InstalledOn", "data.InstalledBy", "data.Status"]
        if "노드" in q or "장비" in q or "사용자" in q:
            group_by = ["id", "target.ComputerName", "target.UserName", *group_by]
        return QueryPlan(
            collection="nodeinfo",
            time_range=time_range,
            group_by=group_by,
            metrics=[Metric(name="node_count", op="count_distinct", field="id")],
            filters=filters,
            sort=[SortSpec(field="node_count", direction="desc")],
            limit=requested_limit(q, 200),
            output="table",
            notes=["template_planner", "Windows update inventory"],
        )

    user_directory_question = (
        "사용자" in q
        or "임직원" in q
        or "직원" in q
        or "팀장" in q
        or "팀원" in q
    ) and not (
        "User." in q
        or "장비" in q
        or "노드" in q
        or "프로세스" in q
        or "cpu" in q.lower()
        or "메모리" in q
        or "설치" in q
        or "인벤토리" in q
        or has_os_term(q)
        or "제품" in q
        or "프로그램" in q
        or "소프트웨어" in q
        or "버전" in q
        or re.search(r"\b(?:caption|version|buildnumber|ubr)\b", q, flags=re.IGNORECASE) is not None
        or "게시자" in q
        or "온라인" in q
        or "오프라인" in q
        or "상태" in q
        or security_inventory_question(q)
    )
    if user_directory_question:
        filters = []
        if "팀장" in q:
            filters.append(FilterSpec(field="role", op="regex", value="팀장"))
        elif "팀원" in q:
            filters.append(FilterSpec(field="role", op="regex", value="팀원"))
        return QueryPlan(
            collection="user",
            time_range=time_range,
            group_by=["group_code", "role", "name", "emp_key"],
            metrics=[Metric(name="user_count", op="count", field="emp_key")],
            filters=filters,
            sort=[SortSpec(field="group_code", direction="asc")],
            limit=requested_limit(q, 200),
            output="table",
            notes=["template_planner", "user directory"],
        )

    if "그룹" in q or "조직" in q or "팀 목록" in q:
        return QueryPlan(
            collection="group",
            time_range=time_range,
            group_by=["group_code", "name", "parent_code"],
            metrics=[Metric(name="group_count", op="count", field="group_code")],
            sort=[SortSpec(field="group_code", direction="asc")],
            limit=requested_limit(q, 200),
            output="table",
            notes=["template_planner", "group directory"],
        )

    if current_node_question and "health" in q.lower():
        return QueryPlan(
            collection="node",
            time_range=time_range,
            group_by=[
                "id",
                "status",
                "System.ComputerName",
                "User.user.name",
                "User.user.role",
                "System.ip",
                "System.name",
            ],
            metrics=[Metric(name="health_max", op="max", field="health"), *current_node_metrics()],
            filters=[],
            sort=[SortSpec(field="health_max", direction="desc")],
            limit=requested_limit(q, 20),
            output="table",
            notes=["template_planner", "current node health ranking"],
        )

    if current_node_question and ("cpu" in q.lower() or "씨피유" in q or "부하" in q or "로드" in q) and not ("코어" in q or "프로세서" in q):
        return QueryPlan(
            collection="node",
            time_range=time_range,
            group_by=["id", "status", "System.ComputerName", "User.user.name", "User.user.role", "System.ip", "System.name"],
            metrics=current_node_metrics(),
            filters=[],
            sort=[SortSpec(field="cpu_rate", direction="desc")],
            limit=requested_limit(q, 20),
            output="table",
            notes=["template_planner", "current node CPU ranking"],
        )

    if (
        current_node_question
        and ("온라인" in q or "오프라인" in q or "상태" in q)
        and not security_inventory_question(q)
        and not nodeinfo_inventory_status_question(q)
    ):
        filters = []
        if "온라인" in q and "오프라인" not in q:
            filters.append(FilterSpec(field="status", op="eq", value="online"))
        elif "오프라인" in q and "온라인" not in q:
            filters.append(FilterSpec(field="status", op="eq", value="offline"))
        return QueryPlan(
            collection="node",
            time_range=time_range,
            group_by=["status", "System.name", "Agent.version"],
            metrics=[Metric(name="node_count", op="count", field="id")],
            filters=filters,
            sort=[SortSpec(field="node_count", direction="desc")],
            limit=requested_limit(q, 100),
            output="table",
            notes=["template_planner", "current node status"],
        )

    if "프로세스" in q and ("실행된" in q or "실행한" in q or "실행 중" in q) and ("명령줄" in q or "커맨드" in q or "command" in q.lower()):
        proc_match = re.search(r"([A-Za-z0-9_.-]+\.exe)\s*프로세스", q, flags=re.IGNORECASE)
        filters = [FilterSpec(field="ProcName", op="regex", value=re.escape(proc_match.group(1)))] if proc_match else []
        return QueryPlan(
            collection="sprocess",
            time_range=time_range,
            group_by=["id", "ProcName", "Command", "ProcPath", "CompanyName", "ProductName", "FileVersion"],
            metrics=process_perf_metrics(),
            filters=filters,
            sort=[SortSpec(field="sample_count", direction="desc")],
            limit=requested_limit(q, 100),
            output="table",
            notes=["template_planner", "process command line inventory"],
        )

    if (
        ("장비" in q or "노드" in q or has_node_inventory_process_constraint(q))
        and has_node_inventory_process_constraint(q)
        and ("프로세스" in q or "부하" in q or "powershell" in q.lower() or "파워쉘" in q)
    ):
        filters: list[FilterSpec] = []
        group_by = ["id", "node.CSName", "node.OSCaption", "ProcName"]
        if "사용자" in q or "사람" in q:
            group_by.insert(2, "target.UserName")
        if "윈도 10" in q or "windows 10" in q.lower():
            filters.append(FilterSpec(field="node.OSCaption", op="regex", value="Windows 10"))
        elif "윈도 11" in q or "windows 11" in q.lower():
            filters.append(FilterSpec(field="node.OSCaption", op="regex", value="Windows 11"))
        if "powershell" in q.lower() or "파워쉘" in q:
            filters.append(FilterSpec(field="ProcName", op="regex", value="powershell"))
            group_by.append("Command")
        core_match = re.search(r"코어\s*(\d+)\s*(?:이하|미만|보다 낮)", q)
        if core_match:
            op = "lt" if "미만" in core_match.group(0) or "보다 낮" in core_match.group(0) else "lte"
            filters.append(FilterSpec(field="node.Cores", op=op, value=int(core_match.group(1))))
            group_by.insert(3, "node.Cores")
        sort_field = "pscore_avg"
        if "cpu" in q.lower() or "CPU" in q:
            sort_field = "cpu_avg"
        elif "메모리" in q:
            sort_field = "memory_avg_mb"
        elif "io" in q.lower() or "IO" in q:
            sort_field = "io_avg_mbps"
        elif "핸들" in q or "handle" in q.lower():
            sort_field = "handle_avg"
        return QueryPlan(
            collection="sprocess_nodeinfo",
            time_range=time_range,
            group_by=group_by,
            metrics=process_perf_metrics(),
            filters=filters,
            sort=[SortSpec(field=sort_field, direction="desc")],
            limit=requested_limit(q, 100),
            output="table",
            notes=["template_planner", "sprocess joined with nodeinfo"],
        )

    if "프로세스" in q and asks_process_metric(q) and ("노드" in q or "장비" in q or "endpoint" in q.lower() or "id별" in q.lower()):
        group_by = ["id", "ProcName"]
        if "회사" in q:
            group_by.append("CompanyName")
        if "제품" in q:
            group_by.append("ProductName")
        sort_field = requested_process_sort_field(q)
        return QueryPlan(
            collection="sprocess",
            time_range=time_range,
            group_by=group_by,
            metrics=process_perf_metrics(),
            sort=[SortSpec(field=sort_field, direction="desc")],
            limit=requested_limit(q, 100),
            limit_per_group=10,
            output="table",
            notes=["template_planner", "process metric by node"],
        )

    if (
        (not version_metadata_question or os_inventory_field_question)
        and (
            "nodeinfo" in q.lower()
            or "노드정보" in q
            or "장비" in q
            or "노드" in q
            or has_os_term(q)
            or "윈도" in q
        )
        and not (
            ("설치" in q and not (has_os_term(q) and ("설치일" in q or "설치 일" in q or "installdate" in q.lower())))
            or "제거 명령" in q
            or "삭제 명령" in q
            or "uninstall" in q.lower()
            or "게시자" in q
            or "프린터" in q
            or security_inventory_question(q)
            or windows_named_installed_program_question(q)
        )
    ):
        if has_os_term(q):
            filters = [FilterSpec(field="name", op="eq", value="OS")]
            q_lower = q.lower()
            os_count_metric = (
                [Metric(name="user_count", op="count_distinct", field="target.UserName")]
                if "사용자 수" in q or "사용자수" in q
                else [Metric(name="node_count", op="count_distinct", field="id")]
            )
            os_count_sort = "user_count" if os_count_metric[0].name == "user_count" else "node_count"
            os_filter = os_caption_filter_value(q)
            if os_filter:
                filters.append(FilterSpec(field="data.Caption", op="regex", value=os_filter))
            os_build_filter = os_build_filter_value(q)
            if os_build_filter:
                filters.append(FilterSpec(field="data.BuildNumber", op="eq", value=os_build_filter))
            if "설치일" in q or "설치 일" in q or "installdate" in q_lower:
                sort_direction = "asc" if "오래된" in q or "낡은" in q else "desc"
                return QueryPlan(
                    collection="nodeinfo",
                    time_range=time_range,
                    group_by=[
                        "id",
                        "target.ComputerName",
                        "target.UserName",
                        "target.ip",
                        "data.Caption",
                        "data.Version",
                        "data.BuildNumber",
                        "data.UBR",
                        "data.InstallDate",
                    ],
                    metrics=node_count_metric(),
                    filters=filters,
                    sort=[SortSpec(field="data.InstallDate", direction=sort_direction)],
                    limit=requested_limit(q, 200),
                    output="table",
                    notes=["template_planner", "node OS install date inventory"],
                )
            if ("사용자별" in q or "장비별" in q or "노드별" in q) and not ("사용자 수" in q or "사용자수" in q):
                return QueryPlan(
                    collection="nodeinfo",
                    time_range=time_range,
                    group_by=[
                        "id",
                        "target.ComputerName",
                        "target.UserName",
                        "target.UserRole",
                        "target.ip",
                        "data.Caption",
                        "data.Version",
                        "data.BuildNumber",
                        "data.UBR",
                        "data.OSArchitecture",
                    ],
                    metrics=node_count_metric(),
                    filters=filters,
                    sort=[SortSpec(field="target.UserName", direction="asc")],
                    limit=requested_limit(q, 200),
                    output="table",
                    notes=["template_planner", "node OS inventory by user"],
                )
            if "몇개" in q or "몇 개" in q or "장비 수" in q or "장비수" in q or "노드 수" in q or "노드수" in q:
                return QueryPlan(
                    collection="nodeinfo",
                    time_range=time_range,
                    group_by=["data.Caption", "data.BuildNumber", "data.UBR", "data.OSArchitecture"],
                    metrics=os_count_metric,
                    filters=filters,
                    sort=[SortSpec(field=os_count_sort, direction="desc")],
                    limit=requested_limit(q, 200),
                    output="table",
                    notes=["template_planner", "node OS count"],
                )
            if "빌드별" in q or "빌드 별" in q or "build" in q_lower or "ubr별" in q_lower or "ubr 별" in q_lower:
                return QueryPlan(
                    collection="nodeinfo",
                    time_range=time_range,
                    group_by=["data.Caption", "data.BuildNumber", "data.UBR", "data.OSArchitecture"],
                    metrics=os_count_metric,
                    filters=filters,
                    sort=[SortSpec(field=os_count_sort, direction="desc")],
                    limit=requested_limit(q, 200),
                    output="table",
                    notes=["template_planner", "node OS build distribution"],
                )
            return QueryPlan(
                collection="nodeinfo",
                time_range=time_range,
                group_by=["id", "target.ComputerName", "target.UserName", "data.CSName", "data.Caption", "data.BuildNumber", "data.UBR", "data.OSArchitecture"],
                metrics=node_count_metric(),
                filters=filters,
                sort=[SortSpec(field="id", direction="asc")],
                limit=requested_limit(q, 200),
                output="table",
                notes=["template_planner", "node OS inventory"],
            )

        if "cpu" in q.lower() or "코어" in q or "프로세서" in q:
            sort_direction = "asc" if "낮" in q or "적" in q else "desc"
            return QueryPlan(
                collection="nodeinfo",
                time_range=time_range,
                group_by=["id", "data.Name"],
                metrics=[
                    Metric(name="core_max", op="max", field="data.NumberOfCores"),
                    Metric(name="logical_processor_max", op="max", field="data.NumberOfLogicalProcessors"),
                    Metric(name="clock_max", op="max", field="data.MaxClockSpeed"),
                ],
                filters=[FilterSpec(field="name", op="eq", value="CPU")],
                sort=[SortSpec(field="core_max", direction=sort_direction)],
                limit=requested_limit(q, 200),
                output="table",
                notes=["template_planner", "node CPU inventory"],
            )

        if "메모리" in q or "memory" in q.lower() or "ram" in q.lower():
            return QueryPlan(
                collection="nodeinfo",
                time_range=time_range,
                group_by=["id", "data.Capacity"],
                metrics=node_count_metric(),
                filters=[FilterSpec(field="name", op="eq", value="MEMORY")],
                sort=[SortSpec(field="id", direction="asc")],
                limit=requested_limit(q, 200),
                output="table",
                notes=["template_planner", "node memory inventory"],
            )

        if "어댑터" in q or "adapter" in q.lower() or "mac 주소" in q.lower():
            return QueryPlan(
                collection="nodeinfo",
                time_range=time_range,
                group_by=[
                    "id",
                    "target.ComputerName",
                    "target.UserName",
                    "target.ip",
                    "data.Name",
                    "data.NetConnectionID",
                    "data.MACAddress",
                    "data.AdapterType",
                    "data.NetEnabled",
                    "data.Status",
                ],
                metrics=node_count_metric(),
                filters=[FilterSpec(field="name", op="eq", value="NETWORKADAPTER")],
                sort=[SortSpec(field="id", direction="asc")],
                limit=requested_limit(q, 200),
                output="table",
                notes=["template_planner", "network adapter inventory"],
            )

        if (
            "디스크" in q
            or "disk" in q.lower()
            or "드라이브" in q
            or "저장공간" in q
            or "여유 공간" in q
            or "여유공간" in q
            or "용량" in q
        ):
            sort_field = "free_space_gb_min" if "부족" in q or "여유" in q or "적" in q else "disk_size_gb_max"
            sort_direction = "asc" if sort_field == "free_space_gb_min" else "desc"
            filters = [
                FilterSpec(field="name", op="eq", value="DISKDRIVE"),
                FilterSpec(field="data.FreeSpace", op="not_empty", value=None),
                FilterSpec(field="data.FreeSpaceGB", op="not_empty", value=None),
                FilterSpec(field="data.DriveType", op="eq", value="Local Disk"),
            ]
            if re.search(r"\bC\s*(?:드라이브|drive|:)", q, flags=re.IGNORECASE):
                filters.append(FilterSpec(field="data.Name", op="regex", value=r"^C:"))
            return QueryPlan(
                collection="nodeinfo",
                time_range=time_range,
                group_by=[
                    "id",
                    "target.ComputerName",
                    "target.UserName",
                    "target.ip",
                    "data.Name",
                    "data.VolumeName",
                    "data.FileSystem",
                ],
                metrics=[
                    Metric(name="free_space_gb_min", op="min", field="data.FreeSpaceGB"),
                    Metric(name="disk_size_gb_max", op="max", field="data.SizeGB"),
                    Metric(name="drive_count", op="count", field="id"),
                ],
                filters=filters,
                sort=[SortSpec(field=sort_field, direction=sort_direction)],
                limit=requested_limit(q, 200),
                output="table",
                notes=["template_planner", "node disk free space inventory"],
            )

    if hotfix_inventory_question(q) and not windows_update_installed_product:
        filters = [FilterSpec(field="name", op="eq", value="UPDATE")]
        kb_match = re.search(r"\b(KB\d{4,})\b", q, flags=re.IGNORECASE)
        if kb_match:
            filters.append(FilterSpec(field="data.HotFixID", op="regex", value=kb_match.group(1)))
        group_by = ["data.HotFixID", "data.Caption", "data.InstalledOn", "data.InstalledBy", "data.Status"]
        if "노드" in q or "장비" in q or "사용자" in q:
            group_by = ["id", "target.ComputerName", "target.UserName", *group_by]
        return QueryPlan(
            collection="nodeinfo",
            time_range=time_range,
            group_by=group_by,
            metrics=[Metric(name="node_count", op="count_distinct", field="id")],
            filters=filters,
            sort=[SortSpec(field="node_count", direction="desc")],
            limit=requested_limit(q, 200),
            output="table",
            notes=["template_planner", "Windows update inventory"],
        )

    if "프린터" in q:
        printer_match = re.search(r"([A-Za-z가-힣0-9_.+#()/ -]+?)\s*프린터", q, flags=re.IGNORECASE)
        printer_term = printer_match.group(1).strip(" ,_-") if printer_match else ""
        filters = [FilterSpec(field="name", op="eq", value="PRINTER")]
        if "기본" in q:
            printer_term = ""
        if printer_term and not re.match(r"^(?:기본|전체|장비별|노드별|프린터|드라이버)$", printer_term, flags=re.IGNORECASE):
            filters.append(FilterSpec(field="data.DriverName", op="regex", value=regex_for_inventory_alternatives(printer_term)))
        group_by = [
            "id",
            "target.ComputerName",
            "target.UserName",
            "target.ip",
            "data.Name",
            "data.DriverName",
            "data.PortName",
            "data.Default",
            "data.Status",
        ]
        if has_os_term(q):
            group_by.extend(["node.OSCaption", "node.BuildNumber", "node.OSArchitecture"])
            if "ubr" in q.lower():
                group_by.append("node.UBR")
        metrics = node_count_metric()
        if "드라이버별" in q or "장비 수" in q or "장비수" in q:
            group_by = ["data.DriverName", "data.Name"]
            metrics = [Metric(name="node_count", op="count_distinct", field="id")]
        return QueryPlan(
            collection="nodeinfo",
            time_range=time_range,
            group_by=group_by,
            metrics=metrics,
            filters=filters,
            sort=[SortSpec(field="node_count" if metrics[0].name == "node_count" else "row_count", direction="desc")],
            limit=requested_limit(q, 200),
            output="table",
            notes=["template_planner", "printer inventory"],
        )

    driver_term = installed_program_term(q)
    if ("그래픽" in q or "비디오" in q or "드라이버" in q) and driver_term:
        return QueryPlan(
            collection="nodeinfo",
            time_range=time_range,
            group_by=[
                "id",
                "target.status",
                "target.ComputerName",
                "target.UserName",
                "target.UserRole",
                "target.ip",
                "data.Name",
            ],
            metrics=[Metric(name="node_count", op="count_distinct", field="id")],
            filters=[
                FilterSpec(field="name", op="eq", value="VIDEOCARD"),
                FilterSpec(field="data.Name", op="regex", value=driver_term),
            ],
            sort=[SortSpec(field="target.ComputerName", direction="asc")],
            limit=requested_limit(q, 200),
            output="table",
            notes=["template_planner", "video card driver inventory"],
        )

    vaccine_inventory_question = (
        explicit_security_inventory_question(q)
        and (
            "설치" in q
            or "버전" in q
            or "상태" in q
            or "서명" in q
            or "경로" in q
            or "장비" in q
            or "노드" in q
            or "사용자" in q
        )
        and not (
            ("설치일" in q or "제거" in q or "삭제" in q or "원본" in q or "게시자" in q)
            and not ("백신" in q or "보안 제품" in q)
        )
    )
    if vaccine_inventory_question:
        filters = [FilterSpec(field="name", op="eq", value="VACCINE")]
        security_term = security_inventory_product_regex(q)
        if security_term:
            filters.append(FilterSpec(field="data.displayName", op="regex", value=security_term))
        group_by = [
            "id",
            "target.ComputerName",
            "target.UserName",
            "data.displayName",
            "data.Signature",
            "data.Status",
        ]
        if "경로" in q or "path" in q.lower():
            group_by.extend(["data.FileCountersPath", "data.FileCounters.ProcPath"])
        metrics = node_count_metric()
        if "버전별" in q or "버전 별" in q or "장비 수" in q or "장비수" in q:
            group_by = ["data.displayName", "data.Signature", "data.Status"]
            metrics = [Metric(name="node_count", op="count_distinct", field="id")]
        return QueryPlan(
            collection="nodeinfo",
            time_range=time_range,
            group_by=group_by,
            metrics=metrics,
            filters=filters,
            sort=[SortSpec(field="node_count" if metrics[0].name == "node_count" else "row_count", direction="desc")],
            limit=requested_limit(q, 200),
            output="table",
            notes=["template_planner", "vaccine inventory"],
        )

    if (
        "설치 프로그램" in q
        or "설치된 프로그램" in q
        or "설치 항목" in q
        or "설치한" in q
        or "깔린" in q
        or "드라이버" in q
        or "uninstall" in q.lower()
        or installed_program_metadata_term(q) is not None
        or "제거 명령" in q
        or "삭제 명령" in q
        or installed_program_inventory_question(q)
        or (
            "설치" in q
            and (
                "제품명" in q
                or "버전" in q
                or "장비" in q
                or "노드" in q
                or "사용자" in q
                or "게시자" in q
                or "경로" in q
                or "위치" in q
                or "키" in q
                or "삭제" in q
                or "제거" in q
                or "설치일" in q
            )
        )
    ):
        filters = [FilterSpec(field="name", op="eq", value="UNINSTALL")]
        multiple_versions = multiple_installed_program_versions_question(q)
        recent_installed_inventory = (
            "최근" in q
            and ("설치된" in q or "설치한" in q or "설치" in q)
            and ("소프트웨어" in q or "프로그램" in q or "설치 항목" in q)
        )
        raw_term = None if recent_installed_inventory or multiple_versions else installed_program_term(q)
        metadata_term = installed_program_metadata_term(q)
        if metadata_term:
            raw_term = metadata_term
        if raw_term:
            product_name, product_version = split_installed_program_name_version(raw_term)
            if product_name:
                product_alias = installed_program_vendor_alias(product_name)
                filter_field = (
                    "data.publisher"
                    if looks_like_publisher_name(product_name)
                    and ("제품" in q or "소프트웨어" in q or "프로그램" in q)
                    and ("버전" in q or "분포" in q or "목록" in q or "장비 수" in q or "장비수" in q)
                    else "data.name"
                )
                filters.append(
                    FilterSpec(
                        field=filter_field,
                        op="regex",
                        value=product_alias or regex_for_installed_program_names(product_name),
                    )
                )
            if product_version:
                filters.append(FilterSpec(field="data.version", op="regex", value=re.escape(product_version)))
        elif not multiple_versions:
            term = installed_program_regex_term(q)
            if term:
                filters.append(FilterSpec(field="data.name", op="regex", value=term))
        requested_fields: list[str] = []
        if (
            "설치 경로" in q
            or "설치경로" in q
            or "설치 위치" in q
            or "설치위치" in q
            or "installlocation" in q.lower()
            or ("경로" in q and "원본" not in q)
        ):
            requested_fields.extend(["data.installLocation", "data.Path"])
            if asks_empty_value(q):
                filters.append(FilterSpec(field="data.installLocation", op="empty", value=None))
        if asks_empty_value(q) and ("version" in q.lower() or "버전" in q):
            filters.append(FilterSpec(field="data.version", op="empty", value=None))
        if "설치 키" in q or "설치키" in q or "installkey" in q.lower():
            requested_fields.append("data.installKey")
        if "원본" in q or "소스" in q or "installsource" in q.lower():
            requested_fields.append("data.installSource")
            if asks_empty_value(q):
                filters.append(FilterSpec(field="data.installSource", op="empty", value=None))
            elif "있는" in q:
                filters.append(FilterSpec(field="data.installSource", op="not_empty", value=None))
        if "제거" in q or "삭제" in q or "uninstall" in q.lower():
            requested_fields.append("data.uninstallString")
            if asks_empty_value(q):
                filters.append(FilterSpec(field="data.uninstallString", op="empty", value=None))
            elif "있는" in q:
                filters.append(FilterSpec(field="data.uninstallString", op="not_empty", value=None))
        if recent_installed_inventory or "설치일" in q or "설치 일" in q or "installedtime" in q.lower():
            requested_fields.append("data.installedTime")
        if metadata_term:
            requested_fields.extend(["data.installLocation", "data.Path"])
        requested_fields = list(dict.fromkeys(requested_fields))
        group_by = [
            "id",
            "target.status",
            "target.ComputerName",
            "target.UserName",
            "target.UserRole",
            "target.ip",
            "data.name",
            "data.version",
            "data.publisher",
            *requested_fields,
        ]
        metrics = node_count_metric()
        if "패치 준비" in q or ("OS 빌드" in q and ("설치" in q or "패치" in q)):
            group_by = ["node.OSCaption", "node.BuildNumber", "node.OSArchitecture", "data.name", "data.version", "data.publisher"]
            metrics = [Metric(name="node_count", op="count_distinct", field="id")]
        elif asks_empty_value(q) and ("version" in q.lower() or "버전" in q) and (
            "게시자" in q or "publisher" in q.lower() or "제품명" in q or "제품" in q
        ):
            group_by = ["data.publisher", "data.name", "data.version", *requested_fields]
            metrics = [Metric(name="node_count", op="count_distinct", field="id")]
        elif ("제품명" in q or "제품" in q or "소프트웨어" in q or "프로그램" in q) and "버전" in q and ("게시자" in q or "publisher" in q.lower()):
            group_by = ["data.name", "data.version", "data.publisher", *requested_fields]
            metrics = [Metric(name="node_count", op="count_distinct", field="id")]
        elif multiple_versions and wants_per_node_rows(q):
            group_by = [
                "id",
                "target.status",
                "target.ComputerName",
                "target.UserName",
                "target.UserRole",
                "target.ip",
                "data.name",
                "data.version",
                "data.publisher",
                *requested_fields,
            ]
            metrics = [Metric(name="row_count", op="count", field="id")]
        elif multiple_versions:
            group_by = ["data.name", "data.version", "data.publisher", *requested_fields]
            metrics = [Metric(name="node_count", op="count_distinct", field="id")]
        elif "게시자별" in q or "게시자 별" in q or "회사별" in q or "회사 별" in q:
            if "버전" in q or "version" in q.lower() or "분포" in q:
                group_by = ["data.publisher", "data.name", "data.version", *requested_fields]
            else:
                group_by = ["data.publisher", *requested_fields]
            metrics = [Metric(name="node_count", op="count_distinct", field="id")]
        elif "버전별" in q or "버전 별" in q or "장비 수" in q or "장비수" in q:
            group_by = ["data.name", "data.version", "data.publisher", *requested_fields]
            metrics = [Metric(name="node_count", op="count_distinct", field="id")]
        elif "제품별" in q or "제품 별" in q or "제품명별" in q or "제품명 별" in q:
            group_by = ["data.name", "data.version", "data.publisher", *requested_fields]
            metrics = [Metric(name="node_count", op="count_distinct", field="id")]
        elif re.search(r"(?:프로그램|소프트웨어).*(?:많은|많이|적은|적게).*(?:노드|장비)", q):
            group_by = [
                "id",
                "target.status",
                "target.ComputerName",
                "target.UserName",
                "target.UserRole",
                "target.ip",
            ]
            metrics = [Metric(name="installed_program_count", op="count", field="data.name")]
        sort = [SortSpec(field="node_count" if metrics[0].name == "node_count" else "row_count", direction="desc")]
        if metrics[0].name == "installed_program_count":
            sort = [SortSpec(field="installed_program_count", direction="asc" if asks_fewer_installed_programs(q) else "desc")]
        if recent_installed_inventory:
            sort = [SortSpec(field="data.installedTime", direction="desc")]
        elif "설치일" in q and ("오래된" in q or "낡은" in q):
            sort = [SortSpec(field="data.installedTime", direction="asc")]
        return QueryPlan(
            collection="nodeinfo",
            time_range=time_range,
            group_by=group_by,
            metrics=metrics,
            filters=filters,
            sort=sort,
            limit=requested_limit(q, 200),
            output="table",
            notes=["template_planner", "installed program multi-version inventory" if multiple_versions else "installed program inventory"],
        )

    if security_inventory_question(q):
        return QueryPlan(
            collection="nodeinfo",
            time_range=time_range,
            group_by=["id", "target.ComputerName", "target.UserName", "data.displayName", "data.Signature", "data.Status"],
            metrics=node_count_metric(),
            filters=[FilterSpec(field="name", op="eq", value="VACCINE")],
            sort=[SortSpec(field="id", direction="asc")],
            limit=requested_limit(q, 200),
            output="table",
            notes=["template_planner", "vaccine inventory"],
        )

    if ("업데이트 대상" in q or "패치 대상" in q or "구버전" in q) and ("제품" in q or "프로그램" in q or "소프트웨어" in q) and (
        "버전별" in q or "버전 별" in q or "많은" in q
    ):
        return QueryPlan(
            collection="nodeinfo",
            time_range=time_range,
            group_by=["data.publisher", "data.name", "data.version"],
            metrics=[Metric(name="node_count", op="count_distinct", field="id")],
            filters=[FilterSpec(field="name", op="eq", value="UNINSTALL")],
            sort=[SortSpec(field="node_count", direction="desc")],
            limit=requested_limit(q, 200),
            output="table",
            notes=["template_planner", "installed program version distribution"],
        )

    installed_version_term = installed_product_version_term(q)
    if installed_version_term:
        inv_time_range = inventory_time_range(q, timezone)
        filter_field = installed_product_version_filter_field(q)
        filters = [FilterSpec(field="name", op="eq", value="UNINSTALL")]
        if filter_field == "data.name":
            product_name, product_version = split_installed_program_name_version(strip_inventory_prefix(installed_version_term))
            if product_name:
                filters.append(FilterSpec(field="data.name", op="regex", value=regex_with_flexible_name_separators(product_name)))
            if product_version:
                filters.append(FilterSpec(field="data.version", op="regex", value=installed_program_version_regex(product_version)))
        else:
            filters.append(FilterSpec(field=filter_field, op="regex", value=re.escape(installed_version_term)))
        return QueryPlan(
            collection="nodeinfo",
            time_range=inv_time_range,
            group_by=["data.publisher", "data.name", "data.version"],
            metrics=[Metric(name="node_count", op="count_distinct", field="id")],
            filters=filters,
            sort=[SortSpec(field="node_count", direction="desc")],
            limit=requested_limit(q, 200),
            output="table",
            notes=["template_planner", "installed program publisher/version inventory"],
        )

    if version_metadata_question:
        group_by = version_group_fields(q)
        return QueryPlan(
            collection="sprocess",
            time_range=time_range,
            group_by=group_by,
            metrics=process_perf_metrics(),
            sort=[SortSpec(field="sample_count", direction="desc")],
            limit=requested_limit(q, 100),
            output="table",
            notes=["template_planner", "Windows file version metadata"],
        )

    if "부모" in q or "pproc" in q.lower():
        group_by = ["PProcName"]
        if "자식" in q or "프로세스" in q:
            group_by = ["PProcName", "ProcName"]
        return QueryPlan(
            collection="sprocess",
            time_range=time_range,
            group_by=group_by,
            metrics=process_perf_metrics(),
            sort=[SortSpec(field="sample_count", direction="desc")],
            limit=requested_limit(q, 100),
            output="table",
            notes=["template_planner", "parent process"],
        )

    if "powershell" in q.lower() or "파워쉘" in q:
        return QueryPlan(
            collection="sprocess",
            time_range=time_range,
            group_by=["ProcName", "Command"] if "명령" in q or "커맨드" in q or "command" in q.lower() else ["ProcName"],
            metrics=process_perf_metrics(),
            filters=[FilterSpec(field="ProcName", op="regex", value="powershell")],
            sort=[SortSpec(field="sample_count", direction="desc")],
            limit=requested_limit(q, 100),
            output="table",
            notes=["template_planner", "powershell process filter"],
        )

    if "노드" in q or "장비" in q or "endpoint" in q.lower() or "id별" in q.lower():
        group_by = ["id"]
        if "프로세스" in q:
            group_by.append("ProcName")
        sort_field = requested_process_sort_field(q) if "프로세스" in q and asks_process_metric(q) else ("pscore_avg" if "부하" in q or "load" in q.lower() else "sample_count")
        return QueryPlan(
            collection="sprocess",
            time_range=time_range,
            group_by=group_by,
            metrics=process_perf_metrics(),
            sort=[SortSpec(field=sort_field, direction="desc")],
            limit=requested_limit(q, 100),
            limit_per_group=10 if len(group_by) > 1 else None,
            output="table",
            notes=["template_planner", "node id"],
        )

    if "ticket" in q.lower() or "티켓" in q or "수집 세션" in q:
        group_by = ["ticket"]
        if "프로세스" in q:
            group_by.append("ProcName")
        return QueryPlan(
            collection="sprocess",
            time_range=time_range,
            group_by=group_by,
            metrics=process_perf_metrics(),
            sort=[SortSpec(field="sample_count", direction="desc")],
            limit=requested_limit(q, 100),
            limit_per_group=10 if len(group_by) > 1 else None,
            output="table",
            notes=["template_planner", "collection ticket"],
        )

    if "명령줄" in q or "커맨드라인" in q or "command" in q.lower():
        group_by = ["Command"]
        if "프로세스" in q:
            group_by = ["ProcName", "Command"]
        return QueryPlan(
            collection="sprocess",
            time_range=time_range,
            group_by=group_by,
            metrics=process_perf_metrics(),
            sort=[SortSpec(field="sample_count", direction="desc")],
            limit=requested_limit(q, 100),
            output="table",
            notes=["template_planner", "command line"],
        )

    if "경로" in q or "path" in q.lower():
        group_by = ["FilePath"] if "파일" in q else ["ProcPath"]
        if "프로세스" in q:
            group_by = ["ProcName", group_by[0]]
        return QueryPlan(
            collection="sprocess",
            time_range=time_range,
            group_by=group_by,
            metrics=process_perf_metrics(),
            sort=[SortSpec(field="sample_count", direction="desc")],
            limit=requested_limit(q, 100),
            output="table",
            notes=["template_planner", "path metadata"],
        )

    if "파일명" in q or "파일 이름" in q or "filename" in q.lower():
        return QueryPlan(
            collection="sprocess",
            time_range=time_range,
            group_by=["FileName"],
            metrics=process_perf_metrics(),
            sort=[SortSpec(field="sample_count", direction="desc")],
            limit=requested_limit(q, 100),
            output="table",
            notes=["template_planner", "file name metadata"],
        )

    if "서명" in q or "signer" in q.lower() or "codesign" in q.lower() or "게시자" in q or "퍼블리셔" in q:
        if "없는" in q or "없" in q or "미서명" in q:
            return QueryPlan(
                collection="sprocess",
                time_range=time_range,
                group_by=["ProcName", "ProcPath", "CompanyName"],
                metrics=process_perf_metrics(),
                filters=[
                    FilterSpec(field="Signer", op="empty", value=None),
                    FilterSpec(field="Codesign", op="empty", value=None),
                ],
                filter_mode="any",
                sort=[SortSpec(field="sample_count", direction="desc")],
                limit=requested_limit(q, 100),
                output="table",
                notes=["template_planner", "unsigned process"],
            )
        group_by = ["Codesign"] if "상세" in q or "인증서" in q or "codesign" in q.lower() else ["Signer"]
        if "프로세스" in q:
            group_by.append("ProcName")
        return QueryPlan(
            collection="sprocess",
            time_range=time_range,
            group_by=group_by,
            metrics=process_perf_metrics(),
            sort=[SortSpec(field="sample_count", direction="desc")],
            limit=requested_limit(q, 100),
            output="table",
            notes=["template_planner", "signature metadata"],
        )

    if "시스템 파일" in q or "비시스템" in q or "issystem" in q.lower():
        group_by = ["IsSystem"]
        if "프로세스" in q:
            group_by.append("ProcName")
        filters = []
        if "비시스템" in q or "아닌" in q:
            filters = [FilterSpec(field="IsSystem", op="eq", value=0)]
        elif "시스템 파일" in q and ("만" in q or "중" in q):
            filters = [FilterSpec(field="IsSystem", op="eq", value=1)]
        return QueryPlan(
            collection="sprocess",
            time_range=time_range,
            group_by=group_by,
            metrics=process_perf_metrics(),
            filters=filters,
            sort=[SortSpec(field="sample_count", direction="desc")],
            limit=requested_limit(q, 100),
            output="table",
            notes=["template_planner", "system file flag"],
        )

    if ("프로세스" in q and ("부하" in q or "많이" in q or "높" in q)) or "pscore" in q.lower():
        group_by = ["ProcName"]
        if "요일" in q:
            group_by = ["weekday", "ProcName"]
        elif "시간대" in q:
            group_by = ["hour", "ProcName"]
        metrics = [
            Metric(name="pscore_avg", op="avg_by_count", field="pscore"),
            Metric(name="cpu_avg", op="avg_by_count", field="CPU"),
            Metric(name="memory_avg_mb", op="avg_by_count", field="Memory"),
            Metric(name="io_avg_mbps", op="avg_by_count", field="IO"),
            Metric(name="handle_avg", op="avg_by_count", field="Handle"),
            Metric(name="sample_count", op="sum", field="CounterCount"),
        ]
        sort_field = requested_process_sort_field(q)
        return QueryPlan(
            collection="sprocess",
            time_range=time_range,
            group_by=group_by,
            metrics=metrics,
            sort=[SortSpec(field=sort_field, direction="desc")],
            limit=requested_limit(q, 100),
            limit_per_group=10 if len(group_by) > 1 else None,
            output="table",
            notes=["template_planner"],
        )

    if "countercount" in q.lower() or "카운터" in q or "측정" in q or "샘플" in q:
        group_by = ["ProcName"]
        if "요일" in q:
            group_by = ["weekday", "ProcName"]
        elif "시간대" in q:
            group_by = ["hour", "ProcName"]
        return QueryPlan(
            collection="sprocess",
            time_range=time_range,
            group_by=group_by,
            metrics=process_perf_metrics(),
            sort=[SortSpec(field="sample_count", direction="desc")],
            limit=requested_limit(q, 100),
            limit_per_group=10 if len(group_by) > 1 else None,
            output="table",
            notes=["template_planner", "CounterCount is Agent sample count"],
        )

    if "회사명" in q or "회사별" in q:
        group_by = version_group_fields(q) if "버전" in q or "설명" in q else ["CompanyName"]
        return QueryPlan(
            collection="sprocess",
            time_range=time_range,
            group_by=group_by,
            metrics=[
                Metric(name="io_avg_mbps", op="avg_by_count", field="IO"),
                Metric(name="sample_count", op="sum", field="CounterCount"),
            ],
            sort=[SortSpec(field="io_avg_mbps", direction="desc")],
            limit=requested_limit(q, 100),
            output="table",
            notes=["template_planner"],
        )

    if "제품명" in q or "제품별" in q:
        group_by = version_group_fields(q) if "버전" in q or "설명" in q else ["ProductName"]
        return QueryPlan(
            collection="sprocess",
            time_range=time_range,
            group_by=group_by,
            metrics=[
                Metric(name="pscore_avg", op="avg_by_count", field="pscore"),
                Metric(name="sample_count", op="sum", field="CounterCount"),
            ],
            sort=[SortSpec(field="pscore_avg", direction="desc")],
            limit=requested_limit(q, 100),
            output="table",
            notes=["template_planner"],
        )

    if "제품버전" in q or "제품 버전" in q or "productversion" in q.lower():
        return QueryPlan(
            collection="sprocess",
            time_range=time_range,
            group_by=["ProductName", "ProductVersion"],
            metrics=process_perf_metrics(),
            sort=[SortSpec(field="sample_count", direction="desc")],
            limit=requested_limit(q, 100),
            output="table",
            notes=["template_planner", "product version metadata"],
        )

    if "메모리" in q and ("추이" in q or "전체" in q or "사용률" in q):
        group_by = ["hour"] if "시간대" in q else ["date"]
        return QueryPlan(
            collection="system",
            time_range=time_range,
            group_by=group_by,
            metrics=[Metric(name="memory_rate_avg", op="avg", field="Memory.rate")],
            sort=[SortSpec(field=group_by[0], direction="asc")],
            limit=200,
            output="line_chart",
            notes=["template_planner"],
        )

    if "크래시" in q or "충돌" in q or "탐지" in q:
        if "프로세스" in q:
            group_by = ["ProcName"]
            if "요일" in q:
                group_by = ["weekday", "ProcName"]
            elif "시간" in q:
                group_by = ["hour", "ProcName"]
            event_filters = []
            if "있는" in q or "발생" in q or "많" in q:
                if "탐지" in q:
                    event_filters.append(FilterSpec(field="Detect", op="gt", value=0))
                if "크래시" in q or "충돌" in q:
                    event_filters.append(FilterSpec(field="Crash", op="gt", value=0))
            return QueryPlan(
                collection="sprocess",
                time_range=time_range,
                group_by=group_by,
                metrics=[
                    Metric(name="crash_sum", op="sum", field="Crash"),
                    Metric(name="detect_sum", op="sum", field="Detect"),
                    Metric(name="sample_count", op="sum", field="CounterCount"),
                    Metric(name="pscore_avg", op="avg_by_count", field="pscore"),
                ],
                filters=event_filters,
                filter_mode="any",
                sort=[SortSpec(field="detect_sum", direction="desc")],
                limit=requested_limit(q, 100),
                limit_per_group=10 if len(group_by) > 1 else None,
                output="table",
                notes=["template_planner", "process events"],
            )
        group_by = ["date", "hour"] if "시간" in q else ["date"]
        return QueryPlan(
            collection="system",
            time_range=time_range,
            group_by=group_by,
            metrics=[
                Metric(name="crash_sum", op="sum", field="Process.crash"),
                Metric(name="detect_sum", op="sum", field="Stat.detect"),
            ],
            sort=[SortSpec(field="crash_sum", direction="desc")],
            limit=100,
            output="table",
            notes=["template_planner"],
        )

    if "실행" in q or "시작" in q or "종료" in q or "running" in q.lower():
        group_by = ["ProcName"]
        if "요일" in q:
            group_by = ["weekday", "ProcName"]
        elif "시간" in q:
            group_by = ["hour", "ProcName"]
        sort_field = "start_sum"
        if "종료" in q:
            sort_field = "stop_sum"
        elif "실행중" in q or "실행 중" in q or "running" in q.lower():
            sort_field = "running_sum"
        return QueryPlan(
            collection="sprocess",
            time_range=time_range,
            group_by=group_by,
            metrics=[
                Metric(name="running_sum", op="sum", field="Running"),
                Metric(name="start_sum", op="sum", field="Start"),
                Metric(name="stop_sum", op="sum", field="Stop"),
                Metric(name="sample_count", op="sum", field="CounterCount"),
            ],
            sort=[SortSpec(field=sort_field, direction="desc")],
            limit=requested_limit(q, 100),
            limit_per_group=10 if len(group_by) > 1 else None,
            output="table",
            notes=["template_planner", "process lifecycle"],
        )

    return None
