#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
import random
import sys
import time
import uuid
import http.client
import urllib.error
import urllib.request
from dataclasses import dataclass
from pathlib import Path
from typing import Any


DEFAULT_SERVER = "http://127.0.0.1:3184"
ROOT = Path(__file__).resolve().parent
OUTPUT_DIR = ROOT / "output"


@dataclass(frozen=True)
class Scenario:
    category: str
    question: str
    execute: bool = True
    source: str = "orange"
    expected_collections: tuple[str, ...] = ()


def request_json(method: str, url: str, payload: dict[str, Any] | None = None, timeout: float = 120.0) -> Any:
    data = None if payload is None else json.dumps(payload, ensure_ascii=False).encode("utf-8")
    request = urllib.request.Request(
        url,
        data=data,
        headers={"Content-Type": "application/json"},
        method=method,
    )
    with urllib.request.urlopen(request, timeout=timeout) as response:
        return json.loads(response.read().decode("utf-8"))


def base_scenarios() -> list[Scenario]:
    return [
        Scenario("endpoint-health", "현재 온라인 장비와 오프라인 장비 수를 보여줘", source="human-request-regression", expected_collections=("node",)),
        Scenario("software-inventory", "Microsoft Edge와 Microsoft OneDrive 설치 버전을 사용자별로 보여줘", source="human-request-regression", expected_collections=("nodeinfo",)),
        Scenario("asset-inventory", "NETWORK 인벤토리에서 dhcp gateway subnet 조합별 장비 수를 보여줘", source="runtime-regression", expected_collections=("nodeinfo",)),
        Scenario("asset-inventory", "SYSTEM 인벤토리에서 BIOS 제조사와 BaseBoard 제품을 장비별로 보여줘 인벤토리 기준", source="runtime-regression", expected_collections=("nodeinfo",)),
        Scenario("software-inventory", "설치 프로그램 중 version이 비어 있는 항목을 게시자 제품명 기준으로 보여줘", source="needs-review-regression", expected_collections=("nodeinfo",)),
        Scenario("software-inventory", "설치 프로그램 중 게시자 제품명 installLocation 조합별 장비 수를 보여줘 인벤토리 기준", source="needs-review-regression", expected_collections=("nodeinfo",)),
        Scenario("software-inventory", "안랩 제품 버전별 설치 장비를 찾아줘", source="defender/tanium", expected_collections=("nodeinfo", "filelist")),
        Scenario("asset-inventory", "Microsoft Print To PDF와 SINDOH 프린터 드라이버 설치 장비 수를 비교해줘", source="human-request-regression", expected_collections=("nodeinfo",)),
        Scenario("asset-inventory", "VMware SVGA 3D와 Mirage Driver 그래픽 카드 장비 수를 보여줘", source="human-request-regression", expected_collections=("nodeinfo",)),
        Scenario("asset-inventory", "전체 장비의 윈도우 버전 분포를 보여줘", source="defender/tanium", expected_collections=("nodeinfo",)),
        Scenario("asset-inventory", "윈도우 10 장비 중 CPU 코어 수가 낮은 노드를 찾아줘", source="servicenow/tanium", expected_collections=("nodeinfo", "sprocess_nodeinfo")),
        Scenario("asset-inventory", "물리 메모리 크기가 제일 큰 장비와 사용자를 보여줘", source="servicenow", expected_collections=("node",)),
        Scenario("asset-inventory", "삼성 장비 수를 보여줘", source="needs-review-regression", expected_collections=("nodeinfo",)),
        Scenario("asset-inventory", "제조사별 장비 수를 보여줘", source="needs-review-regression", expected_collections=("nodeinfo",)),
        Scenario("asset-inventory", "OS 빌드 19045와 26200 장비 수를 비교해줘", source="needs-review-regression", expected_collections=("nodeinfo",)),
        Scenario("asset-inventory", "삼성 장비를 가진 사용자를 찾아줘", source="orange", expected_collections=("nodeinfo",)),
        Scenario("asset-inventory", "Mirage 그래픽 드라이버 설치한 노드", source="orange", expected_collections=("nodeinfo",)),
        Scenario("asset-inventory", "노드별 CPU 모델과 코어 수를 보여줘", source="tanium/itam", expected_collections=("nodeinfo",)),
        Scenario("asset-inventory", "노드별 메모리 슬롯과 용량을 보여줘", source="tanium/itam", expected_collections=("nodeinfo",)),
        Scenario("asset-inventory", "디스크 용량이 큰 장비를 보여줘", source="tanium/itam", expected_collections=("nodeinfo",)),
        Scenario("asset-inventory", "디스크 부족한 노드들 출력", source="tanium/itam", expected_collections=("nodeinfo",)),
        Scenario("asset-inventory", "디스크 여유 공간이 부족한 장비를 찾아줘", source="tanium/itam", expected_collections=("nodeinfo",)),
        Scenario("asset-inventory", "C 드라이브 여유 공간이 적은 노드를 찾아줘", source="tanium/itam", expected_collections=("nodeinfo",)),
        Scenario("asset-inventory", "C: 드라이브 여유공간 부족 장비를 사용자별로 보여줘", source="tanium/itam", expected_collections=("nodeinfo",)),
        Scenario("asset-inventory", "네트워크 어댑터 목록을 장비별로 보여줘", source="tanium/itam", expected_collections=("nodeinfo",)),
        Scenario("asset-inventory", "IP 주소별 장비 목록을 보여줘", source="tanium/itam", expected_collections=("nodeinfo",)),
        Scenario("asset-inventory", "프린터가 설치된 장비를 보여줘", source="tanium/itam", expected_collections=("nodeinfo",)),
        Scenario("asset-inventory", "SINDOH 프린터 드라이버 설치 장비 수를 보여줘", source="tanium/itam", expected_collections=("nodeinfo",)),
        Scenario("asset-inventory", "프린터 드라이버 기본 프린터 상태를 장비별로 보여줘 인벤토리 기준", source="human-request-regression", expected_collections=("nodeinfo",)),
        Scenario("asset-inventory", "SINDOH N910/MF Series PCL 프린터 드라이버 장비의 윈도우 버전을 보여줘", source="runtime-regression", expected_collections=("nodeinfo",)),
        Scenario("asset-inventory", "최근 설치된 윈도우 업데이트 목록을 보여줘", source="tanium/itam", expected_collections=("nodeinfo",)),
        Scenario("asset-inventory", "에이전트 버전별 장비 수를 보여줘", source="tanium/itam", expected_collections=("nodeinfo",)),
        Scenario("software-inventory", "백신 제품과 상태를 노드별로 보여줘", source="defender/tanium", expected_collections=("nodeinfo",)),
        Scenario("software-inventory", "AhnLab V3 Lite 보안 제품을 사용자별로 보여줘", source="defender/tanium", expected_collections=("nodeinfo",)),
        Scenario("software-inventory", "Windows Defender 보안 제품 서명 상태를 사용자별로 보여줘", source="defender/tanium", expected_collections=("nodeinfo",)),
        Scenario("software-inventory", "알약 백신 상태와 서명을 사용자별로 보여줘", source="defender/tanium", expected_collections=("nodeinfo",)),
        Scenario("software-inventory", "회사 전체 프로그램 설치 목록 보여줘", source="tanium/itam", expected_collections=("nodeinfo",)),
        Scenario("software-inventory", "회사 전체 설치 프로그램을 제품명 버전 게시자별로 보여줘", source="tanium/itam", expected_collections=("nodeinfo",)),
        Scenario("software-inventory", "Microsoft Visual C++ Redistributable 설치 버전 분포를 게시자별로 보여줘", source="needs-review-regression", expected_collections=("nodeinfo",)),
        Scenario("software-inventory", "설치된 프로그램 목록을 노드별로 보여줘", source="tanium/itam", expected_collections=("nodeinfo",)),
        Scenario("software-inventory", "설치된 프로그램을 게시자별로 집계해줘", source="tanium/itam", expected_collections=("nodeinfo",)),
        Scenario("software-inventory", "설치된 프로그램을 버전별로 집계해줘", source="tanium/itam", expected_collections=("nodeinfo",)),
        Scenario("software-inventory", "프로그램이 가장 많이 설치된 노드를 찾아줘", source="tanium/itam", expected_collections=("nodeinfo",)),
        Scenario("software-inventory", "Google Chrome이 설치된 노드 찾기", source="tanium/itam", expected_collections=("nodeinfo",)),
        Scenario("software-inventory", "Microsoft Edge 설치 버전별 장비 수", source="defender/tanium", expected_collections=("nodeinfo",)),
        Scenario("software-inventory", "Chrome 설치 버전별 장비 수를 보여줘", source="defender/tanium", expected_collections=("nodeinfo", "filelist")),
        Scenario("software-inventory", "Google Chrome 설치 장비와 버전을 사용자별로 보여줘", source="tanium/itam", expected_collections=("nodeinfo",)),
        Scenario("software-inventory", "AhnLab V3 Lite 백신 상태와 서명을 장비별로 보여줘", source="defender/tanium", expected_collections=("nodeinfo",)),
        Scenario("software-inventory", "최근 새로 설치된 프로그램과 파일 목록을 보여줘", source="tanium", expected_collections=("filelist", "nodeinfo")),
        Scenario("endpoint-health", "우리회사에서 메모리 사용률이 제일 높은 사람은 누구", source="servicenow", expected_collections=("node",)),
        Scenario("endpoint-health", "CPU 사용률이 높은 장비와 현재 사용자를 보여줘", source="servicenow", expected_collections=("node",)),
        Scenario("endpoint-health", "오프라인 장비를 사용자별로 보여줘", source="orange", expected_collections=("node",)),
        Scenario("endpoint-health", "VHP2의 모든 장비 상태와 사양을 보여줘", source="autoresearch-regression", expected_collections=("node",)),
        Scenario("endpoint-health", "최근 7일 전체 메모리 사용률 추이를 보여줘", source="servicenow", expected_collections=("system",)),
        Scenario("endpoint-health", "최근 회사 전체 설치 이벤트 추이를 보여줘", source="human-request-regression", expected_collections=("system",)),
        Scenario("asset-inventory", "Win10(1507)은 장비 몇개를 가지고 있지?", source="human-request-regression", expected_collections=("nodeinfo",)),
        Scenario("process-performance", "최근 7일간 부하가 가장 높은 프로세스 20개", source="servicenow", expected_collections=("sprocess",)),
        Scenario("process-performance", "오늘 CPU를 많이 사용한 프로세스를 사용자 장비별로 보여줘", source="servicenow", expected_collections=("sprocess", "sprocess_nodeinfo")),
        Scenario("process-performance", "지난 3일 동안 IO가 높은 프로세스를 제품명별로 정리해줘", source="servicenow", expected_collections=("sprocess",)),
        Scenario("process-performance", "최근 일주일 메모리 평균이 높은 프로세스를 요일별로 뽑아줘", source="servicenow", expected_collections=("sprocess",)),
        Scenario("process-performance", "장시간 실행되면서 메모리 사용량이 높은 프로세스를 보여줘", source="service-ops", expected_collections=("sprocess",)),
        Scenario("process-performance", "안랩 프로세스의 평균 CPU와 메모리를 보여줘", source="orange", expected_collections=("sprocess",)),
        Scenario("process-performance", "MpDefenderCoreService.exe 프로세스 평균 CPU Memory IO Handle 부하지수를 보여줘", source="human-request-regression", expected_collections=("sprocess",)),
        Scenario("process-performance", "MpDefenderCoreService.exe 프로세스 측정 횟수와 평균 부하지수를 보여줘", source="human-request-regression", expected_collections=("sprocess",)),
        Scenario("process-integrity", "서명 없는 프로세스 중 부하지수가 높은 파일 경로를 찾아줘", source="defender/tanium", expected_collections=("sprocess",)),
        Scenario("process-integrity", "비시스템 프로세스 중 부하가 높은 항목을 보여줘", source="defender", expected_collections=("sprocess",)),
        Scenario("process-integrity", "PowerShell을 실행한 프로세스와 명령줄을 보여줘", source="security-ops", expected_collections=("sprocess",)),
        Scenario("process-integrity", "Chrome 관련 프로세스의 실행 파일 버전별 측정 횟수", source="defender/tanium", expected_collections=("sprocess",)),
        Scenario("process-integrity", "MpDefenderCoreService.exe 프로세스 파일 버전과 회사명을 보여줘", source="human-request-regression", expected_collections=("sprocess",)),
        Scenario("file-change", "오늘 새로 설치된 파일 목록", source="tanium", expected_collections=("filelist",)),
        Scenario("file-change", "최근 7일간 서명 없는 파일 경로", source="defender/tanium", expected_collections=("filelist",)),
        Scenario("file-change", "다운로드 출처가 있는 파일을 최근 생성 순서로 보여줘", source="security-ops", expected_collections=("filelist",)),
        Scenario("file-change", "파일 인벤토리에서 ProductVersion Signer HostUrl 조합별 파일 수를 보여줘 인벤토리 기준", source="needs-review-regression", expected_collections=("filelist",)),
        Scenario("file-change", "안랩 경로에 생성된 파일을 제품 버전별로 보여줘", source="orange", expected_collections=("filelist",)),
        Scenario("event-triage", "최근 네트워크 장애 증상이 많은 장비", source="servicenow", expected_collections=("detect", "report")),
        Scenario("event-triage", "최근 30일 탐지 많은 사용자", source="security-ops", expected_collections=("detect",)),
        Scenario("event-triage", "삭제된 파일 접근 이벤트가 많은 제품", source="orange", expected_collections=("detect", "timeline")),
        Scenario("event-triage", "CPU 원인이 많은 프로세스 리포트", source="servicenow", expected_collections=("report",)),
        Scenario("event-triage", "최근 1년 안랩 제품의 장애 사용자별 비중을 알려줘", source="orange", expected_collections=("report",)),
        Scenario("event-triage", "최근 1년 안랩 제품의 장애 비중", source="orange", expected_collections=("report",)),
        Scenario("asset-inventory", "KB5066790 설치 장비를 찾아줘", source="tanium/itam", expected_collections=("nodeinfo",)),
        Scenario("asset-inventory", "Update for x64-based Windows Systems (KB5001716) 설치 노드의 사용자와 장비 상태를 보여줘", source="autoresearch-regression", expected_collections=("nodeinfo",)),
        Scenario("asset-inventory", "Mirage Driver 그래픽 카드 장비를 사용자별로 보여줘", source="tanium/itam", expected_collections=("nodeinfo",)),
        Scenario("timeline", "최근 타임라인 이벤트가 많은 프로세스", source="security-ops", expected_collections=("timeline",)),
        Scenario("timeline", "네트워크 타임라인 이벤트가 많은 사용자", source="security-ops", expected_collections=("timeline",)),
        Scenario("operations-command", "BitLocker 관련 명령 템플릿 찾아줘", source="endpoint-management", expected_collections=("command_template",)),
        Scenario("operations-command", "최근 실행한 명령 상태 보여줘", source="endpoint-management", expected_collections=("command",)),
        Scenario("people", "팀장 목록 보여줘", source="orange", expected_collections=("user",)),
        Scenario("people", "최원용은 장비 몇개를 가지고 있지", source="orange", expected_collections=("node",)),
        Scenario("people", "최원용의 모든 걸 조사해", source="orange", expected_collections=()),
    ]


def generated_scenarios(seed: int, count: int) -> list[Scenario]:
    """Generate controlled variants from IT-admin intent axes.

    This is disabled by default. Use it only after the curated bank has a stable
    pass/fail baseline.
    """
    rng = random.Random(seed)
    periods = ["오늘", "최근 3일", "최근 7일", "최근 30일", "최근 1년"]
    scenarios: list[Scenario] = []
    # Product-specific process/event variants create many zero-result probes on
    # sparse recent windows. Keep generated coverage focused on stable endpoint
    # inventory; base_scenarios still exercises sprocess/report/detect.
    process_vendors: list[str] = []
    process_metrics = ["CPU", "메모리", "IO", "핸들", "부하지수"]
    process_groupings = ["프로세스별", "제품별", "회사별", "장비별", "사용자별", "요일별"]
    process_actions = ["많은 순서로 보여줘", "평균을 계산해줘", "분포를 보여줘"]
    for period in periods:
        for vendor in process_vendors:
            for metric in process_metrics:
                for grouping in process_groupings:
                    for action in process_actions:
                        scenarios.append(
                            Scenario(
                                "process-variant",
                                f"{period} {vendor} 관련 {metric} 사용량을 {grouping} {action}",
                                source="controlled-variant",
                            )
                        )

    event_vendors: list[str] = []
    event_metrics = ["장애", "탐지"]
    event_groupings = ["제품별", "장비별", "사용자별", "요일별"]
    event_actions = ["많은 순서로 보여줘", "비중을 알려줘", "분포를 보여줘"]
    for period in periods:
        for vendor in event_vendors:
            for metric in event_metrics:
                for grouping in event_groupings:
                    for action in event_actions:
                        scenarios.append(
                            Scenario(
                                "event-variant",
                                f"{period} {vendor} 제품의 {metric} {grouping} {action}",
                                source="controlled-variant",
                            )
                        )

    installed_program_products = [
        "Orange The Client 1.6",
        "Microsoft Edge",
        "Microsoft OneDrive",
        "Microsoft Update Health Tools",
        "Microsoft Visual C++ 2015-2022 Redistributable",
        "Google Chrome",
        "VMware Tools",
        "캡처 도구",
        "Microsoft Visual C++ 2015-2022 Redistributable (x64)",
        "nProtect Online Security",
        "nProtect Online Security V1.0(PFS)",
        "AhnLab Safe Transaction",
        "INISAFE",
        "MagicLine",
        "TouchEn",
        "TouchEn nxKey with E2E for 32bit",
        "Microsoft Office",
        "Microsoft Teams",
        "Firefox",
        "Adobe Acrobat",
        "Zoom",
        "Java",
        ".NET",
        "한컴오피스",
        "Update for x64-based Windows Systems (KB5001716)",
    ]
    file_inventory_products = [
        "Microsoft Edge",
        "Google Chrome",
        "AhnLab",
        "Windows Defender",
        "nProtect",
        "INISAFE",
        "MagicLine",
        "TouchEn",
        "Office",
        "Teams",
        "Firefox",
        "Adobe",
        "Zoom",
        "Java",
        ".NET",
        "한컴",
    ]
    security_products = [
        "Windows Defender",
        "AhnLab V3 Lite",
        "알약",
        "V3 Lite",
    ]
    uninstall_counter_products = [
        "Orange The Client 1.6",
        "Microsoft Edge",
        "Google Chrome",
    ]
    vaccine_counter_products = [
        "Windows Defender",
        "AhnLab V3 Lite",
        "알약 백신",
        "V3 Lite",
    ]
    inventory_questions = [
        "회사 전체 프로그램 설치 목록 보여줘",
        "회사 전체 설치 프로그램을 제품명별로 보여줘",
        "회사 전체 설치 프로그램을 버전별로 보여줘",
        "회사 전체 설치 프로그램을 게시자별로 보여줘",
        "설치 프로그램 목록을 노드별로 보여줘",
        "설치 프로그램 목록을 사용자별로 보여줘",
        "설치 프로그램 목록을 제품명 버전 게시자별로 보여줘",
        "설치된 프로그램이 많은 노드를 찾아줘",
        "설치된 프로그램이 적은 노드를 찾아줘",
        "동일 프로그램이 여러 버전으로 설치된 노드를 찾아줘",
        "동일 제품의 여러 버전이 설치된 현황을 보여줘 인벤토리 기준",
        "같은 소프트웨어의 여러 버전 설치 현황을 제품명과 게시자별로 보여줘",
        "설치 프로그램 게시자별 장비 수를 많은 순서로 보여줘",
        "제품명과 버전이 같은 설치 항목을 장비별로 보여줘",
        "설치 원본 경로가 있는 프로그램을 장비별로 보여줘",
        "제거 명령 문자열이 있는 설치 항목을 제품별로 보여줘",
        "Microsoft Edge 설치 위치와 버전을 장비별로 보여줘",
        "Microsoft OneDrive 설치 경로와 게시자를 사용자별로 보여줘",
        "Microsoft Update Health Tools 설치 노드의 OS 빌드를 보여줘",
        "Microsoft Visual C++ 2015-2022 Redistributable 설치 버전과 게시자 분포를 보여줘",
        "Microsoft Visual C++ 2015-2022 Redistributable 설치 장비를 x64 x86 제품명 기준으로 비교해줘",
        "Orange The Client 1.6 설치 경로와 에이전트 빌드를 비교해줘",
        "VMware Tools 설치 장비의 제품 버전 분포를 보여줘",
        "설치 시간이 오래된 Microsoft Edge 장비를 사용자별로 보여줘",
        "게시자 Microsoft Corporation 프로그램 설치 장비 수를 제품별로 보여줘",
        "Microsoft Edge가 설치된 노드 찾아줘",
        "삼성 장비를 사용자별로 보여줘",
        "삼성 장비를 모델별로 보여줘",
        "삼성 장비 수를 보여줘",
        "Mirage 그래픽 드라이버 설치 장비를 사용자별로 보여줘",
        "Mirage 그래픽 드라이버 설치 장비 수를 보여줘",
        "백신 제품과 상태를 노드별로 보여줘",
        "백신 제품 상태별 장비 수를 보여줘",
        "Windows Defender 서명 상태를 사용자별로 보여줘",
        "AhnLab V3 Lite 보안 제품을 사용자별로 보여줘",
        "Windows Defender 보안 제품을 사용자별로 보여줘",
        "알약 백신 상태와 서명을 사용자별로 보여줘",
        "알약 보안 제품 서명별 장비 수를 보여줘",
        "Chrome 설치 버전별 장비 수를 보여줘",
        "V3 보안 제품 상태별 장비 수를 보여줘",
        "알약 보안 제품 상태별 장비 수를 보여줘",
        "AhnLab 보안 제품 상태별 장비 수를 보여줘",
        "백신이 설치되지 않은 장비가 있는지 찾아줘",
        "백신 제품이 여러 개 설치된 장비를 찾아줘",
        "보안 제품 상태가 서로 다른 장비를 사용자별로 보여줘",
        "Windows Defender 보안 제품의 서명 메타데이터를 장비별로 보여줘",
        "AhnLab V3 Lite와 V3 Lite 보안 제품 버전을 비교해줘",
        "알약 백신이 설치된 장비의 보안 상태를 사용자별로 보여줘",
        "백신 제품별 보유 장비 수와 상태 분포를 보여줘",
        "Edge 설치 버전별 장비 수를 보여줘",
        "구버전 Chrome이 설치된 장비를 찾아줘",
        "서명 없는 실행 파일이 많은 장비를 보여줘",
        "보안 제품 서명별 장비 수를 보여줘",
        "제조사별 장비 수를 보여줘",
        "모델별 물리 메모리 크기 분포를 보여줘",
        "윈도우 11 미지원 사양으로 보이는 장비를 찾아줘",
        "CPU 코어 수가 낮고 메모리가 적은 장비를 찾아줘",
        "오래된 OS 빌드를 사용하는 장비를 찾아줘",
        "OS 빌드와 UBR 기준으로 장비 분포를 보여줘",
        "Windows 10 Pro와 Windows 11 Pro 장비를 OS 빌드별로 보여줘",
        "윈도우 설치일이 오래된 장비를 찾아줘",
        "마지막 부팅 시간이 오래된 장비를 찾아줘",
        "CPU 모델별 장비 수를 보여줘",
        "11th Gen Intel Core i5 CPU 장비의 OS 버전을 보여줘",
        "Intel Core Ultra CPU 장비를 사용자별로 보여줘",
        "CPU 코어 수별 장비 분포를 보여줘",
        "논리 프로세서 수가 낮은 장비를 찾아줘",
        "메모리 제조사별 장비 수를 보여줘",
        "Samsung 메모리 장비의 용량 분포를 보여줘",
        "Micron 메모리 장비를 사용자별로 보여줘",
        "메모리 슬롯별 용량 현황을 보여줘",
        "디스크 모델별 장비 수를 보여줘",
        "VMware Virtual NVMe Disk 장비의 디스크 용량을 사용자별로 보여줘",
        "디스크 여유 공간이 부족한 장비를 찾아줘",
        "디스크 부족한 노드들 출력",
        "저장공간 부족한 장비를 사용자별로 보여줘",
        "C 드라이브 여유 공간이 적은 노드를 찾아줘",
        "디스크 용량 대비 여유 공간이 적은 장비를 보여줘",
        "네트워크 어댑터 제조사별 장비 수를 보여줘",
        "Realtek PCIe GbE 네트워크 어댑터 장비를 사용자별로 보여줘",
        "Intel Wi-Fi 6 AX201 어댑터 장비의 MAC 주소를 보여줘",
        "VMware Virtual Ethernet Adapter 장비 수를 어댑터별로 보여줘",
        "IP 주소별 장비 목록을 보여줘",
        "MAC 주소가 중복된 장비가 있는지 찾아줘",
        "기본 프린터가 설정된 장비를 보여줘",
        "프린터 드라이버별 설치 장비 수를 보여줘",
        "Microsoft Print To PDF 기본 프린터 설정 장비를 보여줘",
        "Remote Desktop Easy Print 프린터 드라이버 장비를 사용자별로 보여줘",
        "SINDOH D450/CM Series PCL 프린터 장비의 OS 버전을 보여줘",
        "윈도우 업데이트 HotFixID별 설치 장비 수를 보여줘",
        "KB5011063 설치 장비를 찾아줘",
        "KB5015684 설치 장비를 사용자별로 보여줘",
        "KB5072653 패치 적용 장비 수를 보여줘",
        "KB5066130 설치 장비를 찾아줘",
        "KB5066790 패치 적용 장비를 OS 빌드별로 보여줘",
        "KB5033052 설치 장비와 설치일을 사용자별로 보여줘",
        "최근 설치된 HotFix를 노드별로 보여줘",
        "OS 빌드 19045 장비를 사용자별로 보여줘",
        "Windows 11 Pro 장비의 UBR 분포를 보여줘",
        "KB5072653 HotFix가 설치된 장비를 찾아줘",
        "에이전트 버전별 장비 수를 보여줘",
        "에이전트 설치 경로를 장비별로 보여줘",
        "에이전트 빌드별 장비 수를 보여줘",
        "Orange The Client 1.6 설치 노드를 버전별로 보여줘",
        "Windows Defender 서명 상태를 장비별로 보여줘",
        "AhnLab V3 Lite 백신 설치 장비를 보여줘",
        "알약 백신 상태와 서명을 사용자별로 보여줘",
        "Windows Defender 보안 제품 설치 장비를 찾아줘",
        "Microsoft Edge 설치 경로를 장비별로 보여줘",
        "Microsoft OneDrive 설치 버전별 장비 수를 보여줘",
        "Microsoft Update Health Tools 설치 장비 수를 보여줘",
        "Google Chrome 게시자와 버전을 장비별로 보여줘",
        "VMware Tools 설치 장비와 버전을 보여줘",
        "Update for x64-based Windows Systems KB5001716 설치 장비를 보여줘",
        "캡처 도구 설치 장비를 사용자별로 보여줘",
        "Microsoft Print To PDF 프린터 설치 장비를 보여줘",
        "SINDOH 프린터 드라이버 설치 장비 수를 보여줘",
        "Realtek 네트워크 어댑터 장비를 찾아줘",
        "Intel 무선 어댑터 설치 장비를 보여줘",
        "Bluetooth 네트워크 어댑터 장비를 사용자별로 보여줘",
        "VMware Virtual NVMe Disk 장비를 사용자별로 보여줘",
        "Samsung SSD 장비를 모델별로 보여줘",
        "Intel UHD Graphics 장비 수를 보여줘",
        "비디오카드 모델별 장비 수를 보여줘",
        "모니터 정보가 없는 장비를 찾아줘",
        "팬 상태가 비정상인 장비를 찾아줘",
        "가상 머신 장비를 제조사별로 보여줘",
        "VMware 장비를 사용자별로 보여줘",
        "Samsung 메모리 제조사 장비를 사용자별로 보여줘",
        "VMware SVGA 3D 그래픽 카드 장비를 사용자별로 보여줘",
        "Intel(R) Core(TM) Ultra 7 255H CPU 모델 장비를 사용자별로 보여줘",
        "SINDOH N910 프린터 드라이버별 장비 수를 보여줘",
        "SINDOH N910/MF Series PCL 프린터 드라이버 장비의 윈도우 버전을 보여줘",
        "Samsung Universal Print Driver 3 프린터 드라이버 장비의 윈도우 버전을 보여줘",
    ]
    installed_program_templates = [
        "{product}가 설치된 노드 찾아줘",
        "{product} 설치 노드를 사용자별로 보여줘",
        "{product} 설치 버전별 장비 수를 보여줘",
        "{product} 구버전이 설치된 장비를 찾아줘",
        "{product} 설치 게시자별 장비 수를 보여줘",
        "{product} 설치일이 오래된 장비를 찾아줘",
        "{product} 설치 노드의 패치 준비 상태를 OS 빌드 기준으로 보여줘",
        "{product} 게시자 메타데이터와 버전을 노드별로 보여줘",
        "{product} 설치 항목의 제품명 버전 게시자를 장비별로 보여줘",
    ]
    for product_index, product in enumerate(installed_program_products):
        scenarios.extend(
            Scenario(
                f"inventory-product-{product_index % 4}",
                template.format(product=product),
                source="controlled-variant",
                expected_collections=("nodeinfo",),
            )
            for template in installed_program_templates
        )
    file_inventory_templates = [
        "{product} 파일 경로와 파일 버전을 보여줘",
        "{product} 파일 서명과 회사명을 보여줘",
    ]
    for product in file_inventory_products:
        scenarios.extend(
            Scenario(
                "file-inventory-variant",
                template.format(product=product),
                source="controlled-variant",
                expected_collections=("filelist",),
            )
            for template in file_inventory_templates
        )
    for product in security_products:
        inventory_questions.extend(
            [
                f"{product} 백신 설치 장비를 보여줘",
                f"{product} 백신 상태와 서명을 장비별로 보여줘",
                f"{product} 백신 상태별 장비 수를 보여줘",
                f"{product} 보안 제품을 사용자별로 보여줘",
                f"{product} 보안 제품 상태 분포를 보여줘",
                f"{product} 보안 제품 서명별 장비 수를 보여줘",
            ]
        )
    scenarios.extend(Scenario("inventory-variant", question, source="controlled-variant") for question in inventory_questions)
    scenarios.extend(
        Scenario(
            "inventory-counter-variant",
            f"{product} 설치 항목의 PathCounters 부하지수를 장비별로 보여줘",
            source="controlled-variant",
            expected_collections=("nodeinfo",),
        )
        for product in uninstall_counter_products
    )
    scenarios.extend(
        Scenario(
            "inventory-counter-variant",
            f"{product} 설치 항목의 FileCounters 프로세스명을 보여줘",
            source="controlled-variant",
            expected_collections=("nodeinfo",),
        )
        for product in vaccine_counter_products
    )

    tanium_like_questions = [
        "설치 프로그램 목록을 게시자별로 집계해줘",
        "설치 프로그램을 제품명과 버전별로 보여줘",
        "최근 설치된 소프트웨어를 장비별로 보여줘",
        "패치가 필요한 것으로 보이는 구버전 소프트웨어를 찾아줘",
        "보안 제품이 설치된 장비와 설치되지 않은 장비를 비교해줘",
        "엔드포인트 자산을 OS와 제조사 기준으로 분류해줘",
        "관리 대상 장비 중 오프라인 상태가 많은 사용자를 보여줘",
        "장비별 주요 소프트웨어 버전 현황을 보여줘",
        "동일 제품의 여러 버전이 섞여 있는 소프트웨어를 찾아줘",
        "소프트웨어 게시자별 설치 장비 수를 보여줘",
        "윈도우 보안 제품 설치 현황을 사용자별로 보여줘",
        "보안 제품 상태가 비정상인 장비를 찾아줘",
        "업데이트 대상이 많은 제품을 버전별로 보여줘",
        "프로그램이 가장 적게 설치된 노드를 찾아줘",
        "회사 전체 설치 프로그램을 게시자와 제품 버전 기준으로 분포를 보여줘",
        "패치 적용 준비 상태를 HotFixID와 OS 빌드 기준으로 보여줘",
        "오프라인 관리 대상 장비의 OS 빌드와 에이전트 버전을 보여줘",
        "서명 없는 파일의 회사명과 제품 버전 분포를 보여줘",
        "노드별 소프트웨어 설치 상태를 제품명 버전 설치일 기준으로 보여줘",
    ]
    scenarios.extend(Scenario("itam-variant", question, source="tanium/itam-benchmark") for question in tanium_like_questions)

    endpoint_inventory_questions = [
        "Microsoft Edge 설치 항목을 제품명별로 보여줘",
        "Microsoft OneDrive 설치 항목을 설치일별로 보여줘",
        "Google Chrome 설치 항목을 게시자별로 보여줘",
        "Google Chrome 설치 제품을 버전별로 보여줘",
        "Google Chrome 설치 프로그램 제품명별 장비 수를 보여줘",
        "Microsoft Update Health Tools 설치 버전별 장비 수를 보여줘",
        "VMware Tools 설치 항목을 사용자별로 보여줘",
        "Orange The Client 1.6 설치일과 버전을 장비별로 보여줘",
        "캡처 도구 설치 버전별 장비 수를 보여줘",
        "알약 백신 설치 노드를 사용자별로 보여줘",
        "Windows Defender 보안 제품 서명별 장비 수를 보여줘",
        "AhnLab V3 Lite 보안 제품 상태별 장비 수를 보여줘",
        "V3 Lite 보안 제품 서명 상태를 장비별로 보여줘",
        "Windows Defender 보안 제품 상태별 장비 수를 보여줘",
        "알약 보안 제품 서명별 사용자 목록을 보여줘",
        "KB5015684 HotFixID별 설치 장비 수를 보여줘",
        "KB5066130 패치 설치 사용자를 보여줘",
        "KB5066790 설치 노드를 사용자별로 보여줘",
        "Windows 11 OS 빌드별 장비 수를 보여줘",
        "Windows 10 OS UBR별 장비 수를 보여줘",
        "Microsoft Print To PDF 프린터 설치 장비를 보여줘",
        "Remote Desktop Easy Print 프린터 설치 장비를 사용자별로 보여줘",
        "SINDOH D450 프린터 드라이버별 장비 수를 보여줘",
        "SINDOH N910 프린터 드라이버별 장비 수를 보여줘",
        "회사 전체 설치 프로그램에서 Microsoft Edge와 Google Chrome 버전 분포를 보여줘",
        "Microsoft Edge와 Microsoft OneDrive 설치 버전을 사용자별로 보여줘",
        "Microsoft Update Health Tools 설치 장비를 OS 빌드별로 보여줘",
        "Microsoft Visual C++ 2015-2022 Redistributable 설치 장비를 버전별로 보여줘",
        "Microsoft Visual C++ 2015-2022 Redistributable 설치 노드를 사용자별로 보여줘",
        "Google Chrome 설치 게시자와 설치 위치를 장비별로 보여줘",
        "캡처 도구 설치 여부와 버전을 노드별로 점검해줘",
        "VMware Tools 설치 사용자와 제품 버전을 장비별로 보여줘",
        "Windows Defender와 AhnLab V3 Lite 보안 제품 상태를 장비별로 비교해줘",
        "알약 보안 제품 설치 노드의 서명 상태를 보여줘",
        "KB5066790과 KB5072653 패치 적용 장비를 HotFixID별로 보여줘",
        "OS 빌드 19045 장비의 설치 프로그램 버전 현황을 보여줘",
        "Orange The Client 1.6 설치 경로와 에이전트 빌드를 장비별로 보여줘",
        "Microsoft Print To PDF와 SINDOH 프린터 드라이버 설치 장비 수를 비교해줘",
        "Realtek과 Intel 네트워크 어댑터 설치 장비를 사용자별로 보여줘",
        "VMware SVGA 3D와 Mirage Driver 그래픽 카드 장비 수를 보여줘",
        "Samsung 메모리와 VMware Virtual RAM 메모리 제조사별 장비 수를 보여줘",
    ]
    scenarios.extend(Scenario("endpoint-inventory-variant", question, source="tanium/itam-grounded") for question in endpoint_inventory_questions)

    endpoint_management_questions = [
        "회사 전체 설치 프로그램을 제품명 게시자 버전 기준으로 정리해줘",
        "장비별 소프트웨어 인벤토리를 제품명 버전 게시자 설치위치 기준으로 보여줘",
        "노드별 설치 프로그램 수를 많은 순서로 보여줘",
        "노드별 설치 프로그램 수를 적은 순서로 보여줘",
        "제품별 설치 버전 개수를 비교해서 여러 버전이 섞인 제품을 보여줘",
        "게시자별 설치 장비 수와 제품 수를 보여줘",
        "Microsoft Edge 설치 장비를 버전과 설치 위치 기준으로 보여줘",
        "Microsoft Edge 구버전 의심 장비를 버전별로 보여줘",
        "Microsoft Edge 설치 항목의 게시자 분포를 보여줘",
        "Microsoft OneDrive 설치 장비를 사용자와 버전 기준으로 보여줘",
        "Microsoft OneDrive 설치 위치가 있는 장비를 찾아줘",
        "Google Chrome 설치 장비를 버전과 게시자 기준으로 보여줘",
        "Chrome 구버전 의심 장비를 사용자별로 보여줘",
        "Firefox 설치 장비를 버전별로 보여줘",
        "Adobe Acrobat 설치 장비를 게시자와 버전 기준으로 보여줘",
        "Zoom 설치 장비를 사용자별로 보여줘",
        "Java 설치 장비를 버전별로 보여줘",
        ".NET 설치 항목을 제품명과 버전 기준으로 보여줘",
        "Microsoft Office 설치 장비를 버전과 게시자 기준으로 보여줘",
        "Microsoft Teams 설치 장비를 사용자별로 보여줘",
        "한컴오피스 설치 장비를 제품명 버전 기준으로 보여줘",
        "nProtect Online Security 설치 장비를 버전별로 보여줘",
        "nProtect 설치 항목의 게시자 분포를 보여줘",
        "INISAFE 설치 장비를 제품명과 버전 기준으로 보여줘",
        "MagicLine 설치 장비를 사용자별로 보여줘",
        "TouchEn 설치 장비를 버전별로 보여줘",
        "AhnLab Safe Transaction 설치 장비를 게시자와 버전 기준으로 보여줘",
        "AhnLab V3 Lite 보안 제품 장비를 상태와 서명 기준으로 보여줘",
        "V3 Lite 보안 제품 상태별 장비 수를 보여줘",
        "알약 보안 제품 장비를 상태와 서명 기준으로 보여줘",
        "Windows Defender 보안 제품 상태별 장비 수를 보여줘",
        "Windows Defender 서명 메타데이터를 장비별로 보여줘",
        "백신 제품 displayName별 설치 장비 수를 보여줘",
        "백신 제품 상태가 비어 있는 장비를 찾아줘",
        "보안 제품이 여러 개 탐지된 장비를 사용자별로 보여줘",
        "보안 제품 상태와 OS 빌드를 장비별로 보여줘",
        "OS Caption BuildNumber UBR 기준 장비 수를 보여줘",
        "Windows 10 장비의 BuildNumber UBR 분포를 보여줘",
        "Windows 11 장비의 BuildNumber UBR 분포를 보여줘",
        "OS BuildNumber 19045 장비의 UBR 분포를 보여줘",
        "OS BuildNumber 26200 장비의 UBR 분포를 보여줘",
        "OS 설치일이 오래된 장비를 사용자별로 보여줘",
        "마지막 부팅 시간이 오래된 장비를 OS 빌드별로 보여줘",
        "KB5066790 적용 장비를 OS BuildNumber UBR 기준으로 보여줘",
        "KB5015684 적용 장비를 사용자별로 보여줘",
        "KB5033052 적용 장비와 설치일을 보여줘",
        "KB5072653 패치 적용 장비 수를 HotFixID별로 보여줘",
        "윈도우 업데이트 InstalledOn 기준 패치 분포를 보여줘",
        "패치 HotFixID별 설치 장비와 OS 빌드를 보여줘",
        "패치가 없는 것으로 보이는 장비를 OS 빌드 기준으로 점검해줘",
        "오프라인 관리 대상 장비를 사용자와 OS 빌드 기준으로 보여줘",
        "온라인 관리 대상 장비를 에이전트 빌드별로 보여줘",
        "에이전트 설치 경로와 빌드를 장비별로 보여줘",
        "Orange The Client 1.6 설치 장비를 에이전트 빌드와 비교해줘",
        "하드웨어 인벤토리를 SYSTEM 제조사 모델 기준으로 보여줘",
        "Samsung 제조사 장비를 모델별로 보여줘",
        "VMware 제조사 장비를 사용자별로 보여줘",
        "메모리 제조사별 장비 수와 용량을 보여줘",
        "Samsung 메모리 장비를 파트번호와 용량 기준으로 보여줘",
        "Micron 메모리 장비를 사용자별로 보여줘",
        "CPU 모델별 장비 수와 코어 수를 보여줘",
        "Intel Core i5 CPU 장비를 OS 빌드별로 보여줘",
        "Intel Core Ultra CPU 장비를 사용자별로 보여줘",
        "AMD Ryzen CPU 장비를 코어 수 기준으로 보여줘",
        "디스크 모델별 장비 수와 용량을 보여줘",
        "SAMSUNG 디스크 모델 장비를 사용자별로 보여줘",
        "VMware Virtual NVMe Disk 장비를 OS 빌드별로 보여줘",
        "C 드라이브 여유 공간 부족 장비를 사용자별로 보여줘",
        "네트워크 어댑터 제조사별 장비 수를 보여줘",
        "Realtek 네트워크 어댑터 장비를 MAC 주소와 사용자 기준으로 보여줘",
        "Intel Wi-Fi 네트워크 어댑터 장비를 사용자별로 보여줘",
        "Bluetooth 네트워크 어댑터 장비 수를 보여줘",
        "MAC 주소가 비어 있는 네트워크 어댑터를 장비별로 보여줘",
        "Microsoft Print To PDF 프린터가 있는 장비를 사용자별로 보여줘",
        "SINDOH 프린터 드라이버 장비를 OS 빌드별로 보여줘",
        "Samsung Universal Print Driver 3 장비를 사용자별로 보여줘",
        "기본 프린터 설정 장비를 프린터 이름별로 보여줘",
        "VMware SVGA 3D 그래픽 카드 장비를 사용자별로 보여줘",
        "Intel Iris Xe Graphics 그래픽 카드 장비를 OS 빌드별로 보여줘",
        "Mirage Driver 그래픽 카드 장비 수를 보여줘",
        "파일 인벤토리 목록을 회사명 제품버전 기준으로 보여줘",
        "파일 인벤토리에서 제품 버전이 비어 있는 파일을 제품명별로 보여줘",
        "파일 인벤토리에서 회사명 없는 실행 파일을 경로별로 보여줘",
        "새로 생성된 exe 파일을 회사명과 경로 기준으로 보여줘",
        "최근 생성된 파일을 제품명 회사명 서명 기준으로 보여줘",
    ]
    scenarios.extend(
        Scenario(
            "endpoint-management-expanded",
            question,
            source="controlled-variant-expanded",
            expected_collections=("filelist",) if "파일 인벤토리" in question or "exe 파일" in question or "서명 없는 파일" in question else ("nodeinfo",),
        )
        for question in endpoint_management_questions
    )
    buckets: dict[str, list[Scenario]] = {}
    for scenario in scenarios:
        buckets.setdefault(scenario.category, []).append(scenario)
    for bucket in buckets.values():
        rng.shuffle(bucket)

    category_order = [
        "inventory-variant",
        "itam-variant",
        "inventory-product-0",
        "inventory-product-1",
        "inventory-product-2",
        "inventory-product-3",
        "process-variant",
        "event-variant",
    ]
    category_order.extend(category for category in buckets if category not in category_order)
    selected: list[Scenario] = []
    while len(selected) < count and any(buckets.values()):
        for category in category_order:
            bucket = buckets.get(category) or []
            if bucket:
                selected.append(bucket.pop())
                if len(selected) >= count:
                    break
    return selected


def limited_scenarios(scenarios: list[Scenario], limit: int) -> list[Scenario]:
    """Return a deterministic category-balanced prefix for bounded smoke runs."""
    if limit <= 0 or limit >= len(scenarios):
        return scenarios
    buckets: dict[str, list[Scenario]] = {}
    for scenario in scenarios:
        buckets.setdefault(scenario.category, []).append(scenario)
    selected: list[Scenario] = []
    category_order = list(buckets)
    while len(selected) < limit and any(buckets.values()):
        for category in category_order:
            bucket = buckets.get(category) or []
            if not bucket:
                continue
            selected.append(bucket.pop(0))
            if len(selected) >= limit:
                break
    return selected


def dataset_reference_time(base_url: str, timeout: float) -> str | None:
    data = request_json("GET", f"{base_url}/api/source-time-range", timeout=timeout)
    return data.get("reference_time")


def run_scenario(base_url: str, scenario: Scenario, timeout: float, reference_time: str | None = None) -> dict[str, Any]:
    endpoint = "/api/mongo-query" if scenario.execute else "/api/mongo-plan"
    payload: dict[str, Any] = {"question": scenario.question}
    if reference_time:
        payload["reference_time"] = reference_time
    started = time.monotonic()
    try:
        response = request_json("POST", f"{base_url}{endpoint}", payload, timeout=timeout)
        elapsed_ms = round((time.monotonic() - started) * 1000, 3)
        collection = (response.get("plan") or {}).get("collection")
        expected_match = not scenario.expected_collections or collection in scenario.expected_collections
        return {
            "category": scenario.category,
            "question": scenario.question,
            "source": scenario.source,
            "ok": bool(response.get("ok")),
            "backend": response.get("backend"),
            "collection": collection,
            "expected_collections": list(scenario.expected_collections),
            "expected_match": expected_match,
            "quality_status": response.get("quality_status"),
            "failure_reason": response.get("failure_reason"),
            "candidate_fix": response.get("candidate_fix"),
            "result_count": response.get("result_count"),
            "elapsed_ms": elapsed_ms,
            "errors": response.get("errors") or [],
        }
    except (urllib.error.URLError, TimeoutError, http.client.RemoteDisconnected, ConnectionError) as exc:
        return {
            "category": scenario.category,
            "question": scenario.question,
            "ok": False,
            "quality_status": "failed",
            "failure_reason": "api_error",
            "elapsed_ms": round((time.monotonic() - started) * 1000, 3),
            "errors": [str(exc)],
        }
    except Exception as exc:
        return {
            "category": scenario.category,
            "question": scenario.question,
            "ok": False,
            "quality_status": "failed",
            "failure_reason": "unexpected_autotest_error",
            "elapsed_ms": round((time.monotonic() - started) * 1000, 3),
            "errors": [str(exc)],
        }


def summarize(rows: list[dict[str, Any]]) -> dict[str, Any]:
    by_quality: dict[str, int] = {}
    by_collection: dict[str, int] = {}
    for row in rows:
        by_quality[row.get("quality_status") or "unknown"] = by_quality.get(row.get("quality_status") or "unknown", 0) + 1
        by_collection[row.get("collection") or "none"] = by_collection.get(row.get("collection") or "none", 0) + 1
    return {
        "total": len(rows),
        "ok": sum(1 for row in rows if row.get("ok")),
        "failed": sum(1 for row in rows if not row.get("ok")),
        "needs_review": sum(1 for row in rows if row.get("quality_status") == "needs_review"),
        "candidate_fix": sum(1 for row in rows if row.get("candidate_fix")),
        "collection_mismatch": sum(1 for row in rows if row.get("expected_match") is False),
        "by_quality": by_quality,
        "by_collection": by_collection,
        "mismatches": [row for row in rows if row.get("expected_match") is False],
        "slowest": sorted(rows, key=lambda row: row.get("elapsed_ms") or 0, reverse=True)[:5],
    }


def write_status(base_url: str, run_id: str, state: str, total: int, rows: list[dict[str, Any]], current: str = "", reference_time: str = "") -> None:
    status = {
        "run_id": run_id,
        "state": state,
        "updated_at": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
        "total": total,
        "completed": len(rows),
        "current_question": current,
        "summary": summarize(rows),
        "recent": rows[-20:],
        "reference_time": reference_time,
    }
    try:
        request_json("POST", f"{base_url}/api/autotest/status", status, timeout=10.0)
    except Exception as exc:
        print(f"autotest status db update failed: {exc}", file=sys.stderr, flush=True)


def main() -> int:
    parser = argparse.ArgumentParser(description="q2 autonomous question tester")
    parser.add_argument("--server", default=DEFAULT_SERVER)
    parser.add_argument("--timeout", type=float, default=150.0)
    parser.add_argument("--generated", type=int, default=0)
    parser.add_argument("--seed", type=int, default=3184)
    parser.add_argument("--limit", type=int, default=0, help="0 means run all generated scenarios")
    parser.add_argument("--output", default="", help="optional debug JSON output path; DB is the primary store")
    parser.add_argument("--reference-time", default="dataset", help="ISO timestamp used as now for relative time ranges, 'dataset' uses source max timestamp, empty uses wall clock")
    args = parser.parse_args()

    base_url = args.server.rstrip("/")
    scenarios = base_scenarios() + generated_scenarios(args.seed, args.generated)
    if args.limit > 0:
        scenarios = limited_scenarios(scenarios, args.limit)
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

    run_id = uuid.uuid4().hex[:12]
    rows: list[dict[str, Any]] = []
    write_status(base_url, run_id, "running", len(scenarios), rows, reference_time=reference_time)
    for index, scenario in enumerate(scenarios, start=1):
        write_status(base_url, run_id, "running", len(scenarios), rows, current=scenario.question, reference_time=reference_time)
        row = run_scenario(base_url, scenario, timeout=args.timeout, reference_time=reference_time or None)
        rows.append(row)
        status = row.get("quality_status") or ("ok" if row.get("ok") else "failed")
        if row.get("expected_match") is False:
            status = "mismatch"
        print(
            f"{index:03d}/{len(scenarios):03d} {status:12s} "
            f"{str(row.get('collection') or '-'):10s} {row.get('elapsed_ms'):9.1f}ms {scenario.question}",
            flush=True,
        )
        write_status(base_url, run_id, "running", len(scenarios), rows, reference_time=reference_time)

    result = {
        "created_at": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
        "server": base_url,
        "summary": summarize(rows),
        "rows": rows,
    }
    write_status(base_url, run_id, "completed", len(scenarios), rows, reference_time=reference_time)
    print(json.dumps(result["summary"], ensure_ascii=False, indent=2))
    if args.output:
        output_path = Path(args.output)
        output_path.parent.mkdir(parents=True, exist_ok=True)
        output_path.write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding="utf-8")
        print(f"wrote debug file {output_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
