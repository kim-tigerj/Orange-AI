#!/usr/bin/env python3
import os
import sys
import signal
import argparse
import ast
import re
import hashlib
import json
import shutil
import time
import uuid
import subprocess
import urllib.error
import urllib.request
from datetime import datetime
from rich.console import Console
from core.handlers import ToolHandler
from context.project_context import ProjectContext
from utils.ast_utils import get_function_source
from prompt_toolkit import PromptSession
from prompt_toolkit.history import FileHistory

console = Console()
Q1_ROOT = os.path.dirname(os.path.abspath(__file__))
PROJECT_ROOT = os.path.dirname(os.path.dirname(Q1_ROOT))
MODEL_ID = "mlx-community/Qwen2.5-Coder-32B-Instruct-8bit"
DEFAULT_SERVER_URL = "http://127.0.0.1:8765"
DEFAULT_SERVER_TIMEOUT = 300
DEFAULT_TASK_MAX_TOKENS = 1024
DEFAULT_IMPROVE_MAX_TOKENS = 900
IMPROVE_SYSTEM_PROMPT = """당신은 q1 함수 단위 자가개선기다.
규칙:
- 사용자가 제공한 대상 함수 본문과 목표만 근거로 판단한다.
- 수정이 필요 없으면 DONE_NO_CHANGE만 출력한다.
- 수정이 필요하면 replace_function 도구 호출 하나만 출력한다.
- 도구 호출은 반드시 <tool_call name="replace_function" path="..." func_name="..."> 형식의 XML 태그다.
- read_function, read_file, list_functions, execute_bash는 호출하지 않는다.
- CLI 메시지는 기존 한국어 스타일과 console.print 사용 규칙을 유지한다.
- 오류를 성공처럼 숨기지 않고 기존 실패 흐름을 유지한다.
"""
IMPROVE_ONCE_SYSTEM_PROMPT = """당신은 q1 자가개선 작업 실행기다.
규칙:
- job 목표와 관련된 코드만 필요한 만큼 읽는다.
- 수정이 필요 없으면 DONE_NO_CHANGE만 출력한다.
- 수정은 replace 또는 replace_function 하나로 최소화한다.
- 도구 호출은 반드시 <tool_call name="replace_function" ...> 또는 <tool_call name="replace" ...> 형식의 XML 태그다.
- execute_bash는 호출하지 않는다.
- 오류를 성공처럼 숨기지 않고 기존 실패 흐름을 유지한다.
"""
SELF_PLAY_SYSTEM_PROMPT = """당신은 q1 정팀장이다. 주어진 함수에 대해 실제 개선이 필요하면 정확히 하나의 write tool_call(replace, replace_function, write_file 중 하나)을 출력한다.
개선이 불필요하면 "DONE_NO_CHANGE"만 출력한다.
그 외 텍스트, 설명, 주석은 출력하지 않는다.
허용 경로 밖, 위험 패턴(os.system, subprocess., shutil.rmtree, eval, exec, __import__, socket.)은 사용하지 않는다.
"""
RESPONSE_LOG = os.path.join(PROJECT_ROOT, "llm", "log", "RESPONSE_HISTORY.md")
PROPOSAL_ROOT = os.path.join(PROJECT_ROOT, "llm", "q1", "proposal")
PROPOSAL_PENDING_DIR = os.path.join(PROPOSAL_ROOT, "pending")
PROPOSAL_ACCEPTED_DIR = os.path.join(PROPOSAL_ROOT, "accepted")
PROPOSAL_REJECTED_DIR = os.path.join(PROPOSAL_ROOT, "rejected")
DATASET_ROOT = os.path.join(PROJECT_ROOT, "llm", "q1", "dataset", "lora")
DEFAULT_IMPROVE_ALLOWED_PATHS = [
    "llm/q1/q1.py",
    "llm/q1/core/handlers.py",
    "llm/q1/core/analyzer.py",
    "llm/q1/context/project_context.py",
    "llm/q1/utils/ast_utils.py",
    "llm/q1/utils/logger.py",
    "llm/q1/q1_server.py",
    "llm/q1/persona/PROMPT.md",
]
DEFAULT_SEED_FUNCTION_JOBS = [
    (
        "llm/q1/q1.py:write_latest_summary",
        "다음 세션이 전체 로그를 읽지 않아도 되도록 latest_summary의 운영 요약 품질을 개선한다.",
    ),
    (
        "llm/q1/q1.py:dry_run_auto",
        "auto-improve 실행 전에 처리 순서와 skip 이유를 더 명확히 출력하도록 개선한다.",
    ),
    (
        "llm/q1/q1.py:cycle_once",
        "한 번의 자율 반복에서 시간 낭비와 불필요한 모델 호출을 줄이도록 개선한다.",
    ),
    (
        "llm/q1/q1.py:compact_logs",
        "다음 세션의 토큰 사용량을 줄이도록 로그 압축 결과와 안전성을 개선한다.",
    ),
    (
        "llm/q1/core/handlers.py:replace_function",
        "함수 단위 자가수정이 실패했을 때 오류 원인을 더 명확히 남기도록 개선한다.",
    ),
    (
        "llm/q1/q1.py:run_validation_suite",
        "반복 개선의 성공/실패 판정이 실제 런타임 결함을 더 잘 잡도록 검증 범위를 개선한다.",
    ),
    (
        "llm/q1/q1.py:write_improve_report",
        "다음 반복이 전체 로그를 읽지 않아도 원인과 결과를 판단하도록 보고서 품질을 개선한다.",
    ),
    (
        "llm/q1/q1.py:server_health",
        "서버 상태 확인이 반복 루프 진입 전 병목과 오류 원인을 더 빠르게 보여주도록 개선한다.",
    ),
    (
        "llm/q1/core/handlers.py:parse_tool_calls",
        "모델이 생성한 tool_call을 quote 혼합, 긴 content, 잘림 상황에서도 더 안정적으로 파싱하도록 개선한다.",
    ),
    (
        "llm/q1/core/handlers.py:execute_tools",
        "도구 실행 실패가 다음 모델 턴과 보고서에 더 명확히 전달되도록 오류 결과를 개선한다.",
    ),
    (
        "llm/q1/q1.py:queue_health",
        "큐가 비었을 때 다음 행동과 병목을 더 명확히 보여주도록 운영 상태 출력을 개선한다.",
    ),
    (
        "llm/q1/q1.py:seed_default_jobs",
        "기본 후보가 모두 소진되어도 안전한 다음 작업을 만들 수 있도록 seed 생성 흐름을 개선한다.",
    ),
    (
        "llm/q1/q1.py:seed_from_failures",
        "실패 보고서를 복구 작업으로 바꿀 때 이미 고쳐진 대상과 반복 실패를 더 잘 건너뛰도록 개선한다.",
    ),
    (
        "llm/q1/q1.py:print_latest_failure",
        "최신 실패 원인을 긴 보고서 없이 빠르게 판단할 수 있도록 실패 요약 출력을 개선한다.",
    ),
    (
        "llm/q1/q1.py:create_function_job",
        "자동 생성 job이 중복과 목표 누락을 줄이도록 함수 job 작성 흐름을 개선한다.",
    ),
    (
        "llm/q1/q1.py:archive_job",
        "완료/실패 job 이동 시 이름 충돌과 재시도 판단을 더 안정적으로 처리하도록 개선한다.",
    ),
    (
        "llm/q1/q1.py:generate_response_server",
        "서버 요청 실패나 지연이 반복 루프 보고서에 더 명확히 남도록 서버 호출 경로를 개선한다.",
    ),
    (
        "llm/q1/q1.py:list_md_files",
        "job/report 파일 목록을 안정적으로 정렬하고 누락 디렉터리를 안전하게 처리하도록 개선한다.",
    ),
    (
        "llm/q1/q1.py:extract_target_function",
        "job 문서에서 대상 함수를 더 안정적으로 추출해 잘못된 skip을 줄이도록 개선한다.",
    ),
    (
        "llm/q1/q1.py:extract_job_goal",
        "job 문서의 목표를 간결하게 추출해 함수 개선 프롬프트 품질을 높이도록 개선한다.",
    ),
    (
        "llm/q1/q1.py:extract_report_field",
        "보고서 필드 추출이 빈 값과 공백에 더 견고하게 동작하도록 개선한다.",
    ),
    (
        "llm/q1/q1.py:snapshot_paths",
        "변경 감지가 누락 파일과 읽기 오류에 더 안정적으로 대응하도록 개선한다.",
    ),
    (
        "llm/q1/q1.py:auto_improve",
        "자동 개선 루프가 실패 job을 격리하면서 다음 작업으로 계속 진행하도록 운영 출력을 개선한다.",
    ),
    (
        "llm/q1/q1.py:archive_skipped",
        "실행 불가능한 job 정리가 반복 루프 전에 더 명확한 보고서를 남기도록 개선한다.",
    ),
    (
        "llm/q1/q1.py:loop",
        "제한 반복 실행에서 각 cycle의 상태와 중단 원인을 더 쉽게 추적하도록 개선한다.",
    ),
    (
        "llm/q1/core/handlers.py:_resolve_path",
        "도구 경로 해석이 프로젝트 루트 경계와 흔한 경로 실수를 더 명확히 처리하도록 개선한다.",
    ),
    (
        "llm/q1/core/handlers.py:_ensure_write_allowed",
        "쓰기 허용 경계 오류가 더 명확한 메시지로 보고되도록 개선한다.",
    ),
    (
        "llm/q1/core/handlers.py:_validate_python_with_context",
        "Python 구문 오류 보고가 모델의 다음 수정에 더 도움이 되도록 주변 문맥 출력을 개선한다.",
    ),
    (
        "llm/q1/core/handlers.py:list_functions",
        "함수 목록 도구가 읽기 오류와 빈 결과를 더 명확히 전달하도록 개선한다.",
    ),
    (
        "llm/q1/core/handlers.py:read_function",
        "함수 읽기 도구가 대상 누락과 경로 오류를 더 명확히 보고하도록 개선한다.",
    ),
    (
        "llm/q1/core/handlers.py:write_file",
        "파일 쓰기 도구가 content 누락과 Python 검증 오류를 더 안전하게 처리하도록 개선한다.",
    ),
    (
        "llm/q1/core/handlers.py:replace",
        "문자열 치환 도구가 match 실패 원인을 더 명확히 보고하도록 개선한다.",
    ),
    (
        "llm/q1/core/handlers.py:read_file",
        "파일 읽기 도구가 범위와 경로 오류를 더 예측 가능하게 처리하도록 개선한다.",
    ),
    (
        "llm/q1/core/handlers.py:execute_bash",
        "명령 실행 도구가 timeout과 출력 길이 정보를 더 명확히 남기도록 개선한다.",
    ),
    (
        "llm/q1/core/handlers.py:list_directory",
        "디렉터리 목록 도구가 누락 경로와 파일/디렉터리 구분을 더 안정적으로 처리하도록 개선한다.",
    ),
]
DISCOVERY_SEED_PATHS = [
    "llm/q1/q1.py",
    "llm/q1/core/handlers.py",
    "llm/q1/core/analyzer.py",
    "llm/q1/context/project_context.py",
    "llm/q1/utils/ast_utils.py",
    "llm/q1/utils/logger.py",
    "llm/q1/q1_server.py",
]
DISCOVERY_EXCLUDED_FUNCTIONS = {
    "restart",
    "signal_handler",
}
DIRECT_TOOL_NAMES = {
    "list_functions", "read_function", "replace_function", "write_file",
    "replace", "read_file", "execute_bash", "list_directory", "list_files",
    "restart_system"
}

def looks_like_direct_task(text):
    if not text:
        return False
    if any(tool in text for tool in DIRECT_TOOL_NAMES):
        return True
    has_path = re.search(r'[\w.-]+(?:/[\w.-]+)+/?', text) is not None
    direct_words = ("목록", "리스트", "디렉터리", "폴더", "나열", "읽", "내용", "확인", "보여", "출력")
    if has_path and any(word in text for word in direct_words):
        return True
    return False
def parse_function_target(target):
    if not target or ":" not in target:
        raise ValueError("function target must use <path>:<function>")
    path, func_name = target.rsplit(":", 1)
    if not path or not func_name:
        raise ValueError("function target must use <path>:<function>")
    return path.strip(), func_name.strip()
def signal_handler(sig, frame):
    console.print("\n[bold bright_yellow]🛑 정팀장 종료[/bold bright_yellow]")
    sys.exit(0)

signal.signal(signal.SIGINT, signal_handler)

class Q1Engine:
    """Orange AI Engine v0.1.94 (Stable Tool Loop)"""
    
    def __init__(self, load_model=True, allowed_write_paths=None, backend="server", server_url=DEFAULT_SERVER_URL, server_timeout=DEFAULT_SERVER_TIMEOUT, improve_max_tokens=DEFAULT_IMPROVE_MAX_TOKENS):
        self.log_dir = os.path.join(PROJECT_ROOT, "llm", "log")
        os.makedirs(self.log_dir, exist_ok=True)
        self.tool_handler = ToolHandler(self.log_dir, PROJECT_ROOT, allowed_write_paths=allowed_write_paths)
        self.backend = backend
        self.server_url = server_url.rstrip("/")
        self.server_timeout = server_timeout
        self.improve_max_tokens = improve_max_tokens
        self.last_generation_elapsed_ms = None
        self.response_cache_dir = os.path.join(PROJECT_ROOT, "llm", "q1", "cache", "responses")
        self.last_response_cache_hit = False
        self.last_generation_streamed = False
        self.persona = ProjectContext(PROJECT_ROOT).get_persona_prompt() + """

## 감독 보강 규칙
- 사용자가 도구 이름을 직접 지정하면 반드시 그 도구를 그대로 사용한다.
- 디렉터리 목록은 list_directory, 파일 전체/일부 읽기는 read_file, 함수 목록은 list_functions, 함수 본문은 read_function이다.
- 도구 결과가 success이고 사용자의 요청이 충족되면 추가 도구를 반복 호출하지 말고 간단히 종료한다.
"""
        self.model = None
        self.tokenizer = None
        self.last_tool_results = []
        if load_model and self.backend == "local":
            try:
                import mlx_lm
                self.model, self.tokenizer = mlx_lm.load(MODEL_ID)
            except Exception as e:
                console.print(f"\n[bold red]모델 로딩 실패: {e}[/bold red]")
                sys.exit(1)

    def save_response(self, response):
        if os.environ.get("Q1_SKIP_RESPONSE_LOG") == "1":
            return
        with open(RESPONSE_LOG, "a", encoding="utf-8") as f:
            f.write(f"\n--- {datetime.now().strftime('%Y-%m-%d %H:%M:%S')} ---\n{response}\n")

    def try_direct_tool_command(self, user_input):
        tool_names = sorted(self.tool_handler.tool_map.keys(), key=len, reverse=True)
        tool_name = next((name for name in tool_names if name in user_input), None)
        if not tool_name:
            tool_name = self.infer_simple_tool(user_input)
        if not tool_name:
            return None

        call = {"name": tool_name}
        if tool_name in {"read_file", "list_directory", "list_files", "list_functions"}:
            path = self.extract_path(user_input)
            if path:
                call["path"] = path
        elif tool_name == "read_function":
            path = self.extract_path(user_input)
            func_name = self.extract_after_label(user_input, "func_name") or self.extract_after_label(user_input, "함수")
            if path:
                call["path"] = path
            if func_name:
                call["func_name"] = func_name
        elif tool_name == "execute_bash":
            command = self.extract_quoted(user_input) or self.extract_after_label(user_input, "command")
            if command:
                call["command"] = command
        else:
            return None

        response = "<tool_call " + " ".join(f'{key}=\"{value}\"' for key, value in call.items()) + " />"
        result = self.tool_handler.execute_tools(response)
        console.print("[bold green]직접 실행 결과:[/bold green]")
        console.print(result, markup=False)
        self.save_response(response)
        return result

    def infer_simple_tool(self, user_input):
        path = self.extract_path(user_input)
        if not path:
            return None

        list_words = ("목록", "리스트", "디렉터리", "폴더", "나열")
        read_words = ("읽", "내용", "확인", "보여", "출력")

        if any(word in user_input for word in list_words):
            return "list_directory"
        elif any(word in user_input for word in read_words):
            return "read_file"
        else:
            return None
    def extract_path(self, text):
        quoted = self.extract_quoted(text)
        if quoted and "/" in quoted:
            return quoted
        matches = re.findall(r'[\w.-]+(?:/[\w.-]+)+/?', text)
        if matches:
            return matches[0]
        else:
            return None
    def extract_quoted(self, text):
        match = re.search(r'["\']([^"\']+)["\']', text)
        return match.group(1) if match else None
    def extract_after_label(self, text, label):
        match = re.search(rf'{re.escape(label)}\s*[:=]\s*([\w.-]+)', text)
        if match:
            return match.group(1)
        else:
            return None
    def chat(self, user_input, max_turns=8, max_tokens=1024, allowed_tools=None, system_prompt=None, require_done_or_tool=False):
        direct_result = self.try_direct_tool_command(user_input)
        if direct_result is not None:
            return

        self.ensure_model_loaded()
        messages = [{"role": "system", "content": system_prompt or self.persona}, {"role": "user", "content": user_input}]
        
        seen_tool_calls = {}
        for turn in range(1, max_turns + 1):
            console.print(f"\n[dim]Thinking...[/dim]")
            full_response = ""
            try:
                full_response = self.generate_response(messages, max_tokens)
                if not self.last_generation_streamed:
                    console.print(full_response, end="", markup=False)
                
                self.save_response(full_response)
                calls = self.tool_handler.parse_tool_calls(full_response)
                if "<tool_call" in full_response and not calls:
                    error_result = (
                        "<tool_result name='parse_tool_calls' status='error'>"
                        "malformed or truncated tool_call"
                        "</tool_result>"
                    )
                    console.print("\n[bold red]도구 호출 파싱 실패:[/bold red]")
                    console.print(error_result, markup=False)
                    self.last_tool_results.append(error_result)
                    break
                if allowed_tools and any(call.get("name") not in allowed_tools for call in calls):
                    blocked = sorted({call.get("name", "(missing)") for call in calls if call.get("name") not in allowed_tools})
                    error_result = (
                        "<tool_result name='allowed_tools' status='error'>"
                        f"disallowed tool call: {', '.join(blocked)}"
                        "</tool_result>"
                    )
                    console.print("\n[yellow]허용되지 않은 도구 호출 감지. 루프를 중단합니다.[/yellow]")
                    console.print(error_result, markup=False)
                    self.last_tool_results.append(error_result)
                    break
                call_signature = repr(calls)
                if calls:
                    seen_tool_calls[call_signature] = seen_tool_calls.get(call_signature, 0) + 1
                    if seen_tool_calls[call_signature] > 1:
                        error_result = (
                            "<tool_result name='tool_loop' status='error'>"
                            "repeated identical tool call"
                            "</tool_result>"
                        )
                        console.print("\n[yellow]동일 도구 호출 반복 감지. 루프를 중단합니다.[/yellow]")
                        console.print(error_result, markup=False)
                        self.last_tool_results.append(error_result)
                        break
                tool_results = self.tool_handler.execute_tools(full_response)
                if not tool_results:
                    if require_done_or_tool and full_response.strip() != "DONE_NO_CHANGE":
                        error_result = (
                            "<tool_result name='final_response' status='error'>"
                            "expected DONE_NO_CHANGE or a valid tool_call"
                            "</tool_result>"
                        )
                        console.print("\n[bold red]자가개선 응답 형식 오류:[/bold red]")
                        console.print(error_result, markup=False)
                        self.last_tool_results.append(error_result)
                        break
                    console.print("\n[yellow]작업 완료 (도구 호출 없음).[/yellow]")
                    break
                console.print("\n[bold green]실행 결과:[/bold green]")
                console.print(tool_results, markup=False)
                self.last_tool_results.append(tool_results)
                write_tools = {"replace_function", "write_file", "replace"}
                if calls and any(call.get("name") in write_tools for call in calls) and "status='success'" in tool_results:
                    console.print("\n[yellow]쓰기 도구 성공. 루프를 종료합니다.[/yellow]")
                    break
                messages.append({"role": "assistant", "content": full_response})
                messages.append({"role": "user", "content": tool_results})
            
            except Exception as e:
                if "q1 server unavailable" in str(e):
                    raise
                console.print(f"\n[bold red]시스템 처리 중 심각 오류 발생: {e}[/bold red]")
                messages.append({"role": "assistant", "content": full_response})
                messages.append({"role": "user", "content": f"System error occurred: {e}"})
        else:
            console.print("\n[bold red]최대 턴 도달. 작업을 중단합니다.[/bold red]")

    def ensure_model_loaded(self):
        if self.backend == "server":
            return
        if self.model is not None and self.tokenizer is not None:
            return
        try:
            import mlx_lm
            self.model, self.tokenizer = mlx_lm.load(MODEL_ID)
        except Exception as e:
            console.print(f"\n[bold red]모델 로딩 실패: {e}[/bold red]")
            sys.exit(1)
    def generate_response(self, messages, max_tokens, *, disable_cache=False):
        if self.backend == "server":
            return self.generate_response_server(messages, max_tokens, disable_cache=disable_cache)
        return self.generate_response_local(messages, max_tokens, disable_cache=disable_cache)

    def generate_response_local(self, messages, max_tokens, *, disable_cache=False):
        self.last_generation_streamed = False
        self.ensure_model_loaded()
        import mlx_lm

        started = time.monotonic()
        prompt = self.tokenizer.apply_chat_template(messages, tokenize=False, add_generation_prompt=True)
        chunks = []
        for response in mlx_lm.stream_generate(self.model, self.tokenizer, prompt, max_tokens=max_tokens):
            chunks.append(response.text)
        text = "".join(chunks)
        self.last_generation_elapsed_ms = int((time.monotonic() - started) * 1000)
        return text
    def generate_response_server(self, messages, max_tokens, *, disable_cache=False):
        self.last_response_cache_hit = False
        self.last_generation_streamed = False
        if not disable_cache:
            cached = self.read_response_cache(messages, max_tokens)
            if cached is not None:
                self.last_generation_elapsed_ms = 0
                self.last_response_cache_hit = True
                return cached
        payload = json.dumps({"messages": messages, "max_tokens": max_tokens}).encode("utf-8")
        request = urllib.request.Request(
            f"{self.server_url}/generate_stream",
            data=payload,
            headers={"Content-Type": "application/json"},
            method="POST",
        )
        started = time.monotonic()
        chunks = []
        try:
            with urllib.request.urlopen(request, timeout=self.server_timeout) as response:
                for raw_line in response:
                    if not raw_line.strip():
                        continue
                    event = json.loads(raw_line.decode("utf-8"))
                    event_type = event.get("event")
                    if event_type == "chunk":
                        text = event.get("text", "")
                        chunks.append(text)
                        self.last_generation_streamed = True
                        console.print(text, end="", markup=False)
                    elif event_type == "error":
                        raise RuntimeError(f"q1 server error: {event.get('error')}")
                    elif event_type == "done":
                        break
        except urllib.error.HTTPError as exc:
            if exc.code == 404:
                return self.generate_response_server_blocking(messages, max_tokens, started=started, disable_cache=disable_cache)
            raise RuntimeError(f"q1 server error: HTTP {exc.code}") from exc
        except urllib.error.URLError as exc:
            raise RuntimeError(f"q1 server unavailable at {self.server_url}: {exc}") from exc
        self.last_generation_elapsed_ms = int((time.monotonic() - started) * 1000)
        text = "".join(chunks)
        if not disable_cache:
            self.write_response_cache(messages, max_tokens, text)
        return text

    def generate_response_server_blocking(self, messages, max_tokens, started=None, *, disable_cache=False):
        payload = json.dumps({"messages": messages, "max_tokens": max_tokens}).encode("utf-8")
        request = urllib.request.Request(
            f"{self.server_url}/generate",
            data=payload,
            headers={"Content-Type": "application/json"},
            method="POST",
        )
        started = started or time.monotonic()
        try:
            with urllib.request.urlopen(request, timeout=self.server_timeout) as response:
                data = json.loads(response.read().decode("utf-8"))
        except urllib.error.URLError as exc:
            raise RuntimeError(f"q1 server unavailable at {self.server_url}: {exc}") from exc
        if not data.get("ok"):
            raise RuntimeError(f"q1 server error: {data.get('error')}")
        self.last_generation_elapsed_ms = int((time.monotonic() - started) * 1000)
        self.last_generation_streamed = False
        text = data.get("text", "")
        if not disable_cache:
            self.write_response_cache(messages, max_tokens, text)
        return text

    def response_cache_enabled(self):
        return os.environ.get("Q1_DISABLE_RESPONSE_CACHE") != "1"

    def response_cache_key(self, messages, max_tokens):
        payload = {
            "server_url": self.server_url,
            "max_tokens": max_tokens,
            "messages": messages,
        }
        encoded = json.dumps(payload, ensure_ascii=False, sort_keys=True, separators=(",", ":")).encode("utf-8")
        return hashlib.sha256(encoded).hexdigest()

    def read_response_cache(self, messages, max_tokens):
        if not self.response_cache_enabled():
            return None
        path = os.path.join(self.response_cache_dir, self.response_cache_key(messages, max_tokens) + ".json")
        try:
            with open(path, "r", encoding="utf-8") as f:
                data = json.load(f)
        except (OSError, json.JSONDecodeError):
            return None
        text = data.get("text")
        if not isinstance(text, str):
            return None
        return text

    def write_response_cache(self, messages, max_tokens, text):
        if not self.response_cache_enabled() or not text:
            return
        os.makedirs(self.response_cache_dir, exist_ok=True)
        path = os.path.join(self.response_cache_dir, self.response_cache_key(messages, max_tokens) + ".json")
        payload = {
            "created_at": datetime.now().strftime("%Y-%m-%d %H:%M:%S"),
            "server_url": self.server_url,
            "max_tokens": max_tokens,
            "text": text,
        }
        tmp_path = path + ".tmp"
        with open(tmp_path, "w", encoding="utf-8") as f:
            json.dump(payload, f, ensure_ascii=False)
        os.replace(tmp_path, path)

    def self_check(self):
        original_ensure_model_loaded = self.ensure_model_loaded
        original_generate_response = self.generate_response
        original_save_response = self.save_response
        previous_tool_results = self.last_tool_results
        self.last_tool_results = []
        try:
            self.ensure_model_loaded = lambda: None
            self.generate_response = lambda messages, max_tokens: (
                '<tool_call name="read_function" path="llm/q1/q1.py" func_name="chat" />'
            )
            self.save_response = lambda response: None
            with console.capture():
                self.chat("allowed tool guard self-check", max_turns=1, allowed_tools={"replace_function"})
            allowed_tool_guard_ok = (
                bool(self.last_tool_results)
                and "name='allowed_tools' status='error'" in self.last_tool_results[-1]
            )
        finally:
            self.ensure_model_loaded = original_ensure_model_loaded
            self.generate_response = original_generate_response
            self.save_response = original_save_response
            self.last_tool_results = previous_tool_results
        checks = [
            ("project_root", PROJECT_ROOT, os.path.exists(os.path.join(PROJECT_ROOT, "README.md"))),
            ("persona", "loaded", "정팀장" in self.persona),
            ("read_file", "llm/q1/q1.py", "status='success'" in self.tool_handler.read_file({"path": "llm/q1/q1.py", "end_line": "3"})),
            ("list_directory", "llm/q1/core", "handlers.py" in self.tool_handler.list_directory({"path": "llm/q1/core"})),
            ("allowed_tool_guard", "disallowed read_function", allowed_tool_guard_ok),
        ]
        failed = [name for name, _, ok in checks if not ok]
        for name, detail, ok in checks:
            console.print(f"[{'green' if ok else 'red'}]{name}: {detail} -> {'OK' if ok else 'FAIL'}[/]")
        if failed:
            console.print(f"[red]Self-check failed for: {', '.join(failed)}[/]")
            raise SystemExit(1)

    def tool_self_test(self):
        tmp_dir = os.path.join(PROJECT_ROOT, "llm", "q1", "tmp")
        os.makedirs(tmp_dir, exist_ok=True)
        rel_path = "llm/q1/tmp/tool_self_test.py"
        full_path = os.path.join(PROJECT_ROOT, rel_path)
        original = "\n".join([
            "class Sample:",
            "    def target(self):",
            "        return 'old'",
            "    def sibling(self):",
            "        return 'sibling'",
            "",
        ])
        with open(full_path, "w", encoding="utf-8") as f:
            f.write(original)
        handler = ToolHandler(self.log_dir, PROJECT_ROOT, allowed_write_paths=[rel_path])
        response = (
            "<tool_call name=\"replace_function\" "
            f"path=\"{rel_path}\" func_name=\"target\" "
            "content='def target(self):\n"
            "    data = {\"key\": \"new\"}\n"
            "    return data['key']\n"
            "' />"
        )
        block_response = (
            "<tool_call name=\"replace_function\" "
            f"path=\"{rel_path}\" func_name=\"target\">"
            "<content>def target(self):\n"
            "    data = {\"key\": \"block\"}\n"
            "    marker = '<tool_call name=\"noop\" />'\n"
            "    return data['key']\n"
            "</content></tool_call>"
        )
        generic_block_response = (
            "<tool_call name=\"replace_function\" "
            f"path=\"{rel_path}\" func_name=\"target\">\n"
            "def target(self):\n"
            "    marker = '<tool_call name=\"noop\" />'\n"
            "    return 'generic'\n"
            "</tool_call>"
        )
        raw_cdata_response = (
            "<tool_call name=\"replace_function\" "
            f"path=\"{rel_path}\" func_name=\"target\">\n"
            "<![CDATA[\n"
            "def target(self):\n"
            "    return 'raw_cdata'\n"
            "]]>\n"
            "</tool_call>"
        )
        close_tag_string_response = (
            "<tool_call name=\"replace_function\" "
            f"path=\"{rel_path}\" func_name=\"target\">\n"
            "def target(self):\n"
            "    marker = '</tool_call>'\n"
            "    return marker\n"
            "</tool_call>"
        )
        parsed = handler.parse_tool_calls(response)
        parsed_block = handler.parse_tool_calls(block_response)
        parsed_generic_block = handler.parse_tool_calls(generic_block_response)
        parsed_raw_cdata = handler.parse_tool_calls(raw_cdata_response)
        parsed_close_tag_string = handler.parse_tool_calls(close_tag_string_response)
        parse_ok = (
            len(parsed) == 1
            and parsed[0].get("name") == "replace_function"
            and parsed[0].get("path") == rel_path
            and parsed[0].get("func_name") == "target"
            and "return data['key']" in parsed[0].get("content", "")
            and len(parsed_block) == 1
            and parsed_block[0].get("name") == "replace_function"
            and parsed_block[0].get("path") == rel_path
            and parsed_block[0].get("func_name") == "target"
            and "block" in parsed_block[0].get("content", "")
            and parsed_block[0].get("content", "").count("<tool_call") == 1
            and len(parsed_generic_block) == 1
            and parsed_generic_block[0].get("name") == "replace_function"
            and parsed_generic_block[0].get("path") == rel_path
            and parsed_generic_block[0].get("func_name") == "target"
            and "generic" in parsed_generic_block[0].get("content", "")
            and parsed_generic_block[0].get("content", "").count("<tool_call") == 1
            and len(parsed_raw_cdata) == 1
            and parsed_raw_cdata[0].get("name") == "replace_function"
            and parsed_raw_cdata[0].get("path") == rel_path
            and parsed_raw_cdata[0].get("func_name") == "target"
            and "raw_cdata" in parsed_raw_cdata[0].get("content", "")
            and "<![CDATA[" not in parsed_raw_cdata[0].get("content", "")
            and len(parsed_close_tag_string) == 1
            and parsed_close_tag_string[0].get("name") == "replace_function"
            and parsed_close_tag_string[0].get("path") == rel_path
            and parsed_close_tag_string[0].get("func_name") == "target"
            and "marker = '</tool_call>'" in parsed_close_tag_string[0].get("content", "")
        )
        result = handler.execute_tools(response)
        ok = parse_ok and "status='success'" in result and result.count("status='success'") == 1
        with open(full_path, "r", encoding="utf-8") as f:
            updated = f.read()
        ok = ok and "    def target(self):" in updated and "return data['key']" in updated and "    def sibling(self):" in updated
        no_newline_response = (
            "<tool_call name=\"replace_function\" "
            f"path=\"{rel_path}\" func_name=\"target\">"
            "<content>"
            "def target(self):\n"
            "    return 'no_newline'"
            "</content></tool_call>"
        )
        no_newline_result = handler.execute_tools(no_newline_response)
        ok = ok and "status='success'" in no_newline_result
        signature_mutation_response = (
            "<tool_call name=\"replace_function\" "
            f"path=\"{rel_path}\" func_name=\"target\">\n"
            "def renamed(self):\n"
            "    return 'bad'\n"
            "</tool_call>"
        )
        signature_mutation_result = handler.execute_tools(signature_mutation_response)
        signature_guard_ok = "status='error'" in signature_mutation_result and "name mismatch" in signature_mutation_result
        decorated_original = "\n".join([
            "class Sample:",
            "    @staticmethod",
            "    def target():",
            "        return 'old'",
            "",
        ])
        with open(full_path, "w", encoding="utf-8") as f:
            f.write(decorated_original)
        missing_decorator_response = (
            "<tool_call name=\"replace_function\" "
            f"path=\"{rel_path}\" func_name=\"target\">\n"
            "def target():\n"
            "    return 'bad'\n"
            "</tool_call>"
        )
        missing_decorator_result = handler.execute_tools(missing_decorator_response)
        decorator_guard_ok = "status='error'" in missing_decorator_result and "decorators" in missing_decorator_result
        return_original = "\n".join([
            "class Sample:",
            "    def target(self):",
            "        return 'old'",
            "",
        ])
        with open(full_path, "w", encoding="utf-8") as f:
            f.write(return_original)
        missing_return_response = (
            "<tool_call name=\"replace_function\" "
            f"path=\"{rel_path}\" func_name=\"target\">\n"
            "def target(self):\n"
            "    value = 'bad'\n"
            "</tool_call>"
        )
        missing_return_result = handler.execute_tools(missing_return_response)
        return_guard_ok = "status='error'" in missing_return_result and "return flow" in missing_return_result
        yield_original = "\n".join([
            "class Sample:",
            "    def target(self):",
            "        yield 'old'",
            "",
        ])
        with open(full_path, "w", encoding="utf-8") as f:
            f.write(yield_original)
        missing_yield_response = (
            "<tool_call name=\"replace_function\" "
            f"path=\"{rel_path}\" func_name=\"target\">\n"
            "def target(self):\n"
            "    return ['bad']\n"
            "</tool_call>"
        )
        missing_yield_result = handler.execute_tools(missing_yield_response)
        yield_guard_ok = "status='error'" in missing_yield_result and "yield flow" in missing_yield_result
        topology_original = "\n".join([
            "class Sample:",
            "    def target(self):",
            "        return 'old'",
            "",
        ])
        with open(full_path, "w", encoding="utf-8") as f:
            f.write(topology_original)
        topology_change_response = (
            "<tool_call name=\"replace_function\" "
            f"path=\"{rel_path}\" func_name=\"target\">\n"
            "def target(self):\n"
            "    def helper():\n"
            "        return 'bad'\n"
            "    return helper()\n"
            "</tool_call>"
        )
        topology_change_result = handler.execute_tools(topology_change_response)
        topology_guard_ok = "status='error'" in topology_change_result and "function topology changed" in topology_change_result
        ok = ok and signature_guard_ok and decorator_guard_ok and return_guard_ok and yield_guard_ok and topology_guard_ok
        compile_result = handler.execute_bash({"command": f"./venv/bin/python -m py_compile {rel_path}"})
        ok = ok and "status='success'" in compile_result
        read_range_result = handler.read_file({"path": rel_path, "start_line": "1", "end_line": "2"})
        ok = ok and "status='success'" in read_range_result and "class Sample:" in read_range_result
        rollback_path = "llm/q1/tmp/tool_self_test_rollback.txt"
        rollback_full_path = os.path.join(PROJECT_ROOT, rollback_path)
        with open(rollback_full_path, "w", encoding="utf-8") as f:
            f.write("before")
        rollback_snapshot = self.snapshot_file_contents([rollback_path])
        with open(rollback_full_path, "w", encoding="utf-8") as f:
            f.write("after")
        rollback_files = self.restore_file_contents(rollback_snapshot, [rollback_path])
        with open(rollback_full_path, "r", encoding="utf-8") as f:
            rollback_content = f.read()
        rollback_ok = rollback_files == [rollback_path] and rollback_content == "before"
        ok = ok and rollback_ok
        import_ok_path = "llm/q1/tmp/import_ok.py"
        import_bad_path = "llm/q1/tmp/import_bad.py"
        with open(os.path.join(PROJECT_ROOT, import_ok_path), "w", encoding="utf-8") as f:
            f.write("VALUE = 1\n")
        with open(os.path.join(PROJECT_ROOT, import_bad_path), "w", encoding="utf-8") as f:
            f.write("raise RuntimeError('import boom')\n")
        import_results = self.verify_changed_python_imports([import_ok_path, import_bad_path])
        import_guard_ok = (
            len(import_results) == 2
            and import_results[0]["ok"]
            and not import_results[1]["ok"]
            and "import boom" in import_results[1]["result"]
        )
        boundary_path = "llm/q1/tmp/post_write_boundary.py"
        boundary_full_path = os.path.join(PROJECT_ROOT, boundary_path)
        boundary_original = "def target(value):\n    return value\n"
        with open(boundary_full_path, "w", encoding="utf-8") as f:
            f.write(boundary_original)
        boundary_ok_result = self.verify_target_boundary_after_write(
            f"{boundary_path}:target",
            boundary_original,
        )
        with open(boundary_full_path, "w", encoding="utf-8") as f:
            f.write("def target(value, extra=None):\n    return value\n")
        boundary_bad_result = self.verify_target_boundary_after_write(
            f"{boundary_path}:target",
            boundary_original,
        )
        post_write_boundary_ok = boundary_ok_result["ok"] and not boundary_bad_result["ok"] and "arguments" in boundary_bad_result["result"]
        ok = ok and import_guard_ok and post_write_boundary_ok
        console.print("[bold]q1 tool self-test[/bold]")
        console.print(f"parse_tool_calls_roundtrip: {'OK' if parse_ok else 'FAIL'}")
        console.print(result, markup=False)
        console.print(no_newline_result, markup=False)
        console.print(signature_mutation_result, markup=False)
        console.print(missing_decorator_result, markup=False)
        console.print(f"replace_function_signature_guard: {'OK' if signature_guard_ok else 'FAIL'}")
        console.print(f"replace_function_decorator_guard: {'OK' if decorator_guard_ok else 'FAIL'}")
        console.print(missing_return_result, markup=False)
        console.print(missing_yield_result, markup=False)
        console.print(f"replace_function_return_guard: {'OK' if return_guard_ok else 'FAIL'}")
        console.print(f"replace_function_yield_guard: {'OK' if yield_guard_ok else 'FAIL'}")
        console.print(topology_change_result, markup=False)
        console.print(f"replace_function_topology_guard: {'OK' if topology_guard_ok else 'FAIL'}")
        console.print(compile_result, markup=False)
        console.print(read_range_result, markup=False)
        console.print(f"post_apply_rollback: {'OK' if rollback_ok else 'FAIL'}")
        console.print(f"changed_python_import_guard: {'OK' if import_guard_ok else 'FAIL'}")
        console.print(f"post_write_boundary_guard: {'OK' if post_write_boundary_ok else 'FAIL'}")
        console.print(f"replace_function_runtime: {'OK' if ok else 'FAIL'}")
        try:
            os.remove(full_path)
        except OSError:
            pass
        try:
            os.remove(rollback_full_path)
        except OSError:
            pass
        for cleanup_path in (import_ok_path, import_bad_path):
            try:
                os.remove(os.path.join(PROJECT_ROOT, cleanup_path))
            except OSError:
                pass
        try:
            os.remove(boundary_full_path)
        except OSError:
            pass
        if not ok:
            raise SystemExit(1)

    def print_status(self):
        job_root = os.path.join(PROJECT_ROOT, "llm", "q1", "job")
        todo_dir = os.path.join(job_root, "todo")
        done_dir = os.path.join(job_root, "done")
        failed_dir = os.path.join(job_root, "failed")
        report_dir = os.path.join(job_root, "report")
        todo = self.list_md_files(todo_dir)
        done = self.list_md_files(done_dir)
        failed = self.list_md_files(failed_dir)
        reports = self.list_md_files(report_dir, exclude={"latest_summary.md"})
        latest_summary = os.path.join(report_dir, "latest_summary.md")

        discovered = self.discover_function_targets()
        seed_summary = self.seed_candidate_summary(discovered=discovered, todo_files=todo)
        seed_candidates = seed_summary["candidates"]

        console.print("[bold]q1 status[/bold]")
        console.print(f"backend: {self.backend}")
        console.print(f"server_url: {self.server_url}")
        console.print(f"todo_count: {len(todo)}")
        console.print(f"done_count: {len(done)}")
        console.print(f"report_count: {len(reports)}")
        console.print(f"latest_summary: {'present' if os.path.exists(latest_summary) else 'missing'}")
        console.print(f"failed_count: {len(failed)}")
        console.print(f"discovered_target_count: {len(discovered)}")
        console.print(f"seed_candidate_count: {len(seed_candidates)}")
        console.print(f"seed_rejected_existing_todo: {seed_summary['rejected_existing_todo']}")
        console.print(f"seed_rejected_recent_done_or_no_change: {seed_summary['rejected_recent_done_or_no_change']}")
        console.print(f"seed_rejected_failed: {seed_summary['rejected_failed']}")
        if seed_candidates:
            console.print(f"seed_next_candidate: {seed_candidates[0]}")
        if todo:
            console.print(f"next_todo: {todo[0]}")
        elif not seed_candidates:
            console.print(f"next_action: {self.next_manual_job_command()}")
        if os.path.exists(latest_summary):
            with open(latest_summary, "r", encoding="utf-8") as f:
                summary = f.read()
            next_job = self.extract_report_field(summary, "next_recommended_job")
            latest_verdict = self.extract_report_field(summary, "latest_verdict")
            console.print(f"latest_verdict: {latest_verdict}")
            console.print(f"next_recommended_job: {next_job}")

    def print_jobs(self):
        job_root = os.path.join(PROJECT_ROOT, "llm", "q1", "job")
        groups = [
            ("todo", os.path.join(job_root, "todo")),
            ("done", os.path.join(job_root, "done")),
            ("failed", os.path.join(job_root, "failed")),
            ("report", os.path.join(job_root, "report")),
        ]
        for label, directory in groups:
            try:
                files = self.list_md_files(directory)
                console.print(f"[bold]{label} ({len(files)})[/bold]")
                if not files:
                    console.print("- (empty)")
                for name in files:
                    console.print(f"- {name}")
            except OSError as e:
                console.print(f"[bold red]Error in {label}: {str(e)}[/bold red]")
    def server_health(self):
        started = time.monotonic()
        request = urllib.request.Request(f"{self.server_url}/health", method="GET")
        try:
            with urllib.request.urlopen(request, timeout=self.server_timeout) as response:
                body = response.read().decode("utf-8")
                data = json.loads(body)
        except (urllib.error.URLError, TimeoutError, json.JSONDecodeError) as exc:
            elapsed_ms = int((time.monotonic() - started) * 1000)
            console.print("[bold red]q1 server health: FAIL[/bold red]")
            console.print(f"url: {self.server_url}")
            console.print(f"elapsed_ms: {elapsed_ms}")
            console.print(f"error: {exc}")
            raise SystemExit(1)

        elapsed_ms = int((time.monotonic() - started) * 1000)
        ok = bool(data.get("ok"))
        console.print(f"[bold {'green' if ok else 'red'}]q1 server health: {'OK' if ok else 'FAIL'}[/bold {'green' if ok else 'red'}]")
        console.print(f"url: {self.server_url}")
        console.print(f"elapsed_ms: {elapsed_ms}")
        console.print(f"model: {data.get('model', '(missing)')}")
        console.print(f"queue_depth: {data.get('queue_depth', '(missing)')}")
        console.print(f"active_job_id: {data.get('active_job_id', '(none)')}")
        console.print(f"cache_entries: {data.get('cache_entries', '(missing)')}")
        console.print(f"worker_alive: {data.get('worker_alive', '(missing)')}")
        if not ok:
            console.print(f"status: {data.get('status', '(missing)')}")
            console.print(f"message: {data.get('message', '(missing)')}")
            raise SystemExit(1)
    def _check_server_health(self):
        request = urllib.request.Request(f"{self.server_url}/health", method="GET")
        try:
            with urllib.request.urlopen(request, timeout=5) as response:
                data = json.loads(response.read().decode("utf-8"))
        except (urllib.error.URLError, TimeoutError, json.JSONDecodeError):
            return False
        return bool(data.get("ok"))
    def server_host_port(self):
        match = re.match(r"^https?://([^/:]+)(?::(\d+))?", self.server_url)
        if not match:
            return "127.0.0.1", 8765
        return match.group(1), int(match.group(2) or 80)
    def server_listener_pids(self, port):
        try:
            result = subprocess.run(
                ["lsof", "-nP", f"-iTCP:{port}", "-sTCP:LISTEN", "-t"],
                capture_output=True,
                text=True,
                timeout=2,
            )
        except (OSError, subprocess.TimeoutExpired):
            return []
        if result.returncode not in {0, 1}:
            return []
        pids = []
        for line in result.stdout.splitlines():
            try:
                pids.append(int(line.strip()))
            except ValueError:
                continue
        return sorted(set(pids))
    def process_command(self, pid):
        try:
            result = subprocess.run(
                ["ps", "-p", str(pid), "-o", "command="],
                capture_output=True,
                text=True,
                timeout=2,
            )
        except (OSError, subprocess.TimeoutExpired):
            return ""
        return result.stdout.strip() if result.returncode == 0 else ""
    def stop_server_processes(self, port):
        pids = [
            pid for pid in self.server_listener_pids(port)
            if "q1_server.py" in self.process_command(pid)
        ]
        for pid in pids:
            try:
                os.kill(pid, signal.SIGTERM)
                console.print(f"stopping q1 server pid: {pid}")
            except ProcessLookupError:
                pass
        deadline = time.monotonic() + 20
        while time.monotonic() < deadline:
            alive = [pid for pid in pids if self.process_command(pid)]
            if not alive:
                return pids
            if not any(pid in self.server_listener_pids(port) for pid in alive):
                return pids
            time.sleep(0.2)
        for pid in pids:
            if self.process_command(pid):
                try:
                    os.kill(pid, signal.SIGKILL)
                    console.print(f"force killed q1 server pid: {pid}")
                except ProcessLookupError:
                    pass
        return pids
    def wait_for_server_health(self, timeout=120):
        deadline = time.monotonic() + timeout
        last_error = None
        while time.monotonic() < deadline:
            request = urllib.request.Request(f"{self.server_url}/health", method="GET")
            try:
                with urllib.request.urlopen(request, timeout=2) as response:
                    data = json.loads(response.read().decode("utf-8"))
                    request_id = response.headers.get("X-Q1-Request-Id", "(missing)")
                if data.get("ok"):
                    return data, request_id
            except (OSError, urllib.error.URLError, TimeoutError, json.JSONDecodeError) as exc:
                last_error = exc
            time.sleep(1)
        raise RuntimeError(f"q1 server did not become healthy: {last_error}")
    def restart_server(self):
        host, port = self.server_host_port()
        stopped = self.stop_server_processes(port)
        log_path = os.path.join(PROJECT_ROOT, "server.log")
        python_path = os.path.join(PROJECT_ROOT, "venv", "bin", "python")
        if not os.path.exists(python_path):
            python_path = sys.executable
        command = [
            python_path,
            os.path.join(PROJECT_ROOT, "llm", "q1", "q1_server.py"),
            "--host", host,
            "--port", str(port),
        ]
        with open(log_path, "ab") as log_file:
            process = subprocess.Popen(
                command,
                cwd=PROJECT_ROOT,
                stdout=log_file,
                stderr=subprocess.STDOUT,
                start_new_session=True,
            )
        data, request_id = self.wait_for_server_health()
        console.print("[bold green]q1 server restarted[/bold green]")
        console.print(f"stopped_pids: {', '.join(str(pid) for pid in stopped) if stopped else '(none)'}")
        console.print(f"started_pid: {process.pid}")
        console.print(f"url: {self.server_url}")
        console.print(f"model: {data.get('model', '(missing)')}")
        console.print(f"uptime_seconds: {data.get('uptime_seconds', '(missing)')}")
        console.print(f"request_id_header: {request_id}")
        console.print(f"log: {os.path.relpath(log_path, PROJECT_ROOT)}")

    def print_latest_report(self):
        path = os.path.join(PROJECT_ROOT, "llm", "q1", "job", "report", "latest_summary.md")
        self.write_latest_summary()
        if not os.path.exists(path):
            console.print("[bold red]latest_summary.md missing[/bold red]")
            raise SystemExit(1)
        with open(path, "r", encoding="utf-8") as f:
            content = f.read().rstrip()
        console.print(content)
    def create_function_job(self, target, goal):
        path, func_name = parse_function_target(target)
        todo_dir = os.path.join(PROJECT_ROOT, "llm", "q1", "job", "todo")
        os.makedirs(todo_dir, exist_ok=True)
        safe_path = path.replace("/", "_").replace(".", "_")
        job_path = os.path.join(todo_dir, f"function_{safe_path}__{func_name}.md")
    
        # Check for existing job files and avoid duplicates
        existing_jobs = [f for f in os.listdir(todo_dir) if f.startswith(f"function_{safe_path}__{func_name}")]
        if existing_jobs:
            # Check if the goal is already covered in existing jobs
            goal_covered = False
            for job in existing_jobs:
                with open(os.path.join(todo_dir, job), "r", encoding="utf-8") as f:
                    job_content = f.read()
                    if f"## Goal\n{goal}" in job_content:
                        goal_covered = True
                        break
            if goal_covered:
                console.print(f"[bold yellow]Job already exists with the same goal:[/bold yellow] {job_path}")
                return "DONE_NO_CHANGE"
    
        # If no existing job covers the goal, create a new job file
        if os.path.exists(job_path):
            job_path = os.path.join(todo_dir, f"function_{safe_path}__{func_name}_{datetime.now().strftime('%Y%m%d_%H%M%S')}.md")
    
        content = "\n".join([
            f"# q1 Function Job: {target}",
            "",
            "## Goal",
            goal,
            "",
            "## Target Function",
            f"- `{target}`",
            "",
            "## Required Behavior",
            f"- 전체 파일을 읽지 말고 `read_function`으로 `{func_name}`만 읽는다.",
            f"- 수정이 필요하면 `replace_function`으로 `{func_name}` 함수 하나만 교체한다.",
            "- 수정이 불필요하면 `DONE_NO_CHANGE`만 출력하고 멈춘다.",
            "",
            "## Done Criteria",
            "- Python compile 통과",
            "- `--self-check` 통과",
            "- 직접 task 검증 통과",
            "",
        ])
        with open(job_path, "w", encoding="utf-8") as f:
            f.write(content)
        console.print(f"[bold green]created job:[/bold green] {job_path}")
    def seed_default_jobs(self, count):
        if count < 1:
            console.print("[bold red]--seed-count must be >= 1[/bold red]")
            raise SystemExit(1)
        todo_dir = os.path.join(PROJECT_ROOT, "llm", "q1", "job", "todo")
        os.makedirs(todo_dir, exist_ok=True)
        existing_targets = set()
        for name in self.list_md_files(todo_dir):
            with open(os.path.join(todo_dir, name), "r", encoding="utf-8") as f:
                target = self.extract_target_function(f.read())
            if target:
                existing_targets.add(target)

        recent_verdicts = self.recent_target_verdicts()
        failed_targets = self.failed_job_targets()
        created = 0
        skipped = 0
        skipped_recent = 0
        skipped_failed = 0
        for target, goal in DEFAULT_SEED_FUNCTION_JOBS:
            if created >= count:
                break
            if target in existing_targets:
                skipped += 1
                continue
            if target in failed_targets:
                skipped_failed += 1
                continue
            if recent_verdicts.get(target) in {"DONE", "NO_CHANGE"}:
                skipped_recent += 1
                continue
            self.create_function_job(target, goal)
            existing_targets.add(target)
            created += 1

        console.print("[bold]q1 seed default jobs[/bold]")
        console.print(f"created: {created}")
        console.print(f"skipped_existing: {skipped}")
        console.print(f"skipped_recent_done_or_no_change: {skipped_recent}")
        console.print(f"skipped_failed: {skipped_failed}")
        if created == 0:
            console.print(f"next_action: {self.next_manual_job_command()}")

    def discover_function_targets(self):
        discovered = []
        for rel_path in DISCOVERY_SEED_PATHS:
            full_path = os.path.join(PROJECT_ROOT, rel_path)
            if not os.path.exists(full_path):
                continue
            with open(full_path, "r", encoding="utf-8") as f:
                source = f.read()
            try:
                tree = ast.parse(source)
            except SyntaxError as e:
                console.print(f"[yellow]discovery skipped syntax error:[/yellow] {rel_path}: {e}")
                continue
            candidates = [
                node for node in tree.body
                if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef))
            ]
            for class_node in [node for node in tree.body if isinstance(node, ast.ClassDef)]:
                candidates.extend(
                    node for node in class_node.body
                    if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef))
                )
            for node in candidates:
                if node.name == "__init__" or node.name in DISCOVERY_EXCLUDED_FUNCTIONS:
                    continue
                discovered.append(f"{rel_path}:{node.name}")
        return sorted(set(discovered))
    def seed_candidate_summary(self, discovered=None, todo_files=None):
        todo_dir = os.path.join(PROJECT_ROOT, "llm", "q1", "job", "todo")
        discovered = list(discovered if discovered is not None else self.discover_function_targets())
        todo_files = list(todo_files if todo_files is not None else self.list_md_files(todo_dir))
        existing_targets = set()
        for name in todo_files:
            try:
                with open(os.path.join(todo_dir, name), "r", encoding="utf-8") as f:
                    target = self.extract_target_function(f.read())
            except OSError:
                target = None
            if target:
                existing_targets.add(target)
        recent_verdicts = self.recent_target_verdicts()
        failed_targets = self.failed_job_targets()
        candidates = []
        rejected_existing_todo = 0
        rejected_recent = 0
        rejected_failed = 0
        for target in discovered:
            if target in existing_targets:
                rejected_existing_todo += 1
                continue
            if target in failed_targets:
                rejected_failed += 1
                continue
            if recent_verdicts.get(target) in {"DONE", "NO_CHANGE"}:
                rejected_recent += 1
                continue
            candidates.append(target)
        return {
            "discovered_count": len(discovered),
            "candidate_count": len(candidates),
            "candidates": candidates,
            "rejected_existing_todo": rejected_existing_todo,
            "rejected_recent_done_or_no_change": rejected_recent,
            "rejected_failed": rejected_failed,
            "sample_candidates": candidates[:3],
        }
    def seed_discovered_jobs(self, count):
        if count < 1:
            console.print("[bold red]--seed-count must be >= 1[/bold red]")
            raise SystemExit(1)
        todo_dir = os.path.join(PROJECT_ROOT, "llm", "q1", "job", "todo")
        os.makedirs(todo_dir, exist_ok=True)
        existing_targets = set()
        for name in self.list_md_files(todo_dir):
            with open(os.path.join(todo_dir, name), "r", encoding="utf-8") as f:
                target = self.extract_target_function(f.read())
            if target:
                existing_targets.add(target)

        recent_verdicts = self.recent_target_verdicts()
        failed_targets = self.failed_job_targets()
        created = 0
        skipped_existing = 0
        skipped_recent = 0
        skipped_failed = 0
        discovered = self.discover_function_targets()
        for target in discovered:
            if created >= count:
                break
            if target in existing_targets:
                skipped_existing += 1
                continue
            if target in failed_targets:
                skipped_failed += 1
                continue
            if recent_verdicts.get(target) in {"DONE", "NO_CHANGE"}:
                skipped_recent += 1
                continue
            goal = "자동 발견된 함수 후보를 대상으로 안정성, 오류 보고, 반복 실행 비용을 개선한다."
            self.create_function_job(target, goal)
            existing_targets.add(target)
            created += 1

        console.print("[bold]q1 seed discovered jobs[/bold]")
        console.print(f"discovered: {len(discovered)}")
        console.print(f"created: {created}")
        console.print(f"skipped_existing: {skipped_existing}")
        console.print(f"skipped_recent_done_or_no_change: {skipped_recent}")
        console.print(f"skipped_failed: {skipped_failed}")
    def static_signal_targets(self, min_statements=8):
        targets = []
        for rel_path in DISCOVERY_SEED_PATHS:
            full_path = os.path.join(PROJECT_ROOT, rel_path)
            if not os.path.exists(full_path):
                continue
            with open(full_path, "r", encoding="utf-8") as f:
                source = f.read()
            try:
                tree = ast.parse(source)
            except SyntaxError:
                continue
            candidates = [
                node for node in tree.body
                if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef))
            ]
            for class_node in [node for node in tree.body if isinstance(node, ast.ClassDef)]:
                candidates.extend(
                    node for node in class_node.body
                    if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef))
                )
            for node in candidates:
                if node.name == "__init__" or node.name in DISCOVERY_EXCLUDED_FUNCTIONS:
                    continue
                has_return_type = node.returns is not None
                has_arg_types = all(
                    arg.annotation is not None
                    for arg in list(node.args.posonlyargs) + list(node.args.args) + list(node.args.kwonlyargs)
                    if arg.arg not in {"self", "cls"}
                )
                statement_count = sum(
                    1 for child in ast.walk(node)
                    if isinstance(child, ast.stmt)
                )
                if statement_count >= min_statements and (not has_return_type or not has_arg_types):
                    targets.append({
                        "target": f"{rel_path}:{node.name}",
                        "statement_count": statement_count,
                        "missing_return_type": not has_return_type,
                        "missing_arg_types": not has_arg_types,
                    })
        return sorted(targets, key=lambda item: (-item["statement_count"], item["target"]))
    def seed_static_jobs(self, count):
        if count < 1:
            console.print("[bold red]--seed-count must be >= 1[/bold red]")
            raise SystemExit(1)
        todo_dir = os.path.join(PROJECT_ROOT, "llm", "q1", "job", "todo")
        os.makedirs(todo_dir, exist_ok=True)
        existing_targets = set()
        for name in self.list_md_files(todo_dir):
            with open(os.path.join(todo_dir, name), "r", encoding="utf-8") as f:
                target = self.extract_target_function(f.read())
            if target:
                existing_targets.add(target)
        failed_targets = self.failed_job_targets()
        created = 0
        skipped_existing = 0
        skipped_failed = 0
        skipped_no_change_goal = 0
        candidates = self.static_signal_targets()
        for item in candidates:
            if created >= count:
                break
            target = item["target"]
            goal = (
                "정적 분석 신호(static:untyped_long)를 바탕으로 함수 경계를 유지하면서 "
                "명확한 타입 힌트나 오류 진단 개선 여지가 있는지 평가한다."
            )
            if target in existing_targets:
                skipped_existing += 1
                continue
            if target in failed_targets:
                skipped_failed += 1
                continue
            if self.has_no_change_report(target, goal):
                skipped_no_change_goal += 1
                continue
            self.create_function_job(target, goal)
            existing_targets.add(target)
            created += 1

        console.print("[bold]q1 seed static jobs[/bold]")
        console.print(f"static_candidates: {len(candidates)}")
        console.print(f"created: {created}")
        console.print(f"skipped_existing: {skipped_existing}")
        console.print(f"skipped_failed: {skipped_failed}")
        console.print(f"skipped_no_change_same_goal: {skipped_no_change_goal}")
    def failed_job_targets(self):
        failed_dir = os.path.join(PROJECT_ROOT, "llm", "q1", "job", "failed")
        targets = set()
        for name in self.list_md_files(failed_dir):
            file_path = os.path.join(failed_dir, name)
            try:
                with open(file_path, "r", encoding="utf-8") as f:
                    content = f.read()
            except OSError:
                continue
            target = self.extract_target_function(content)
            if target:
                targets.add(target)
        return targets

    def archive_resolved_failures(self):
        failed_dir = os.path.join(PROJECT_ROOT, "llm", "q1", "job", "failed")
        resolved_dir = os.path.join(failed_dir, "resolved")
        os.makedirs(resolved_dir, exist_ok=True)
        latest_reports = self.latest_report_by_target()
        discovered_targets = set(self.discover_function_targets())
        moved = 0
        kept = 0
        skipped = 0
        for name in self.list_md_files(failed_dir):
            source_path = os.path.join(failed_dir, name)
            try:
                with open(source_path, "r", encoding="utf-8", errors="replace") as f:
                    content = f.read()
            except OSError as exc:
                console.print(f"[yellow]failed job skipped:[/yellow] {name}: {exc}")
                skipped += 1
                continue
            target = self.extract_target_function(content)
            latest = latest_reports.get(target) if target else None
            obsolete_target = bool(target and target not in discovered_targets and not latest)
            if not obsolete_target and (not latest or latest.get("verdict") not in {"DONE", "NO_CHANGE"}):
                kept += 1
                continue
            dest_path = os.path.join(resolved_dir, name)
            if os.path.exists(dest_path):
                base, ext = os.path.splitext(name)
                dest_path = os.path.join(resolved_dir, f"{base}_{datetime.now().strftime('%Y%m%d_%H%M%S')}{ext}")
            os.replace(source_path, dest_path)
            moved += 1
        console.print("[bold]q1 archive resolved failures[/bold]")
        console.print(f"moved: {moved}")
        console.print(f"kept: {kept}")
        console.print(f"skipped: {skipped}")
        self.write_latest_summary()

    def existing_todo_targets(self):
        todo_dir = os.path.join(PROJECT_ROOT, "llm", "q1", "job", "todo")
        targets = set()
        for name in self.list_md_files(todo_dir):
            file_path = os.path.join(todo_dir, name)
            try:
                with open(file_path, "r", encoding="utf-8", errors="replace") as f:
                    content = f.read()
            except OSError as exc:
                console.print(f"[yellow]todo target skipped:[/yellow] {name}: {exc}")
                continue

            target = self.extract_target_function(content)
            if target:
                targets.add(target)
        return targets

    def latest_report_by_target(self):
        report_dir = os.path.join(PROJECT_ROOT, "llm", "q1", "job", "report")
        if not os.path.exists(report_dir):
            return {}
        report_files = [
            os.path.join(report_dir, name)
            for name in self.list_md_files(report_dir, exclude={"latest_summary.md"})
        ]
        report_files.sort(key=os.path.getmtime, reverse=True)
        latest = {}
        for path in report_files:
            try:
                with open(path, "r", encoding="utf-8", errors="replace") as f:
                    text = f.read()
            except OSError as exc:
                console.print(f"[yellow]report skipped:[/yellow] {os.path.basename(path)}: {exc}")
                continue
            job = self.extract_report_field(text, "job")
            if not job.startswith("function:"):
                continue
            target = job.replace("function:", "", 1)
            if target not in latest:
                latest[target] = {
                    "path": path,
                    "verdict": self.extract_report_field(text, "verdict"),
                    "changed_files": self.extract_report_field(text, "changed_files"),
                    "goal": self.extract_report_field(text, "goal"),
                    "target_source_hash": self.extract_report_field(text, "target_source_hash"),
                    "text": text,
                }
        return latest

    def retry_goal_for_failure(self, target, report_text):
        if "Unsafe generated content blocked" in report_text:
            return "이전 안전 차단 실패를 반영해 일반 print, 위험 예시, 불필요한 예외 숨김 없이 목표만 작게 수행한다."
        if "CDATA" in report_text:
            return "이전 CDATA 형식 실패를 반복하지 않도록 block content 포맷만 사용해 함수 개선 여부를 다시 판단한다."
        if "f-string expression part cannot include a backslash" in report_text:
            return "이전 f-string backslash 실패를 반복하지 않도록 문자열 조립을 단순화하고 필요한 경우만 최소 수정한다."
        if "invalid syntax" in report_text or "SyntaxError" in report_text:
            return "이전 구문 오류 실패를 반복하지 않도록 함수 본문 하나만 읽고 문법적으로 안전한 최소 변경만 수행한다."
        if "malformed or truncated tool_call" in report_text:
            return "이전 tool_call 파싱 실패를 반복하지 않도록 긴 속성 content를 피하고 필요 시 block content 포맷만 사용한다."
        return "이전 실패 이후 guard와 검증이 보강됐으므로 같은 실수를 반복하지 않고 안전한 최소 개선만 다시 시도한다."

    def suppress_retry_for_failure(self, target, report_text):
        changed_files = self.extract_report_field(report_text, "changed_files")
        if (
            changed_files == "(none)"
            and "malformed or truncated tool_call" in report_text
            and target.endswith(":write_latest_summary")
        ):
            return True
        return False

    def seed_retryable_failures(self, count):
        if count < 1:
            console.print("[bold red]--seed-count must be >= 1[/bold red]")
            raise SystemExit(1)
        failed_targets = sorted(self.failed_job_targets())
        existing_targets = self.existing_todo_targets()
        latest_reports = self.latest_report_by_target()
        recent_verdicts = self.recent_target_verdicts()
        created = 0
        skipped_existing = 0
        skipped_repaired = 0
        skipped_no_report = 0
        skipped_not_continue = 0
        for target in failed_targets:
            if created >= count:
                break
            if target in existing_targets:
                skipped_existing += 1
                continue
            recent = recent_verdicts.get(target)
            if recent in {"DONE", "NO_CHANGE"}:
                skipped_repaired += 1
                continue
            latest = latest_reports.get(target)
            if not latest:
                skipped_no_report += 1
                continue
            verdict = latest.get("verdict")
            if verdict in {"DONE", "NO_CHANGE"}:
                skipped_repaired += 1
                continue
            if verdict != "CONTINUE":
                skipped_not_continue += 1
                continue
            if self.suppress_retry_for_failure(target, latest.get("text", "")):
                skipped_not_continue += 1
                continue
            goal = self.retry_goal_for_failure(target, latest.get("text", ""))
            self.create_function_job(target, goal)
            existing_targets.add(target)
            created += 1

        console.print("[bold]q1 seed retryable failures[/bold]")
        console.print(f"failed_targets: {len(failed_targets)}")
        console.print(f"created: {created}")
        console.print(f"skipped_existing: {skipped_existing}")
        console.print(f"skipped_repaired: {skipped_repaired}")
        console.print(f"skipped_no_report: {skipped_no_report}")
        console.print(f"skipped_not_continue: {skipped_not_continue}")

    def seed_from_failures(self, count):
        if count < 1:
            console.print("[bold red]--seed-count must be >= 1[/bold red]")
            raise SystemExit(1)
        report_dir = os.path.join(PROJECT_ROOT, "llm", "q1", "job", "report")
        todo_dir = os.path.join(PROJECT_ROOT, "llm", "q1", "job", "todo")
        os.makedirs(todo_dir, exist_ok=True)
        existing_targets = set()
        for name in self.list_md_files(todo_dir):
            with open(os.path.join(todo_dir, name), "r", encoding="utf-8") as f:
                target = self.extract_target_function(f.read())
            if target:
                existing_targets.add(target)

        report_files = [
            os.path.join(report_dir, name)
            for name in self.list_md_files(report_dir, exclude={"latest_summary.md"})
        ]
        report_files.sort(key=os.path.getmtime, reverse=True)

        recent_verdicts = self.recent_target_verdicts()
        created = 0
        skipped = 0
        skipped_repaired = 0
        for path in report_files:
            if created >= count:
                break
            with open(path, "r", encoding="utf-8", errors="replace") as f:
                text = f.read()
            if self.extract_report_field(text, "verdict") != "CONTINUE":
                continue
            repair = self.classify_failure_repair(text)
            if not repair:
                skipped += 1
                continue
            target, goal = repair
            if target in existing_targets:
                skipped += 1
                continue
            if recent_verdicts.get(target) in {"DONE", "NO_CHANGE", "FAILED"}:
                skipped_repaired += 1
                continue
            self.create_function_job(target, goal)
            existing_targets.add(target)
            created += 1

        console.print("[bold]q1 seed from failures[/bold]")
        console.print(f"created: {created}")
        console.print(f"skipped: {skipped}")
        console.print(f"skipped_repaired: {skipped_repaired}")
    def classify_failure_repair(self, report_text):
        if "function:llm/q1/q1.py:improve_once" in report_text and (
            "malformed or truncated tool_call" in report_text
            or "expected DONE_NO_CHANGE or a valid tool_call" in report_text
        ):
            return (
                "llm/q1/q1.py:improve_once",
                "자가개선 1회 모드가 자유 텍스트 의사 명령을 만들지 않고 DONE_NO_CHANGE 또는 올바른 XML tool_call만 출력하도록 프롬프트와 허용 도구 경계를 개선한다.",
            )
        if "Unsafe generated content blocked" in report_text:
            return (
                "llm/q1/core/handlers.py:_reject_unsafe_generated_content",
                "위험 생성물 차단 규칙이 반복 실패를 줄이도록 차단 사유와 허용 경계를 더 명확히 개선한다.",
            )
        if "path, func_name, and content are required" in report_text:
            return (
                "llm/q1/core/handlers.py:parse_tool_calls",
                "긴 content와 quote 혼합 tool_call에서 path, func_name, content를 안정적으로 보존하도록 파서를 개선한다.",
            )
        if "f-string: unmatched" in report_text or "f-string expression part cannot include a backslash" in report_text:
            return (
                "llm/q1/q1.py:improve_function",
                "함수 개선 프롬프트가 f-string quote 충돌을 만들지 않도록 더 강한 예시와 금지 규칙을 포함하게 개선한다.",
            )
        if "'/Users/tigerj/Work/OrangeLabs/Orange/Orange-AI/{path}'" in report_text or ("No such file or directory" in report_text and "{path}" in report_text):
            return (
                "llm/q1/q1.py:improve_function",
                "함수 개선 프롬프트가 리터럴 {path}, {func_name}, ... 예시를 모델 출력에 복사하지 않도록 실제 대상값 사용 규칙을 강화한다.",
            )
        if "malformed or truncated tool_call" in report_text or "EOL while scanning string literal" in report_text:
            return (
                "llm/q1/core/handlers.py:parse_tool_calls",
                "잘리거나 quote가 섞인 tool_call을 더 명확히 오류 처리하고 다음 반복이 원인을 알 수 있게 개선한다.",
            )
        return None

    def next_manual_job_command(self):
        report_dir = os.path.join(PROJECT_ROOT, "llm", "q1", "job", "report")
        latest_by_target = self.latest_report_by_target()
        no_change_goal_pairs = set()
        if os.path.exists(report_dir):
            report_files = [
                os.path.join(report_dir, name)
                for name in self.list_md_files(report_dir, exclude={"latest_summary.md"})
            ]
            report_files.sort(key=os.path.getmtime, reverse=True)
            for path in report_files:
                try:
                    with open(path, "r", encoding="utf-8", errors="replace") as f:
                        text = f.read()
                except OSError:
                    continue
                job = self.extract_report_field(text, "job")
                if job.startswith("function:") and self.extract_report_field(text, "verdict") == "NO_CHANGE":
                    goal = self.extract_report_field(text, "goal")
                    if goal != "(missing)":
                        no_change_goal_pairs.add((job.replace("function:", "", 1), goal))
            for path in report_files:
                try:
                    with open(path, "r", encoding="utf-8", errors="replace") as f:
                        text = f.read()
                except OSError:
                    continue
                job = self.extract_report_field(text, "job")
                if job.startswith("function:"):
                    target = job.replace("function:", "", 1)
                    latest = latest_by_target.get(target)
                    if (
                        latest
                        and os.path.abspath(latest.get("path", "")) != os.path.abspath(path)
                        and latest.get("verdict") in {"DONE", "NO_CHANGE"}
                    ):
                        continue
                verdict = self.extract_report_field(text, "verdict")
                if verdict not in {"CONTINUE", "FAIL"}:
                    continue
                repair = self.classify_failure_repair(text)
                if repair:
                    target, goal = repair
                    return f'--new-function-job {target} --goal "{goal}"'
        fallback_jobs = [
            (
                "llm/q1/q1.py:queue_health",
                "큐가 비었을 때 next_action과 bottleneck이 latest_summary와 같은 구체 명령을 보여주는지 점검한다.",
            ),
            (
                "llm/q1/q1.py:print_status",
                "status 출력이 queue_health와 같은 다음 액션과 실패 큐 상태를 보여주는지 점검한다.",
            ),
            (
                "llm/q1/q1.py:dry_run_auto",
                "todo가 비었거나 실행 가능 job이 없을 때 dry-run이 구체적인 다음 명령을 출력하는지 점검한다.",
            ),
            (
                "llm/q1/q1.py:autopilot",
                "autopilot이 더 진행할 job이 없을 때 구체적인 다음 명령과 종료 사유를 일관되게 출력하는지 점검한다.",
            ),
            (
                "llm/q1/q1.py:run_validation_suite",
                "반복 개선 검증이 현재 실패 신호와 로그 절약 정책을 유지하는지 점검한다.",
            ),
            (
                "llm/q1/q1.py:server_health",
                "서버 상태 확인이 빠르게 실패 원인과 지연 정보를 보여주고 반복 루프 진입 전에 충분한 신호를 주는지 점검한다.",
            ),
            (
                "llm/q1/q1.py:compact_logs",
                "로그 압축이 최신 summary와 최근 tail을 보존하면서 반복 실행 비용을 낮추는지 점검한다.",
            ),
            (
                "llm/q1/q1.py:archive_resolved_failures",
                "해결됐거나 더 이상 유효하지 않은 failed job만 정리하고 실제 미해결 실패는 남기는지 점검한다.",
            ),
            (
                "llm/q1/q1.py:generate_response_server",
                "서버 생성 요청 실패와 지연이 RuntimeError로 보고되어 비정상 종료 대신 실패 보고서로 남는지 점검한다.",
            ),
            (
                "llm/q1/q1.py:write_latest_summary",
                "큐가 비었을 때 다음 명시 작업 후보를 구체적으로 추천하도록 개선한다.",
            ),
        ]
        for target, goal in fallback_jobs:
            if (target, goal) in no_change_goal_pairs or self.has_no_change_report(target, goal):
                continue
            return f'--new-function-job {target} --goal "{goal}"'
        corpus_path = os.path.join(PROJECT_ROOT, "llm", "q1", "doc", "EVALUATION_CORPUS.md")
        if not os.path.exists(corpus_path):
            return "--build-corpus"
        return "--status"

    def has_no_change_report(self, target, goal):
        report_dir = os.path.join(PROJECT_ROOT, "llm", "q1", "job", "report")
        if not os.path.exists(report_dir):
            return False
        expected_job = f"function:{target}"
        for name in self.list_md_files(report_dir, exclude={"latest_summary.md"}):
            path = os.path.join(report_dir, name)
            try:
                with open(path, "r", encoding="utf-8", errors="replace") as f:
                    text = f.read()
            except OSError:
                continue
            if (
                self.extract_report_field(text, "job") == expected_job
                and self.extract_report_field(text, "goal") == goal
                and self.extract_report_field(text, "verdict") == "NO_CHANGE"
            ):
                return True
        return False

    def recent_target_verdicts(self):
        return {
            target: report.get("verdict")
            for target, report in self.latest_report_by_target().items()
        }

    def queue_health(self):
        job_root = os.path.join(PROJECT_ROOT, "llm", "q1", "job")
        todo_dir = os.path.join(job_root, "todo")
        done_dir = os.path.join(job_root, "done")
        failed_dir = os.path.join(job_root, "failed")
        report_dir = os.path.join(job_root, "report")
        log_dir = os.path.join(PROJECT_ROOT, "llm", "log")
        todo = self.list_md_files(todo_dir)
        done = self.list_md_files(done_dir)
        failed = self.list_md_files(failed_dir)
        reports = self.list_md_files(report_dir, exclude={"latest_summary.md"})
        runnable = 0
        skipped = 0
        for name in todo:
            with open(os.path.join(todo_dir, name), "r", encoding="utf-8") as f:
                if self.extract_target_function(f.read()):
                    runnable += 1
                else:
                    skipped += 1
        response_log = os.path.join(log_dir, "RESPONSE_HISTORY.md")
        compact_summary = os.path.join(log_dir, "COMPACT_SUMMARY.md")
        response_bytes = os.path.getsize(response_log) if os.path.exists(response_log) else 0
        compact_present = os.path.exists(compact_summary)
        discovered = self.discover_function_targets()
        discovered_total = len(discovered)
        seed_summary = self.seed_candidate_summary(discovered=discovered, todo_files=todo)
        seed_candidates = seed_summary["candidates"]
        next_action = "--cycle-once --limit 1 --max-no-change 1" if runnable else "--seed-default-jobs"
        if not runnable and not seed_candidates:
            next_action = self.next_manual_job_command()

        console.print("[bold]q1 queue health[/bold]")
        console.print(f"todo_count: {len(todo)}")
        console.print(f"runnable_todo_count: {runnable}")
        console.print(f"skippable_todo_count: {skipped}")
        console.print(f"done_count: {len(done)}")
        console.print(f"failed_count: {len(failed)}")
        console.print(f"report_count: {len(reports)}")
        console.print(f"response_history_bytes: {response_bytes}")
        console.print(f"compact_summary: {'present' if compact_present else 'missing'}")
        console.print(f"discovered_target_count: {discovered_total}")
        console.print(f"seed_candidate_count: {len(seed_candidates)}")
        console.print(f"seed_rejected_existing_todo: {seed_summary['rejected_existing_todo']}")
        console.print(f"seed_rejected_recent_done_or_no_change: {seed_summary['rejected_recent_done_or_no_change']}")
        console.print(f"seed_rejected_failed: {seed_summary['rejected_failed']}")
        if seed_candidates:
            console.print(f"seed_next_candidate: {seed_candidates[0]}")
        console.print(f"next_action: {next_action}")
        console.print(f"queue_status: {'empty' if not runnable else 'has runnable jobs'}")
        exhausted = not runnable and not seed_candidates
        console.print(f"bottleneck: {'seed candidates exhausted' if exhausted else 'response log size' if response_bytes > 1000000 else 'none detected'}")
    def print_latest_failure(self):
        report_dir = os.path.join(PROJECT_ROOT, "llm", "q1", "job", "report")
        if not os.path.exists(report_dir):
            console.print("[bold red]report directory missing[/bold red]")
            raise SystemExit(1)
        report_files = [
            os.path.join(report_dir, name)
            for name in os.listdir(report_dir)
            if name.endswith(".md") and name != "latest_summary.md"
        ]
        report_files.sort(key=os.path.getmtime, reverse=True)
        latest_by_target = self.latest_report_by_target()
        for path in report_files:
            with open(path, "r", encoding="utf-8", errors="replace") as f:
                text = f.read()
            job = self.extract_report_field(text, "job")
            if job.startswith("function:"):
                target = job.replace("function:", "", 1)
                latest = latest_by_target.get(target)
                if (
                    latest
                    and os.path.abspath(latest.get("path", "")) != os.path.abspath(path)
                    and latest.get("verdict") in {"DONE", "NO_CHANGE"}
                ):
                    continue
            verdict = self.extract_report_field(text, "verdict")
            tool_error_count = self.extract_report_field(text, "tool_error_count")
            errors = []
            if tool_error_count not in {"0", "(missing)"}:
                errors = re.findall(r"^<tool_result name='([^']+)' status='error'>(.*?)</tool_result>", text, re.MULTILINE | re.DOTALL)
            validation_failed = re.search(r"^## FAIL$", text, re.MULTILINE) is not None
            has_failure = (
                bool(errors)
                or validation_failed
                or verdict in {"CONTINUE", "FAIL"}
            )
            if not has_failure:
                continue
            console.print("[bold]q1 latest failure[/bold]")
            console.print(f"file: {os.path.relpath(path, PROJECT_ROOT)}")
            console.print(f"job: {job}")
            console.print(f"timestamp: {self.extract_report_field(text, 'timestamp')}")
            console.print(f"verdict: {verdict}")
            for name, error in errors[:3]:
                console.print(f"tool_error: {name}: {error.strip()}")
            if validation_failed:
                console.print("validation_failure: present")
            return
        console.print("[bold]q1 latest failure[/bold]")
        console.print("none")
    def list_md_files(self, directory, exclude=None):
        exclude = exclude or set()
        if not os.path.exists(directory):
            return []
        try:
            return sorted(name for name in os.listdir(directory) if name.endswith(".md") and name not in exclude)
        except (FileNotFoundError, NotADirectoryError):
            return []
    def improve_once(self, job_path):
        job_full_path = self.tool_handler._resolve_path(job_path)
        with open(job_full_path, "r", encoding="utf-8") as f:
            job = f.read()

        prompt = f"""
    [자가개선 1회 모드]
    아래 job을 수행하라. 목표는 정팀장(q1)이 자기 자신을 한 단계 안정화하는 것이다.

    절대 규칙:
    - 이번 실행에서 수정은 최대 1개 논리 변경만 허용한다.
    - 쓰기 허용 파일: {", ".join(DEFAULT_IMPROVE_ALLOWED_PATHS)}
    - 수정이 필요하면 replace 또는 replace_function만 사용하라.
    - 도구 호출은 반드시 `<tool_call name="replace_function" path="..." func_name="...">...</tool_call>` 또는 `<tool_call name="replace" path="..." old_string="..." new_string="..." />` XML 형식으로 출력하라.
    - 수정 후 execute_bash로 검증하지 마라. 오감독 런타임이 별도 검증한다.
    - 안전한 변경이 없으면 `DONE_NO_CHANGE`만 출력하고 도구 호출을 멈춰라.

    [JOB]
    {job}
        """
        before = self.snapshot_allowed_files()
        before_contents = self.snapshot_file_contents(DEFAULT_IMPROVE_ALLOWED_PATHS)
        try:
            self.chat(
                prompt,
                system_prompt=IMPROVE_ONCE_SYSTEM_PROMPT,
                allowed_tools={"replace", "replace_function"},
                require_done_or_tool=True,
            )
            after = self.snapshot_allowed_files()
            changed_files = [path for path in DEFAULT_IMPROVE_ALLOWED_PATHS if before.get(path) != after.get(path)]
            report = self.run_validation_suite(job_path, changed_files)
            report["model_elapsed_ms"] = self.last_generation_elapsed_ms
        except RuntimeError as exc:
            after = self.snapshot_allowed_files()
            changed_files = [path for path in DEFAULT_IMPROVE_ALLOWED_PATHS if before.get(path) != after.get(path)]
            report = self.runtime_failure_report(job_path, changed_files, exc)
            report["model_elapsed_ms"] = self.last_generation_elapsed_ms
        self.apply_post_validation_actions(report, before_contents)
        if not report["ok"] and report.get("changed_files"):
            report["rolled_back_files"] = self.restore_file_contents(before_contents, report["changed_files"])
        self.write_improve_report(job_path, report)
        if not report["ok"]:
            raise SystemExit(1)

    def function_source_info(self, target):
        path, func_name = parse_function_target(target)
        with open(self.tool_handler._resolve_path(path), "r", encoding="utf-8") as f:
            source = f.read()
        func_source = get_function_source(source, func_name)
        if func_source is None:
            raise RuntimeError(f"Function not found: {func_name} in file: {path}")
        return {
            "path": path,
            "func_name": func_name,
            "source": func_source,
            "hash": hashlib.sha256(func_source.encode("utf-8")).hexdigest(),
        }

    def improve_function(self, target, goal=None, source_info=None):
        path, func_name = parse_function_target(target)
        goal_text = goal or "함수의 안정성, 가독성, 반복 실행 비용을 개선한다."
        target_source_hash = source_info.get("hash") if source_info else None
        before = self.snapshot_paths([path])
        before_contents = self.snapshot_file_contents([path])
        try:
            source_info = source_info or self.function_source_info(target)
            func_source = source_info["source"]
            target_source_hash = source_info["hash"]
        except (OSError, RuntimeError, ValueError) as exc:
            after = self.snapshot_paths([path])
            changed_files = [path] if before.get(path) != after.get(path) else []
            report = self.runtime_failure_report(f"function:{target}", changed_files, exc)
            report["goal"] = goal_text
            report["target_source_hash"] = target_source_hash
            self.write_improve_report(f"function_{path.replace('/', '_')}__{func_name}", report)
            if not getattr(self, "suppress_improve_exit", False):
                raise SystemExit(1)
            return report
        prompt = f"""
    [함수 단위 자가개선 모드]
    대상: `{path}:{func_name}`
    목표: {goal_text}

    이미 q1이 대상 함수를 읽어 아래에 붙였다. 읽기 도구는 금지다.

    [현재 함수 본문]
```python
{func_source}
```

    절대 규칙:
    - `read_function`, `read_file`, `list_functions`를 호출하지 마라. 호출하면 실패다.
    - 이미 제공된 함수 본문만 근거로 판단한다.
    - 수정이 필요 없으면 `DONE_NO_CHANGE`만 출력하고 도구 호출을 멈춰라.
    - 응답은 `DONE_NO_CHANGE` 또는 `replace_function` 도구 호출 하나만 허용된다.
    - 도구 호출은 반드시 `<tool_call name="replace_function" path="{path}" func_name="{func_name}">...</tool_call>` XML 형식으로 출력하라.
    - 도구 호출에는 리터럴 {{path}}, {{func_name}}, target.split 같은 계산식을 쓰지 말고 실제 대상값만 쓴다.
    - 수정이 필요하면 `replace_function`으로 `{func_name}` 함수 하나만 교체하고, 긴 코드는 반드시 block content 포맷을 사용한다.
    - block content 내부에는 CDATA 종료 토큰을 설명이나 문자열로 포함하지 마라.
    - 목표와 직접 연결된 결함, 오류 보고 개선, 반복 실행 비용 감소가 명확하지 않으면 `DONE_NO_CHANGE`만 출력하고 멈춘다.
    - execute_bash, 새 import, 새 외부 모듈, 새 파일 포맷, 미확인 린터/테스트 도구를 추가하지 마라.
    - 기존 CLI 출력 형식, 보고서 필드, Markdown `- field: value` 형식, 정렬 기준, 종료 코드 의미는 목표가 명시하지 않으면 유지한다.
    - CLI 출력은 `console.print`만 쓰고, 새 메시지는 기존 한국어 스타일을 유지한다.
    - 오류를 성공 문자열로 숨기지 말고 기존 실패 흐름을 유지한다.
    - `except Exception`을 새로 추가하지 말고 필요한 경우 실제 원인에 맞는 구체 예외만 잡아라.
    - f-string dict 접근은 바깥 quote와 다른 quote를 쓰고, expression 안에 backslash escape가 필요한 중첩 f-string을 만들지 마라.
    - 중복 조건 단순화, 변수명 변경, 설명 주석 추가처럼 운영 동작이나 실패 진단이 거의 좋아지지 않는 미세 변경은 하지 마라.
    """
        try:
            self.chat(
                prompt,
                max_turns=1,
                max_tokens=self.improve_max_tokens,
                allowed_tools={"replace_function"},
                system_prompt=IMPROVE_SYSTEM_PROMPT,
                require_done_or_tool=True,
            )
            after = self.snapshot_paths([path])
            changed_files = [path] if before.get(path) != after.get(path) else []
            boundary_result = None
            if changed_files:
                boundary_result = self.verify_target_boundary_after_write(target, func_source)
                if not boundary_result["ok"]:
                    raise RuntimeError(boundary_result["result"])
            report = self.run_validation_suite(f"function:{target}", changed_files)
            if boundary_result:
                report["results"].append(boundary_result)
            report["goal"] = goal_text
            report["target_source_hash"] = target_source_hash
            report["model_elapsed_ms"] = self.last_generation_elapsed_ms
        except RuntimeError as exc:
            after = self.snapshot_paths([path])
            changed_files = [path] if before.get(path) != after.get(path) else []
            report = self.runtime_failure_report(f"function:{target}", changed_files, exc)
            report["goal"] = goal_text
            report["target_source_hash"] = target_source_hash
            report["model_elapsed_ms"] = self.last_generation_elapsed_ms
        self.apply_post_validation_actions(report, before_contents)
        if not report["ok"] and report.get("changed_files"):
            report["rolled_back_files"] = self.restore_file_contents(before_contents, report["changed_files"])
        self.write_improve_report(f"function_{path.replace('/', '_')}__{func_name}", report)
        if not report["ok"] and not getattr(self, "suppress_improve_exit", False):
            raise SystemExit(1)
        return report
    def snapshot_allowed_files(self):
        return self.snapshot_paths(DEFAULT_IMPROVE_ALLOWED_PATHS)

    def apply_post_validation_actions(self, report, before_contents):
        changed_files = report.get("changed_files", [])
        if not report.get("ok") or "llm/q1/q1_server.py" not in changed_files:
            return
        try:
            self.restart_server()
            report["server_restarted"] = True
        except RuntimeError as exc:
            failure = self.runtime_failure_report(report.get("job", "post_validation"), changed_files, exc)
            for key, value in failure.items():
                report[key] = value
            report["server_restarted"] = False

    def snapshot_paths(self, paths):
        snapshot = {}
        for path in paths:
            try:
                full_path = self.tool_handler._resolve_path(path)
                if not os.path.exists(full_path):
                    snapshot[path] = None
                    continue
                with open(full_path, "rb") as f:
                    snapshot[path] = hashlib.sha256(f.read()).hexdigest()
            except (OSError, ValueError) as e:
                snapshot[path] = f"ERROR: {e}"
        return snapshot
    def snapshot_file_contents(self, paths):
        snapshot = {}
        for path in paths:
            try:
                full_path = self.tool_handler._resolve_path(path)
                if not os.path.exists(full_path):
                    snapshot[path] = None
                    continue
                with open(full_path, "rb") as f:
                    snapshot[path] = f.read()
            except (OSError, ValueError) as e:
                snapshot[path] = f"ERROR: {e}"
        return snapshot
    def restore_file_contents(self, snapshot, paths):
        restored = []
        for path in paths:
            if path not in snapshot:
                continue
            content = snapshot[path]
            if isinstance(content, str) and content.startswith("ERROR: "):
                continue
            full_path = self.tool_handler._resolve_path(path)
            if content is None:
                if os.path.exists(full_path):
                    os.remove(full_path)
                    restored.append(path)
                continue
            os.makedirs(os.path.dirname(full_path), exist_ok=True)
            with open(full_path, "wb") as f:
                f.write(content)
            restored.append(path)
        return restored
    def module_name_for_q1_path(self, path):
        if not path.endswith(".py"):
            return None
        try:
            full_path = self.tool_handler._resolve_path(path)
            rel_path = os.path.relpath(full_path, Q1_ROOT)
        except (OSError, ValueError):
            return None
        if rel_path.startswith(".."):
            return None
        module_path = rel_path[:-3]
        if os.path.basename(module_path) == "__init__":
            module_path = os.path.dirname(module_path)
        module_name = module_path.replace(os.sep, ".")
        return module_name or None
    def verify_changed_python_imports(self, changed_files):
        results = []
        env = os.environ.copy()
        env["PYTHONPATH"] = Q1_ROOT + os.pathsep + env.get("PYTHONPATH", "")
        code = "import importlib, sys; importlib.import_module(sys.argv[1])"
        for path in changed_files:
            module_name = self.module_name_for_q1_path(path)
            if not module_name:
                continue
            command_label = f"{sys.executable} -c importlib.import_module {module_name}"
            try:
                proc = subprocess.run(
                    [sys.executable, "-c", code, module_name],
                    cwd=PROJECT_ROOT,
                    env=env,
                    capture_output=True,
                    text=True,
                    timeout=10,
                )
                status = "success" if proc.returncode == 0 else "error"
                result = (
                    f"<tool_result name='import_check' status='{status}'>\n"
                    f"MODULE: {module_name}\n"
                    f"RETURN_CODE: {proc.returncode}\n"
                    f"STDOUT: {proc.stdout[:1000]}\n"
                    f"STDERR: {proc.stderr[:1000]}\n"
                    "</tool_result>"
                )
                results.append({"command": command_label, "ok": proc.returncode == 0, "result": result})
            except subprocess.TimeoutExpired as exc:
                result = (
                    "<tool_result name='import_check' status='error'>\n"
                    f"MODULE: {module_name}\n"
                    "RETURN_CODE: -1\n"
                    f"STDOUT: {(exc.stdout or '')[:1000]}\n"
                    "STDERR: import check timed out\n"
                    "</tool_result>"
                )
                results.append({"command": command_label, "ok": False, "result": result})
        return results
    def verify_target_boundary_after_write(self, target, original_source):
        path, func_name = parse_function_target(target)
        command_label = f"post-write target boundary {target}"
        try:
            current_source = self.function_source_info(target)["source"]
            original_node = self.tool_handler._replacement_function_node(original_source, func_name)
            current_node = self.tool_handler._replacement_function_node(current_source, func_name)
            self.tool_handler._assert_function_boundary_preserved(original_node, current_node)
            return {
                "command": command_label,
                "ok": True,
                "result": (
                    "<tool_result name='post_write_boundary' status='success'>\n"
                    f"TARGET: {target}\n"
                    "</tool_result>"
                ),
            }
        except (OSError, RuntimeError, ValueError, SyntaxError) as exc:
            return {
                "command": command_label,
                "ok": False,
                "result": (
                    "<tool_result name='post_write_boundary' status='error'>\n"
                    f"TARGET: {target}\n"
                    f"ERROR: {str(exc)}\n"
                    "</tool_result>"
                ),
            }
    def run_validation_suite(self, job_path, changed_files):
        commands = [
            "./venv/bin/python -m py_compile llm/q1/q1.py llm/q1/core/handlers.py llm/q1/core/analyzer.py llm/q1/context/project_context.py llm/q1/utils/ast_utils.py llm/q1/utils/logger.py llm/q1/q1_server.py",
            "cd llm/q1 && ../../venv/bin/python -m unittest discover -s tests",
            "./venv/bin/python llm/q1/q1.py --self-check",
            "./venv/bin/python llm/q1/q1.py --tool-self-test",
            "Q1_SKIP_RESPONSE_LOG=1 ./venv/bin/python llm/q1/q1.py --task \"llm/q1/core 목록 확인해라\"",
        ]
        results = []
        for command in commands:
            result = self.tool_handler.execute_bash({"command": command})
            ok = "status='success'" in result
            results.append({"command": command, "ok": ok, "result": result})
        results.extend(self.verify_changed_python_imports(changed_files))
        tool_errors = [
            result for result in self.last_tool_results[-3:]
            if re.search(r"^<tool_result\s+name='[^']+'\s+status='error'>", result, re.MULTILINE)
        ]
        return {
            "job": job_path,
            "timestamp": datetime.now().strftime("%Y-%m-%d %H:%M:%S"),
            "ok": all(item["ok"] for item in results) and not tool_errors,
            "changed_files": changed_files,
            "tool_results": list(self.last_tool_results[-3:]),
            "tool_errors": tool_errors,
            "results": results,
        }

    def runtime_failure_report(self, job_path, changed_files, exc):
        error_text = str(exc)
        tool_error = f"<tool_result name='runtime' status='error'>{error_text}</tool_result>"
        return {
            "job": job_path,
            "timestamp": datetime.now().strftime("%Y-%m-%d %H:%M:%S"),
            "ok": False,
            "changed_files": changed_files,
            "tool_results": [tool_error],
            "tool_errors": [tool_error],
            "results": [
                {
                    "command": "runtime",
                    "ok": False,
                    "result": tool_error,
                }
            ],
        }

    def fast_no_change_report(self, job_path, reason, goal=None, target_source_hash=None):
        return {
            "job": job_path,
            "timestamp": datetime.now().strftime("%Y-%m-%d %H:%M:%S"),
            "ok": True,
            "changed_files": [],
            "tool_results": [
                f"<tool_result name='fast_no_change' status='success'>{reason}</tool_result>"
            ],
            "tool_errors": [],
            "results": [],
            "goal": goal,
            "target_source_hash": target_source_hash,
        }

    def validate(self):
        previous_tool_results = self.last_tool_results
        previous_pycache_prefix = os.environ.get("PYTHONPYCACHEPREFIX")
        try:
            self.last_tool_results = []
            os.environ.setdefault("PYTHONPYCACHEPREFIX", "/private/tmp/q1_pycache")
            report = self.run_validation_suite("manual:validate", [])
        finally:
            self.last_tool_results = previous_tool_results
            if previous_pycache_prefix is None:
                os.environ.pop("PYTHONPYCACHEPREFIX", None)
            else:
                os.environ["PYTHONPYCACHEPREFIX"] = previous_pycache_prefix
        console.print("[bold]q1 validation[/bold]")
        for item in report["results"]:
            status = "PASS" if item["ok"] else "FAIL"
            color = "green" if item["ok"] else "red"
            console.print(f"[{color}]{status}[/] {item['command']}")
        if not report["ok"]:
            raise SystemExit(1)

    def write_improve_report(self, job_path, report):
        report_dir = os.path.join(PROJECT_ROOT, "llm", "q1", "job", "report")
        os.makedirs(report_dir, exist_ok=True)
        safe_name = os.path.basename(job_path).replace(".md", "")
        report_path = os.path.join(report_dir, f"{datetime.now().strftime('%Y%m%d_%H%M%S')}_{safe_name}.md")
        verdict = "DONE" if report["ok"] and report["changed_files"] else "NO_CHANGE" if report["ok"] else "CONTINUE"
        lines = [
            "# q1 Improve Once Report",
            "",
            f"- job: {report['job']}",
            f"- timestamp: {report['timestamp']}",
            f"- verdict: {verdict}",
            f"- changed_files: {', '.join(report['changed_files']) if report['changed_files'] else '(none)'}",
            f"- tool_error_count: {len(report.get('tool_errors', []))}",
        ]
        if report.get("goal"):
            lines.append(f"- goal: {report['goal']}")
        if report.get("target_source_hash"):
            lines.append(f"- target_source_hash: {report['target_source_hash']}")
        if report.get("model_elapsed_ms") is not None:
            lines.append(f"- model_elapsed_ms: {report['model_elapsed_ms']}")
        if report.get("rolled_back_files"):
            lines.append(f"- rolled_back_files: {', '.join(report['rolled_back_files'])}")
        if report.get("server_restarted") is not None:
            lines.append(f"- server_restarted: {str(report['server_restarted']).lower()}")
        if any("fast_no_change" in result for result in report.get("tool_results", [])):
            lines.append("- model_call_skipped: true")
        lines.append("")
        for item in report["results"]:
            lines.extend([
                f"## {'PASS' if item['ok'] else 'FAIL'}",
                f"`{item['command']}`",
                "",
                "```xml",
                item["result"],
                "```",
                "",
            ])
        if report.get("tool_results"):
            lines.extend(["## Tool Results", ""])
            for result in report["tool_results"]:
                lines.extend(["```xml", result, "```", ""])
        with open(report_path, "w", encoding="utf-8") as f:
            f.write("\n".join(lines))
        console.print(f"[bold green]자가개선 보고서:[/bold green] {report_path}")
        self.write_latest_summary()

    def write_latest_summary(self, limit=5):
        job_root = os.path.join(PROJECT_ROOT, "llm", "q1", "job")
        todo_dir = os.path.join(job_root, "todo")
        done_dir = os.path.join(job_root, "done")
        failed_dir = os.path.join(job_root, "failed")
        report_dir = os.path.join(PROJECT_ROOT, "llm", "q1", "job", "report")
        os.makedirs(report_dir, exist_ok=True)
        todo = self.list_md_files(todo_dir)
        done = self.list_md_files(done_dir)
        failed = self.list_md_files(failed_dir)
        report_files = [
            os.path.join(report_dir, name)
            for name in self.list_md_files(report_dir, exclude={"latest_summary.md"})
        ]
        report_files.sort(key=os.path.getmtime, reverse=True)

        entries = []
        for path in report_files[:max(limit * 3, 15)]:
            try:
                with open(path, "r", encoding="utf-8", errors="replace") as f:
                    text = f.read()
            except OSError as exc:
                console.print(f"[yellow]summary report skipped:[/yellow] {os.path.basename(path)}: {exc}")
                continue
            verdict = self.extract_report_field(text, "verdict")
            changed_files = self.extract_report_field(text, "changed_files")
            is_failure_test = (
                verdict == "CONTINUE"
                and changed_files == "(none)"
                and "q1 server unavailable" in text
                and "127.0.0.1:65534" in text
            )
            entries.append({
                "file": os.path.basename(path),
                "job": self.extract_report_field(text, "job"),
                "timestamp": self.extract_report_field(text, "timestamp"),
                "verdict": verdict,
                "changed_files": changed_files,
                "category": "failure_test" if is_failure_test else "development",
                "model_call_skipped": self.extract_report_field(text, "model_call_skipped"),
                "model_elapsed_ms": self.extract_report_field(text, "model_elapsed_ms"),
            })

        operational_entries = [item for item in entries if item["category"] != "failure_test"]
        recent_reports = operational_entries[:limit]
        ignored_failure_tests = len(entries) - len(operational_entries)
        latest_verdict = operational_entries[0]["verdict"] if operational_entries else "(none)"
        latest_category = operational_entries[0]["category"] if operational_entries else "(none)"
        repeated_no_change = sum(1 for item in operational_entries[:2] if item["verdict"] == "NO_CHANGE")
        seed_summary = self.seed_candidate_summary(todo_files=todo)
        seed_candidates = seed_summary["candidates"]
        next_job = "--improve-function llm/q1/q1.py:write_latest_summary"
        if todo:
            next_job = "--auto-improve --limit 1"
        elif latest_verdict == "CONTINUE":
            next_job = "--failure-latest"
        elif not seed_candidates:
            next_job = self.next_manual_job_command()
        elif repeated_no_change >= 2:
            next_job = "--seed-discovered-jobs --seed-count 3"
        elif latest_verdict == "NO_CHANGE":
            next_job = "--seed-default-jobs --seed-count 3"

        summary = {
            "generated_at": datetime.now().strftime("%Y-%m-%d %H:%M:%S"),
            "todo_count": len(todo),
            "done_count": len(done),
            "failed_count": len(failed),
            "report_count": len(report_files),
            "next_todo": todo[0] if todo else "(none)",
            "latest_verdict": latest_verdict,
            "latest_report_category": latest_category,
            "ignored_failure_test_reports": ignored_failure_tests,
            "seed_candidate_count": len(seed_candidates),
            "seed_rejected_existing_todo": seed_summary["rejected_existing_todo"],
            "seed_rejected_recent_done_or_no_change": seed_summary["rejected_recent_done_or_no_change"],
            "seed_rejected_failed": seed_summary["rejected_failed"],
            "seed_sample_candidates": seed_summary["sample_candidates"],
            "next_recommended_job": next_job,
            "recent_reports": [
                {
                    "file": item["file"],
                    "job": item["job"],
                    "timestamp": item["timestamp"],
                    "verdict": item["verdict"],
                    "changed_files": item["changed_files"],
                    "category": item["category"],
                    "model_call_skipped": item["model_call_skipped"],
                    "model_elapsed_ms": item["model_elapsed_ms"],
                }
                for item in recent_reports
            ]
        }

        lines = [
            "# q1 Latest Auto-Improve Summary",
            "",
            f"- generated_at: {summary['generated_at']}",
            f"- todo_count: {summary['todo_count']}",
            f"- done_count: {summary['done_count']}",
            f"- failed_count: {summary['failed_count']}",
            f"- report_count: {summary['report_count']}",
            f"- next_todo: {summary['next_todo']}",
            f"- latest_verdict: {summary['latest_verdict']}",
            f"- latest_report_category: {summary['latest_report_category']}",
            f"- ignored_failure_test_reports: {summary['ignored_failure_test_reports']}",
            f"- seed_candidate_count: {summary['seed_candidate_count']}",
            f"- seed_rejected_existing_todo: {summary['seed_rejected_existing_todo']}",
            f"- seed_rejected_recent_done_or_no_change: {summary['seed_rejected_recent_done_or_no_change']}",
            f"- seed_rejected_failed: {summary['seed_rejected_failed']}",
            f"- seed_sample_candidates: {', '.join(summary['seed_sample_candidates']) if summary['seed_sample_candidates'] else '(none)'}",
            f"- next_recommended_job: {summary['next_recommended_job']}",
            "",
            "## Recent Reports",
        ]
        for report in summary["recent_reports"]:
            lines.extend([
                f"- file: {report['file']}",
                f"  job: {report['job']}",
                f"  timestamp: {report['timestamp']}",
                f"  verdict: {report['verdict']}",
                f"  changed_files: {report['changed_files']}",
                f"  category: {report['category']}",
                f"  model_call_skipped: {report['model_call_skipped']}",
                f"  model_elapsed_ms: {report['model_elapsed_ms']}",
            ])
        with open(os.path.join(report_dir, "latest_summary.md"), "w", encoding="utf-8") as f:
            f.write("\n".join(lines) + "\n")

    def extract_report_field(self, text, field):
        match = re.search(rf"^- {re.escape(field)}: (.*)$", text, re.MULTILINE)
        if match:
            value = match.group(1).strip()
            return value if value else "(missing)"
        return "(missing)"
    def auto_improve(self, limit, max_no_change=0):
        todo_dir = os.path.join(PROJECT_ROOT, "llm", "q1", "job", "todo")
        os.makedirs(todo_dir, exist_ok=True)
        jobs = [
            os.path.join(todo_dir, name)
            for name in sorted(os.listdir(todo_dir))
            if name.endswith(".md")
        ]
        processed = 0
        consecutive_no_change = 0
        skipped_model_calls = 0
        stopped_reason = "limit reached or no jobs"
        latest_reports = self.latest_report_by_target()
        for job_path in jobs:
            if processed >= limit:
                break
            processed += 1
            with open(job_path, "r", encoding="utf-8") as f:
                job = f.read()
            target = self.extract_target_function(job)
            if not target:
                self.write_skip_report(os.path.relpath(job_path, PROJECT_ROOT), "missing Target Function")
                self.archive_job(job_path)
                continue
            job_goal = self.extract_job_goal(job) or ""
            latest = latest_reports.get(target)
            fast_skip_allowed = "자동 발견된 함수 후보" in job_goal
            if fast_skip_allowed and latest and latest.get("verdict") == "NO_CHANGE" and latest.get("changed_files") == "(none)":
                reason = f"recent NO_CHANGE for {target}; skipped model call"
                report = self.fast_no_change_report(f"function:{target}", reason, goal=job_goal)
                report["model_elapsed_ms"] = None
                self.write_improve_report(f"function_{target.replace('/', '_').replace(':', '__')}", report)
                self.archive_job(job_path)
                consecutive_no_change += 1
                skipped_model_calls += 1
                if max_no_change > 0 and consecutive_no_change >= max_no_change:
                    stopped_reason = f"max_no_change reached ({max_no_change})"
                    break
                continue
            current_source_info = None
            current_source_hash = None
            try:
                current_source_info = self.function_source_info(target)
                current_source_hash = current_source_info["hash"]
            except (OSError, RuntimeError, ValueError):
                current_source_hash = None
            exact_no_change = (
                latest
                and latest.get("verdict") == "NO_CHANGE"
                and latest.get("changed_files") == "(none)"
                and latest.get("goal") == job_goal
                and latest.get("target_source_hash") == current_source_hash
                and current_source_hash is not None
            )
            if exact_no_change:
                reason = f"same goal and unchanged source for {target}; skipped model call"
                report = self.fast_no_change_report(
                    f"function:{target}",
                    reason,
                    goal=job_goal,
                    target_source_hash=current_source_hash,
                )
                report["model_elapsed_ms"] = None
                self.write_improve_report(f"function_{target.replace('/', '_').replace(':', '__')}", report)
                self.archive_job(job_path)
                consecutive_no_change += 1
                skipped_model_calls += 1
                if max_no_change > 0 and consecutive_no_change >= max_no_change:
                    stopped_reason = f"max_no_change reached ({max_no_change})"
                    break
                continue
            self.last_tool_results = []
            self.suppress_improve_exit = True
            try:
                report = self.improve_function(target, goal=job_goal, source_info=current_source_info)
            finally:
                self.suppress_improve_exit = False
            if report["ok"]:
                self.archive_job(job_path)
            else:
                self.archive_job(job_path, status="failed")
                stopped_reason = "job failed and archived"
                continue
            if report["ok"] and not report["changed_files"]:
                consecutive_no_change += 1
            else:
                consecutive_no_change = 0
            if max_no_change > 0 and consecutive_no_change >= max_no_change:
                stopped_reason = f"max_no_change reached ({max_no_change})"
                break
        self.write_latest_summary()
        console.print(f"[bold green]auto-improve inspected:[/bold green] {processed}")
        console.print(f"consecutive_no_change: {consecutive_no_change}")
        console.print(f"skipped_model_calls: {skipped_model_calls}")
        console.print(f"stopped_reason: {stopped_reason}")
    def archive_skipped(self):
        todo_dir = os.path.join(PROJECT_ROOT, "llm", "q1", "job", "todo")
        os.makedirs(todo_dir, exist_ok=True)
        jobs = [
            os.path.join(todo_dir, name)
            for name in sorted(os.listdir(todo_dir))
            if name.endswith(".md")
        ]
        archived = 0
        kept = 0
        for job_path in jobs:
            with open(job_path, "r", encoding="utf-8") as f:
                job = f.read()
            if self.extract_target_function(job):
                kept += 1
                continue
            self.write_skip_report(os.path.relpath(job_path, PROJECT_ROOT), "missing Target Function")
            self.archive_job(job_path)
            archived += 1
        self.write_latest_summary()
        console.print("[bold]q1 archive skipped[/bold]")
        console.print(f"archived: {archived}")
        console.print(f"kept: {kept}")
        console.print(f"summary: archived_missing_target={archived}, kept_runnable={kept}")
    def dry_run_auto(self, limit):
        todo_dir = os.path.join(PROJECT_ROOT, "llm", "q1", "job", "todo")
        os.makedirs(todo_dir, exist_ok=True)
        jobs = [
            os.path.join(todo_dir, name)
            for name in sorted(os.listdir(todo_dir))
            if name.endswith(".md")
        ][:limit]
        run_count = 0
        skip_count = 0
        console.print("[bold]q1 dry-run auto[/bold]")
        console.print(f"limit: {limit}")
        console.print(f"planned: {len(jobs)}")
        if not jobs:
            console.print(f"next_action: {self.next_manual_job_command()}")
            console.print("reason: todo directory is empty")
            return
        for index, job_path in enumerate(jobs, start=1):
            with open(job_path, "r", encoding="utf-8") as f:
                job = f.read()
            rel_path = os.path.relpath(job_path, PROJECT_ROOT)
            target = self.extract_target_function(job)
            if target:
                run_count += 1
                console.print(f"{index}. RUN {rel_path} -> {target}")
            else:
                skip_count += 1
                console.print(f"{index}. SKIP {rel_path} -> missing Target Function")
        console.print(f"run_count: {run_count}")
        console.print(f"skip_count: {skip_count}")
        if run_count == 0:
            console.print(f"next_action: {self.next_manual_job_command()}")
            console.print("reason: no runnable jobs found")

    def cycle_once(self, limit, max_no_change=0, compact_logs=False):
        console.print('[bold]q1 cycle once[/bold]')
        self.server_health()
        self.archive_skipped()
        self.dry_run_auto(limit)
        self.auto_improve(limit, max_no_change=max_no_change)
        self.validate()
        if compact_logs:
            self.compact_logs()
        self.print_status()

    def runnable_todo_count(self):
        todo_dir = os.path.join(PROJECT_ROOT, "llm", "q1", "job", "todo")
        runnable = 0
        for name in self.list_md_files(todo_dir):
            file_path = os.path.join(todo_dir, name)
            try:
                with open(file_path, "r", encoding="utf-8", errors="replace") as f:
                    content = f.read()
            except OSError:
                continue
            if self.extract_target_function(content):
                runnable += 1
        return runnable

    def autopilot(self, cycles, interval, limit, seed_count, max_no_change=0, validate_every=5, compact_every=5):
        if cycles < 1:
            console.print("[bold red]--cycles must be >= 1[/bold red]")
            raise SystemExit(1)
        if interval < 0:
            console.print("[bold red]--interval must be >= 0[/bold red]")
            raise SystemExit(1)
        if seed_count < 0:
            console.print("[bold red]--seed-count must be >= 0[/bold red]")
            raise SystemExit(1)

        def seed_candidate_count():
            recent_verdicts = self.recent_target_verdicts()
            failed_targets = self.failed_job_targets()
            return sum(
                1 for target in self.discover_function_targets()
                if target not in failed_targets and recent_verdicts.get(target) not in {"DONE", "NO_CHANGE"}
            )

        console.print("[bold]q1 autopilot[/bold]")
        console.print(f"cycles: {cycles}")
        console.print(f"limit: {limit}")
        console.print(f"seed_count: {seed_count}")
        console.print(f"validate_every: {validate_every}")
        console.print(f"compact_every: {compact_every}")
        self.server_health()
        for cycle in range(1, cycles + 1):
            console.print(f"[bold]autopilot cycle {cycle}/{cycles}[/bold]")
            self.archive_skipped()
            if seed_count > 0 and self.runnable_todo_count() == 0:
                self.seed_retryable_failures(seed_count)
            if seed_count > 0 and self.runnable_todo_count() == 0:
                self.seed_from_failures(seed_count)
            if seed_count > 0 and self.runnable_todo_count() == 0:
                self.seed_discovered_jobs(seed_count)
            if seed_count > 0 and self.runnable_todo_count() == 0:
                self.seed_default_jobs(seed_count)
            if self.runnable_todo_count() == 0:
                console.print("[yellow]autopilot stopped: no runnable jobs[/yellow]")
                console.print(f"seed_candidate_count: {seed_candidate_count()}")
                console.print(f"next_action: {self.next_manual_job_command()}")
                break
            self.dry_run_auto(limit)
            self.auto_improve(limit, max_no_change=max_no_change)
            if validate_every > 0 and cycle % validate_every == 0:
                self.validate()
            if compact_every > 0 and cycle % compact_every == 0:
                self.compact_logs()
            self.write_latest_summary()
            console.print(f"autopilot_cycle_done: {cycle}/{cycles}")
            if cycle < cycles and interval:
                time.sleep(interval)
        self.print_status()
    def loop(self, cycles, interval, limit, max_no_change=0, compact_logs=False):
        if cycles < 1:
            console.print("[bold red]--cycles must be >= 1[/bold red]")
            raise SystemExit(1)
        if interval < 0:
            console.print("[bold red]--interval must be >= 0[/bold red]")
            raise SystemExit(1)
        console.print("[bold]q1 loop[/bold]")
        console.print(f"cycles: {cycles}")
        console.print(f"interval: {interval}")
        for cycle in range(1, cycles + 1):
            console.print(f"[bold]cycle {cycle}/{cycles}[/bold]")
            self.cycle_once(limit, max_no_change=max_no_change, compact_logs=compact_logs)
            console.print(f"cycle_done: {cycle}/{cycles}")
            if cycle < cycles and interval:
                time.sleep(interval)
    def compact_logs(self, keep_chars=12000, min_size=32768):
        log_dir = os.path.join(PROJECT_ROOT, "llm", "log")
        archive_dir = os.path.join(log_dir, "archive")
        os.makedirs(archive_dir, exist_ok=True)
        candidates = []
        response_log = os.path.join(log_dir, "RESPONSE_HISTORY.md")
        if os.path.exists(response_log):
            candidates.append(response_log)
        if os.path.exists(log_dir):
            for name in sorted(os.listdir(log_dir)):
                if name.startswith("BLACKBOX_") and name.endswith(".md"):
                    candidates.append(os.path.join(log_dir, name))

        compacted = []
        skipped = []
        timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
        latest_summary = os.path.join(PROJECT_ROOT, "llm", "q1", "job", "report", "latest_summary.md")
        summary_text = ""
        if os.path.exists(latest_summary):
            with open(latest_summary, "r", encoding="utf-8", errors="replace") as f:
                summary_text = f.read().strip()

        for path in candidates:
            size = os.path.getsize(path)
            name = os.path.basename(path)
            if size < min_size:
                skipped.append((name, size))
                continue
            archive_path = os.path.join(archive_dir, f"{timestamp}_{name}")
            shutil.copy2(path, archive_path)
            with open(path, "r", encoding="utf-8", errors="replace") as f:
                text = f.read()
            tail = text[-keep_chars:]
            compacted_text = "\n".join([
                f"# Compacted {name}",
                "",
                f"- compacted_at: {datetime.now().strftime('%Y-%m-%d %H:%M:%S')}",
                f"- original_bytes: {size}",
                f"- archive: {os.path.relpath(archive_path, PROJECT_ROOT)}",
                f"- kept_tail_chars: {keep_chars}",
                "",
                "## Latest Summary",
                summary_text or "(missing)",
                "",
                "## Recent Tail",
                tail.strip(),
                "",
            ])
            with open(path, "w", encoding="utf-8") as f:
                f.write(compacted_text)
            compacted.append((name, size, os.path.getsize(path)))

        report_path = os.path.join(log_dir, "COMPACT_SUMMARY.md")
        lines = [
            "# q1 Log Compaction Summary",
            "",
            f"- generated_at: {datetime.now().strftime('%Y-%m-%d %H:%M:%S')}",
            f"- compacted_count: {len(compacted)}",
            f"- skipped_count: {len(skipped)}",
            "",
            "## Compacted",
        ]
        for name, before_size, after_size in compacted:
            lines.append(f"- {name}: {before_size} -> {after_size} bytes")
        lines.append("")
        lines.append("## Skipped")
        for name, size in skipped:
            lines.append(f"- {name}: {size} bytes")
        with open(report_path, "w", encoding="utf-8") as f:
            f.write("\n".join(lines) + "\n")

        console.print("[bold]q1 compact logs[/bold]")
        console.print(f"compacted: {len(compacted)}")
        console.print(f"skipped: {len(skipped)}")
        console.print(f"report: {os.path.relpath(report_path, PROJECT_ROOT)}")

    def extract_target_function(self, job_text):
        matches = re.findall(r"^-\s*`([^`]+:[\w_]+)`\s*$", job_text, re.MULTILINE)
        return matches[0] if matches else None
    def extract_job_goal(self, job_text):
        match = re.search(r"^## Goal\s+(.+?)(?:\n## |\Z)", job_text, re.MULTILINE | re.DOTALL)
        if not match:
            return None
        return " ".join(line.strip() for line in match.group(1).splitlines() if line.strip())
    def archive_job(self, job_path, status="done"):
        archive_dir = os.path.join(PROJECT_ROOT, "llm", "q1", "job", status)
        os.makedirs(archive_dir, exist_ok=True)
        name = os.path.basename(job_path)
        target = os.path.join(archive_dir, name)
        if os.path.exists(target):
            base, ext = os.path.splitext(name)
            timestamp = datetime.now().strftime('%Y%m%d_%H%M%S')
            counter = 1
            while os.path.exists(target):
                target = os.path.join(archive_dir, f"{base}_{timestamp}_{counter}{ext}")
                counter += 1
        os.replace(job_path, target)
    def write_skip_report(self, job_path, reason):
        report_dir = os.path.join(PROJECT_ROOT, "llm", "q1", "job", "report")
        os.makedirs(report_dir, exist_ok=True)
        safe_name = os.path.basename(job_path).replace(".md", "")
        report_path = os.path.join(report_dir, f"{datetime.now().strftime('%Y%m%d_%H%M%S')}_{safe_name}_skipped.md")
        lines = [
            "# q1 Improve Skip Report",
            "",
            f"- job: {job_path}",
            f"- timestamp: {datetime.now().strftime('%Y-%m-%d %H:%M:%S')}",
            "- verdict: SKIPPED",
            "- changed_files: (none)",
            f"- failure_reason: {reason}",
            "",
            "- next_recommended_job: --auto-improve --limit 1",
        ]
        with open(report_path, "w", encoding="utf-8") as f:
            f.write("\n".join(lines) + "\n")
        self.write_latest_summary()
    def build_corpus(self):
        doc_dir = os.path.join(PROJECT_ROOT, "llm", "q1", "doc")
        os.makedirs(doc_dir, exist_ok=True)
        path = os.path.join(doc_dir, "EVALUATION_CORPUS.md")
        seed_summary = self.seed_candidate_summary()
        lines = [
            "# q1 Evaluation Corpus",
            "",
            f"- generated_at: {datetime.now().strftime('%Y-%m-%d %H:%M:%S')}",
            f"- discovered_target_count: {seed_summary['discovered_count']}",
            f"- seed_candidate_count: {seed_summary['candidate_count']}",
            f"- seed_rejected_recent_done_or_no_change: {seed_summary['rejected_recent_done_or_no_change']}",
            "",
            "## Guard Cases",
            "",
            "- core_fixture_parse_function_target: target strings are split on the last colon and malformed targets raise `ValueError`.",
            "- core_fixture_extract_job_goal: multiline job goals collapse into a stable single-line goal string.",
            "- core_fixture_module_name_for_q1_path: q1 file paths map to importable module names and non-Python files are ignored.",
            "- core_fixture_looks_like_direct_task: file/listing requests are direct tasks and plain chat is not.",
            "- handler_fixture_strip_code_fence: fenced Python content is unwrapped and unfenced content is preserved.",
            "- handler_fixture_replacement_function_node: replacement snippets must contain exactly one target function.",
            "- handler_fixture_boundary_preserved: body-only changes are accepted and signature changes are rejected.",
            "- handler_fixture_parse_tool_calls_block: block content preserves embedded tool-like strings.",
            "- handler_fixture_parse_tool_calls_cdata: raw CDATA blocks unwrap into Python content.",
            "- handler_fixture_parse_tool_calls_alias: self-closing `function=` aliases resolve to tool names.",
            "- parse_tool_calls_roundtrip: XML attributes, block content, CDATA, and literal closing-tag strings parse correctly.",
            "- replace_function_signature_guard: renamed functions are rejected.",
            "- replace_function_decorator_guard: dropped decorators are rejected.",
            "- replace_function_return_guard: replacements cannot drop existing return flow.",
            "- replace_function_yield_guard: replacements cannot drop existing yield flow.",
            "- replace_function_topology_guard: replacements cannot add or remove functions during splice.",
            "- post_apply_rollback: failed validation can restore pre-change bytes.",
            "- changed_python_import_guard: changed q1 Python modules must import in a subprocess.",
            "- post_write_boundary_guard: target boundaries are checked again after writing.",
            "",
            "## Validation Commands",
            "",
            "- `./venv/bin/python -m py_compile llm/q1/q1.py llm/q1/core/handlers.py llm/q1/core/analyzer.py llm/q1/context/project_context.py llm/q1/utils/ast_utils.py llm/q1/utils/logger.py llm/q1/q1_server.py`",
            "- `cd llm/q1 && ../../venv/bin/python -m unittest discover -s tests`",
            "- `./venv/bin/python llm/q1/q1.py --self-check`",
            "- `./venv/bin/python llm/q1/q1.py --tool-self-test`",
            "- `Q1_SKIP_RESPONSE_LOG=1 ./venv/bin/python llm/q1/q1.py --task \"llm/q1/core 목록 확인해라\"`",
            "",
            "## Next Corpus Gaps",
            "",
            "- Add fixture-level behavior tests for functions that can be exercised without a model call.",
            "- Add an import-time timeout fixture if q1 starts importing side-effect-heavy modules.",
            "- Add a manifest for target-specific verification commands after the corpus is stable.",
        ]
        with open(path, "w", encoding="utf-8") as f:
            f.write("\n".join(lines) + "\n")
        console.print(f"[bold green]evaluation corpus:[/bold green] {path}")
    def print_council_status(self, limit=3):
        path = os.path.join(PROJECT_ROOT, "llm", "q1", "doc", "COUNCIL.md")
        if not os.path.exists(path):
            console.print("[bold red]COUNCIL.md missing[/bold red]")
            raise SystemExit(1)
        with open(path, "r", encoding="utf-8", errors="replace") as f:
            text = f.read()
        rounds = re.split(r"(?m)^## ", text)
        parsed = []
        for chunk in rounds:
            if not chunk.startswith("202"):
                continue
            title, _, body = chunk.partition("\n")
            topic_match = re.search(r"(?ms)^### Topic\s+(.+?)(?=^### |\Z)", body)
            decision_match = re.search(r"(?ms)^### Decision\s+(.+?)(?=^### |\Z)", body)
            claude_state = "present" if "### Claude View" in body else "missing"
            gemini_state = "present" if "### Gemini View" in body and "did not return" not in body else "timeout" if "### Gemini View" in body else "missing"
            parsed.append({
                "title": title.strip(),
                "topic": " ".join((topic_match.group(1).strip() if topic_match else "(missing)").split()),
                "decision": " ".join((decision_match.group(1).strip() if decision_match else "(missing)").split()),
                "claude_state": claude_state,
                "gemini_state": gemini_state,
            })
        console.print("[bold]q1 council status[/bold]")
        console.print("Codex: coordinator + implementer + validator")
        console.print("Claude: strategy/review participant")
        console.print("Gemini: strategy/review participant")
        for item in parsed[-limit:]:
            console.print("")
            console.print(f"round: {item['title']}")
            console.print(f"topic: {item['topic']}")
            console.print(f"Claude: {item['claude_state']}")
            console.print(f"Gemini: {item['gemini_state']}")
            console.print(f"Codex: implemented or queued the decision")
            console.print(f"decision: {item['decision']}")
    def build_scorecard(self):
        report_dir = os.path.join(PROJECT_ROOT, "llm", "q1", "job", "report")
        doc_dir = os.path.join(PROJECT_ROOT, "llm", "q1", "doc")
        os.makedirs(doc_dir, exist_ok=True)
        reports = []
        for name in self.list_md_files(report_dir, exclude={"latest_summary.md"}):
            path = os.path.join(report_dir, name)
            try:
                with open(path, "r", encoding="utf-8", errors="replace") as f:
                    text = f.read()
            except OSError:
                continue
            if "static:untyped_long" not in text:
                continue
            elapsed = self.extract_report_field(text, "model_elapsed_ms")
            try:
                elapsed_ms = int(elapsed)
            except ValueError:
                elapsed_ms = None
            reports.append({
                "file": name,
                "job": self.extract_report_field(text, "job"),
                "verdict": self.extract_report_field(text, "verdict"),
                "changed_files": self.extract_report_field(text, "changed_files"),
                "model_elapsed_ms": elapsed_ms,
            })
        verdict_counts = {}
        elapsed_values = []
        changed_count = 0
        for report in reports:
            verdict_counts[report["verdict"]] = verdict_counts.get(report["verdict"], 0) + 1
            if report["changed_files"] != "(none)":
                changed_count += 1
            if report["model_elapsed_ms"] is not None:
                elapsed_values.append(report["model_elapsed_ms"])
        average_elapsed = int(sum(elapsed_values) / len(elapsed_values)) if elapsed_values else 0
        path = os.path.join(doc_dir, "SCORECARD.md")
        lines = [
            "# q1 Scorecard",
            "",
            f"- generated_at: {datetime.now().strftime('%Y-%m-%d %H:%M:%S')}",
            f"- static_untyped_long_reports: {len(reports)}",
            f"- changed_report_count: {changed_count}",
            f"- no_change_count: {verdict_counts.get('NO_CHANGE', 0)}",
            f"- done_count: {verdict_counts.get('DONE', 0)}",
            f"- continue_count: {verdict_counts.get('CONTINUE', 0)}",
            f"- average_model_elapsed_ms: {average_elapsed}",
            "",
            "## Recent Static Signal Reports",
        ]
        for report in reports[-10:]:
            lines.extend([
                f"- file: {report['file']}",
                f"  job: {report['job']}",
                f"  verdict: {report['verdict']}",
                f"  changed_files: {report['changed_files']}",
                f"  model_elapsed_ms: {report['model_elapsed_ms'] if report['model_elapsed_ms'] is not None else '(missing)'}",
            ])
        with open(path, "w", encoding="utf-8") as f:
            f.write("\n".join(lines) + "\n")
        console.print(f"[bold green]scorecard:[/bold green] {path}")

    def proposal_dirs(self):
        return {
            "pending": PROPOSAL_PENDING_DIR,
            "accepted": PROPOSAL_ACCEPTED_DIR,
            "rejected": PROPOSAL_REJECTED_DIR,
        }

    def ensure_proposal_dirs(self):
        for directory in self.proposal_dirs().values():
            os.makedirs(directory, exist_ok=True)

    def proposal_path(self, proposal_id, status="pending"):
        directory = self.proposal_dirs().get(status)
        if not directory:
            raise ValueError(f"unknown proposal status: {status}")
        return os.path.join(directory, f"{proposal_id}.json")

    def write_json_file(self, path, payload):
        os.makedirs(os.path.dirname(path), exist_ok=True)
        tmp_path = path + ".tmp"
        with open(tmp_path, "w", encoding="utf-8") as f:
            json.dump(payload, f, ensure_ascii=False, indent=2, sort_keys=True)
            f.write("\n")
        os.replace(tmp_path, path)

    def load_json_file(self, path):
        with open(path, "r", encoding="utf-8") as f:
            return json.load(f)

    def list_json_files(self, directory):
        if not os.path.exists(directory):
            return []
        try:
            return sorted(name for name in os.listdir(directory) if name.endswith(".json"))
        except (FileNotFoundError, NotADirectoryError):
            return []

    def create_proposal(self, target, problem, proposed_tool_call, rationale, role="worker", risk="low", source="self-play", model_call_metadata=None, model_output_raw=None):
        self.ensure_proposal_dirs()
        created_at = datetime.now().strftime("%Y-%m-%d %H:%M:%S")
        nonce = uuid.uuid4().hex[:8]
        proposal_id = hashlib.sha256(
            f"{created_at}|{nonce}|{target}|{problem}|{proposed_tool_call}".encode("utf-8")
        ).hexdigest()[:12]
        proposal = {
            "id": proposal_id,
            "schema_version": 1,
            "created_at": created_at,
            "nonce": nonce,
            "status": "pending",
            "source": source,
            "role": role,
            "target": target,
            "problem": problem,
            "rationale": rationale,
            "proposed_tool_call": proposed_tool_call,
            "expected_validation": "./q1 --validate",
            "risk": risk,
            "gate": {
                "requires_oh_supervisor": True,
                "apply_automatically": False,
                "allowed_paths": DEFAULT_IMPROVE_ALLOWED_PATHS + [
                    "llm/q1/dataset/lora/WRITE_SAMPLES.md",
                ],
                "reject_if": [
                    "outside_allowed_paths",
                    "missing_tool_call",
                    "multiple_write_tools",
                    "validation_failure",
                    "server_port_policy_violation",
                ],
            },
        }
        if model_call_metadata is not None:
            proposal["model_call_metadata"] = model_call_metadata
        if model_output_raw is not None:
            proposal["model_output_raw"] = model_output_raw
        self.write_json_file(self.proposal_path(proposal_id), proposal)
        return proposal

    def create_safe_write_proposal(self):
        path = "llm/q1/dataset/lora/WRITE_SAMPLES.md"
        marker = f"- generated_at: {datetime.now().strftime('%Y-%m-%d %H:%M:%S')}; source: proposal-autopilot safe-write\n"
        full_path = self.tool_handler._resolve_path(path)
        if os.path.exists(full_path):
            with open(full_path, "r", encoding="utf-8") as f:
                old_content = f.read()
        else:
            old_content = "# q1 Safe Write Samples\n\n"
        new_content = old_content if old_content.endswith("\n") else old_content + "\n"
        new_content += marker
        proposed_tool_call = (
            f'<tool_call name="write_file" path="{path}">\n'
            f'{new_content}'
            '</tool_call>'
        )
        return self.create_proposal(
            target=path,
            problem="LoRA 데이터셋에 실제 write 성공 샘플을 만들기 위해 안전한 dataset manifest 파일을 갱신한다.",
            proposed_tool_call=proposed_tool_call,
            rationale="코드 본체가 아닌 dataset 문서 파일에만 쓰는 저위험 write proposal로 오감독 apply/rollback 경로를 검증한다.",
            role="worker",
            risk="low",
            source="proposal-autopilot-safe-write",
        )

    def self_play(self, cycles=1):
        created = []
        existing_count = self.proposal_count()
        for index in range(cycles):
            command = self.next_manual_job_command()
            if (existing_count + index) % 10 == 0:
                created.append(self.create_safe_write_proposal())
                continue
            if command.startswith("--new-function-job "):
                match = re.match(r'--new-function-job\s+(\S+)\s+--goal\s+"(.+)"$', command)
                if match:
                    target, goal = match.groups()
                else:
                    target, goal = "llm/q1/q1.py:next_manual_job_command", command
                path, func_name = parse_function_target(target)
                tool_call = (
                    f'<tool_call name="read_function" path="{path}" func_name="{func_name}" />'
                )
                problem = goal
                rationale = (
                    "self-play dry-run은 실제 파일을 수정하지 않고 오감독 검토용 문제 정의와 첫 조사 도구 호출만 제안한다."
                )
            else:
                target, goal = DEFAULT_SEED_FUNCTION_JOBS[(existing_count + index) % len(DEFAULT_SEED_FUNCTION_JOBS)]
                path, func_name = parse_function_target(target)
                tool_call = (
                    f'<tool_call name="read_function" path="{path}" func_name="{func_name}" />'
                )
                problem = goal
                rationale = (
                    f"큐가 비었으므로 deterministic seed 후보를 self-play 문제 정의 샘플로 전환한다. next_action={command}"
                )
            created.append(self.create_proposal(
                target=target,
                problem=problem,
                proposed_tool_call=tool_call,
                rationale=rationale,
                role="planner",
                risk="low",
            ))
        console.print("[bold]q1 self-play proposals[/bold]")
        for proposal in created:
            console.print(f"- {proposal['id']} {proposal['target']}: {proposal['problem']}")

    def build_self_play_prompt(self, target, func_source, problem):
        user_block = (
            f"target: {target}\n"
            f"problem: {problem}\n"
            "function source:\n"
            "---\n"
            f"{func_source}\n"
            "---\n"
            "출력 규칙: 위 시스템 지침 그대로."
        )
        return [
            {"role": "system", "content": SELF_PLAY_SYSTEM_PROMPT},
            {"role": "user", "content": user_block},
        ]

    def parse_model_proposal_text(self, text):
        text = text.strip()
        if not text:
            return {"kind": "malformed", "calls": [], "reason": "empty"}
        if text == "DONE_NO_CHANGE":
            return {"kind": "no_change", "calls": [], "reason": "ok"}
        try:
            calls = self.tool_handler.parse_tool_calls(text)
        except Exception as exc:
            return {"kind": "malformed", "calls": [], "reason": f"exception: {exc}"}
        if not calls:
            return {"kind": "malformed", "calls": [], "reason": "no_tool_call"}
        write_tools = {"replace", "replace_function", "write_file"}
        if any(call.get("name") in write_tools for call in calls):
            return {"kind": "write", "calls": calls, "reason": "ok"}
        read_tools = {"read_function", "read_file", "list_functions", "list_directory", "list_files"}
        if any(call.get("name") in read_tools for call in calls):
            return {"kind": "read_only", "calls": calls, "reason": "ok"}
        return {"kind": "malformed", "calls": calls, "reason": "no_read_or_write_tool"}

    def self_play_model_one(self, target, problem):
        try:
            path, func_name = parse_function_target(target)
            result = self.tool_handler.read_function({"path": path, "func_name": func_name})
        except Exception as exc:
            console.print(f"[yellow]self-play-model skipped {target}: {exc}[/yellow]")
            return None
        match = re.search(r"<tool_result name='read_function' status='success'>\n(.*)\n</tool_result>\s*$", result, re.DOTALL)
        func_source = match.group(1) if match else result
        messages = self.build_self_play_prompt(target, func_source, problem)
        started = time.monotonic()
        text = self.generate_response(messages, max_tokens=1024, disable_cache=True)
        elapsed_ms = int((time.monotonic() - started) * 1000)
        parsed = self.parse_model_proposal_text(text)
        if parsed["kind"] == "no_change":
            proposed_tool_call = "DONE_NO_CHANGE"
        else:
            proposed_tool_call = text
        prompt_payload = json.dumps(messages, ensure_ascii=False, sort_keys=True, separators=(",", ":"))
        proposal = self.create_proposal(
            target=target,
            problem=problem,
            proposed_tool_call=proposed_tool_call,
            rationale=f"model self-play proposal parsed as {parsed['kind']} ({parsed['reason']})",
            role="worker",
            risk="low",
            source="self-play-model-v1",
            model_call_metadata={
                "server_url": self.server_url,
                "elapsed_ms": elapsed_ms,
                "cache_hit": False,
                "prompt_hash": hashlib.sha256(prompt_payload.encode("utf-8")).hexdigest(),
                "max_tokens": 1024,
            },
            model_output_raw=text,
        )
        console.print(f"self_play_model_one: id={proposal['id']} target={target} kind={parsed['kind']} elapsed_ms={elapsed_ms}")
        return proposal

    def self_play_model(self, cycles=1):
        if not self._check_server_health():
            raise SystemExit("q1 server not reachable; refusing silent fallback")
        created = []
        kind_counts = {}
        existing_count = self.proposal_count()
        for index in range(cycles):
            command = self.next_manual_job_command()
            if command.startswith("--new-function-job "):
                match = re.match(r'--new-function-job\s+(\S+)\s+--goal\s+"(.+)"$', command)
                if match:
                    target, problem = match.groups()
                else:
                    target, problem = DEFAULT_SEED_FUNCTION_JOBS[(existing_count + index) % len(DEFAULT_SEED_FUNCTION_JOBS)]
            else:
                target, problem = DEFAULT_SEED_FUNCTION_JOBS[(existing_count + index) % len(DEFAULT_SEED_FUNCTION_JOBS)]
            proposal = self.self_play_model_one(target, problem)
            if proposal is None:
                continue
            created.append(proposal)
            parsed = self.parse_model_proposal_text(proposal.get("model_output_raw", ""))
            kind = parsed["kind"]
            kind_counts[kind] = kind_counts.get(kind, 0) + 1
        console.print("[bold]q1 self-play-model proposals[/bold]")
        console.print(f"created: {len(created)}")
        console.print(f"kind_counts: {kind_counts}")
        for proposal in created:
            console.print(f"- {proposal['id']} {proposal['target']}")

    def list_proposals(self, status="pending", limit=20):
        self.ensure_proposal_dirs()
        directory = self.proposal_dirs().get(status)
        if not directory:
            raise SystemExit(f"unknown proposal status: {status}")
        items = []
        for name in self.list_json_files(directory):
            path = os.path.join(directory, name)
            try:
                proposal = self.load_json_file(path)
            except (OSError, json.JSONDecodeError):
                continue
            items.append(proposal)
        items.sort(key=lambda item: item.get("created_at", ""), reverse=True)
        console.print(f"[bold]q1 proposals: {status} ({len(items)})[/bold]")
        for proposal in items[:limit]:
            console.print(f"- {proposal.get('id', '(missing)')} {proposal.get('target', '(missing)')}")
            console.print(f"  role: {proposal.get('role', '(missing)')} risk: {proposal.get('risk', '(missing)')}")
            console.print(f"  problem: {proposal.get('problem', '(missing)')}")

    def show_proposal(self, proposal_id):
        self.ensure_proposal_dirs()
        for status, directory in self.proposal_dirs().items():
            path = os.path.join(directory, f"{proposal_id}.json")
            if os.path.exists(path):
                proposal = self.load_json_file(path)
                console.print(json.dumps(proposal, ensure_ascii=False, indent=2, sort_keys=True), markup=False)
                return
        raise SystemExit(f"proposal not found: {proposal_id}")

    def find_proposal_file(self, proposal_id):
        self.ensure_proposal_dirs()
        for status, directory in self.proposal_dirs().items():
            path = os.path.join(directory, f"{proposal_id}.json")
            if os.path.exists(path):
                return status, path
        return None, None

    def gate_result(self, name, ok, message):
        return {"name": name, "ok": ok, "message": message}

    def parse_proposal_tool_call(self, proposal):
        proposed = proposal.get("proposed_tool_call", "")
        if proposed.strip() == "DONE_NO_CHANGE":
            return []
        calls = self.tool_handler.parse_tool_calls(proposed)
        return calls

    def validate_proposal_schema(self, proposal):
        required = [
            "id", "schema_version", "status", "target", "problem",
            "rationale", "proposed_tool_call", "expected_validation", "risk", "gate",
        ]
        missing = [field for field in required if field not in proposal]
        if missing:
            return False, f"missing fields: {', '.join(missing)}"
        if proposal.get("status") not in {"pending", "accepted", "rejected"}:
            return False, f"invalid status: {proposal.get('status')}"
        if not isinstance(proposal.get("gate"), dict):
            return False, "gate must be an object"
        return True, "schema ok"

    def proposal_allowed_paths(self, proposal):
        gate = proposal.get("gate") or {}
        allowed = gate.get("allowed_paths")
        if isinstance(allowed, list) and allowed:
            return allowed
        return DEFAULT_IMPROVE_ALLOWED_PATHS

    def static_reject_proposal(self, proposal, calls):
        write_tools = {"replace", "replace_function", "write_file"}
        write_calls = [call for call in calls if call.get("name") in write_tools]
        if len(write_calls) > 1:
            return "multiple write tools"
        dangerous = [
            "os.system", "subprocess.", "shutil.rmtree", "os.remove",
            "os.unlink", "eval(", "exec(", "__import__", "socket.",
        ]
        content = proposal.get("proposed_tool_call", "")
        for pattern in dangerous:
            if pattern in content:
                return f"dangerous pattern: {pattern}"
        return None

    def gate_proposal(self, proposal_id):
        status, path = self.find_proposal_file(proposal_id)
        if not path:
            raise SystemExit(f"proposal not found: {proposal_id}")
        proposal = self.load_json_file(path)
        results = []
        schema_ok, schema_msg = self.validate_proposal_schema(proposal)
        results.append(self.gate_result("schema", schema_ok, schema_msg))
        calls = []
        if schema_ok:
            try:
                calls = self.parse_proposal_tool_call(proposal)
                proposed = proposal.get("proposed_tool_call", "").strip()
                parse_ok = proposed == "DONE_NO_CHANGE" or bool(calls)
                results.append(self.gate_result("tool_parse", parse_ok, "tool parse ok" if parse_ok else "missing or malformed tool_call"))
            except Exception as exc:
                results.append(self.gate_result("tool_parse", False, str(exc)))
        else:
            results.append(self.gate_result("tool_parse", False, "schema failed"))
        if calls:
            allowed_paths = self.proposal_allowed_paths(proposal)
            outside = [
                call.get("path", "")
                for call in calls
                if call.get("path") and call.get("path") not in allowed_paths
            ]
            results.append(self.gate_result(
                "allowed_paths",
                not outside,
                "allowed paths ok" if not outside else f"outside allowed paths: {', '.join(outside)}",
            ))
            static_reject = self.static_reject_proposal(proposal, calls)
            results.append(self.gate_result(
                "static_reject",
                static_reject is None,
                "static checks ok" if static_reject is None else static_reject,
            ))
        else:
            results.append(self.gate_result("allowed_paths", True, "no write path"))
            results.append(self.gate_result("static_reject", True, "no write content"))
        ok = all(item["ok"] for item in results)
        proposal["gate_result"] = {
            "checked_at": datetime.now().strftime("%Y-%m-%d %H:%M:%S"),
            "ok": ok,
            "results": results,
        }
        self.write_json_file(path, proposal)
        console.print(f"[bold {'green' if ok else 'red'}]proposal gate: {'PASS' if ok else 'FAIL'}[/bold {'green' if ok else 'red'}]")
        console.print(f"id: {proposal_id}")
        for item in results:
            console.print(f"- {'PASS' if item['ok'] else 'FAIL'} {item['name']}: {item['message']}")
        return ok, proposal, calls, path

    def move_proposal(self, proposal, from_path, status):
        proposal["status"] = status
        proposal["moved_at"] = datetime.now().strftime("%Y-%m-%d %H:%M:%S")
        destination = self.proposal_path(proposal["id"], status=status)
        self.write_json_file(destination, proposal)
        if os.path.abspath(from_path) != os.path.abspath(destination) and os.path.exists(from_path):
            os.remove(from_path)
        return destination

    def reject_proposal(self, proposal_id, reason):
        status, path = self.find_proposal_file(proposal_id)
        if not path:
            raise SystemExit(f"proposal not found: {proposal_id}")
        proposal = self.load_json_file(path)
        proposal["rejection"] = {
            "reason": reason,
            "rejected_at": datetime.now().strftime("%Y-%m-%d %H:%M:%S"),
        }
        destination = self.move_proposal(proposal, path, "rejected")
        console.print(f"[bold red]proposal rejected:[/bold red] {destination}")

    def apply_proposal(self, proposal_id):
        ok, proposal, calls, path = self.gate_proposal(proposal_id)
        if not ok:
            proposal["rejection"] = {
                "reason": "gate failed",
                "rejected_at": datetime.now().strftime("%Y-%m-%d %H:%M:%S"),
            }
            destination = self.move_proposal(proposal, path, "rejected")
            console.print(f"[bold red]proposal moved to rejected:[/bold red] {destination}")
            raise SystemExit(1)
        write_calls = [call for call in calls if call.get("name") in {"replace", "replace_function", "write_file"}]
        if not write_calls:
            note = "DONE_NO_CHANGE proposal accepted without file writes" if proposal.get("proposed_tool_call", "").strip() == "DONE_NO_CHANGE" else "read-only proposal accepted without file writes"
            proposal["application"] = {
                "applied_at": datetime.now().strftime("%Y-%m-%d %H:%M:%S"),
                "changed_files": [],
                "validation_ok": True,
                "note": note,
            }
            destination = self.move_proposal(proposal, path, "accepted")
            console.print(f"[bold green]proposal accepted without writes:[/bold green] {destination}")
            return
        if len(write_calls) != 1:
            self.reject_proposal(proposal_id, "apply requires exactly one write tool")
            raise SystemExit(1)
        changed_files = sorted({write_calls[0].get("path")})
        before = self.snapshot_file_contents(changed_files)
        previous_tool_results = self.last_tool_results
        try:
            self.last_tool_results = []
            tool_result = self.tool_handler.execute_tools(proposal.get("proposed_tool_call"))
            self.last_tool_results.append(tool_result)
            if "status='success'" not in tool_result:
                raise RuntimeError(tool_result)
            report = self.run_validation_suite(f"proposal:{proposal_id}", changed_files)
            if not report["ok"]:
                restored = self.restore_file_contents(before, changed_files)
                proposal["application"] = {
                    "applied_at": datetime.now().strftime("%Y-%m-%d %H:%M:%S"),
                    "changed_files": changed_files,
                    "validation_ok": False,
                    "rolled_back_files": restored,
                    "tool_result": tool_result,
                }
                proposal["validation_report"] = report
                self.write_json_file(path, proposal)
                self.reject_proposal(proposal_id, "validation failed after apply; rollback completed")
                raise SystemExit(1)
            proposal["application"] = {
                "applied_at": datetime.now().strftime("%Y-%m-%d %H:%M:%S"),
                "changed_files": changed_files,
                "validation_ok": True,
                "tool_result": tool_result,
            }
            proposal["validation_report"] = report
            destination = self.move_proposal(proposal, path, "accepted")
            console.print(f"[bold green]proposal applied:[/bold green] {destination}")
        except Exception as exc:
            restored = self.restore_file_contents(before, changed_files)
            proposal["application"] = {
                "applied_at": datetime.now().strftime("%Y-%m-%d %H:%M:%S"),
                "changed_files": changed_files,
                "validation_ok": False,
                "rolled_back_files": restored,
                "error": str(exc),
            }
            self.write_json_file(path, proposal)
            self.reject_proposal(proposal_id, str(exc))
            raise SystemExit(1)
        finally:
            self.last_tool_results = previous_tool_results

    def pending_proposal_ids(self):
        self.ensure_proposal_dirs()
        ids = []
        for name in self.list_json_files(PROPOSAL_PENDING_DIR):
            ids.append(name[:-5])
        return ids

    def proposal_count(self):
        self.ensure_proposal_dirs()
        return sum(len(self.list_json_files(directory)) for directory in self.proposal_dirs().values())

    def proposal_autopilot(self, cycles=1, apply_limit=20):
        console.print("[bold]q1 proposal autopilot[/bold]")
        applied = 0
        failed = 0
        for cycle in range(1, cycles + 1):
            console.print(f"[bold]cycle {cycle}/{cycles}[/bold]")
            self.self_play(1)
            for proposal_id in self.pending_proposal_ids()[:apply_limit]:
                try:
                    self.apply_proposal(proposal_id)
                    applied += 1
                except SystemExit as exc:
                    if exc.code:
                        failed += 1
                    else:
                        applied += 1
                except Exception as exc:
                    failed += 1
                    console.print(f"[bold red]proposal autopilot error {proposal_id}: {exc}[/bold red]")
        self.export_lora_dataset()
        console.print("[bold]q1 proposal autopilot summary[/bold]")
        console.print(f"cycles: {cycles}")
        console.print(f"applied_or_accepted: {applied}")
        console.print(f"failed_or_rejected: {failed}")

    def proposal_lora_label(self, proposal, status):
        if status == "rejected":
            return "rejected"
        if status == "pending":
            return "proposal_pending"
        application = proposal.get("application") or {}
        if application.get("validation_ok") and application.get("changed_files"):
            return "positive"
        if application.get("validation_ok"):
            return "accepted_read_only"
        return "rejected"

    def proposal_quality_score(self, label):
        scores = {
            "positive": 0.9,
            "accepted_read_only": 0.7,
            "proposal_pending": 0.6,
            "rejected": 0.4,
        }
        return scores.get(label, 0.5)

    def proposal_to_lora_sample(self, proposal, label):
        system = (
            "당신은 q1 proposal 생성기다. 실제 파일을 수정하지 말고 오감독 검토용 산출물만 만든다. "
            "수정이 필요 없으면 DONE_NO_CHANGE, 조사가 필요하면 허용 도구 호출 하나만 출력한다."
        )
        user = "\n".join([
            f"target: {proposal.get('target', '')}",
            f"problem: {proposal.get('problem', '')}",
            f"rationale: {proposal.get('rationale', '')}",
            f"risk: {proposal.get('risk', '')}",
        ])
        response = proposal.get("proposed_tool_call") or "DONE_NO_CHANGE"
        dedupe_key = hashlib.sha256(
            json.dumps({
                "target": proposal.get("target"),
                "problem": proposal.get("problem"),
                "response": response,
            }, ensure_ascii=False, sort_keys=True).encode("utf-8")
        ).hexdigest()
        return {
            "messages": [
                {"role": "system", "content": system},
                {"role": "user", "content": user},
                {"role": "assistant", "content": response},
            ],
            "label": label,
            "source": f"proposal:{proposal.get('id', '(missing)')}",
            "target": proposal.get("target"),
            "quality_score": self.proposal_quality_score(label),
            "dedupe_key": dedupe_key,
            "metadata": {
                "label": label,
                "dedupe_key": dedupe_key,
                "quality_score": self.proposal_quality_score(label),
                "source_proposal": proposal.get("id"),
                "target": proposal.get("target"),
                "risk": proposal.get("risk"),
                "validation": proposal.get("expected_validation", "./q1 --validate"),
                "changed_files": (proposal.get("application") or {}).get("changed_files", []),
            },
        }

    def export_lora_dataset(self, output_name="q1_selfplay.jsonl"):
        self.ensure_proposal_dirs()
        os.makedirs(DATASET_ROOT, exist_ok=True)
        samples = []
        for status, directory in self.proposal_dirs().items():
            for name in self.list_json_files(directory):
                try:
                    proposal = self.load_json_file(os.path.join(directory, name))
                except (OSError, json.JSONDecodeError):
                    continue
                samples.append(self.proposal_to_lora_sample(proposal, self.proposal_lora_label(proposal, status)))
        unique = {}
        label_priority = {"rejected": 4, "positive": 3, "accepted_read_only": 2, "proposal_pending": 1}
        for sample in samples:
            previous = unique.get(sample["dedupe_key"])
            if previous is None or label_priority.get(sample["label"], 0) > label_priority.get(previous["label"], 0):
                unique[sample["dedupe_key"]] = sample
        output_path = os.path.join(DATASET_ROOT, output_name)
        with open(output_path, "w", encoding="utf-8") as f:
            for sample in unique.values():
                f.write(json.dumps(sample, ensure_ascii=False, sort_keys=True) + "\n")
        report_path = os.path.join(DATASET_ROOT, "q1_selfplay_report.md")
        counts = {}
        for sample in unique.values():
            counts[sample["label"]] = counts.get(sample["label"], 0) + 1
        lines = [
            "# q1 LoRA Dataset Report",
            "",
            f"- generated_at: {datetime.now().strftime('%Y-%m-%d %H:%M:%S')}",
            f"- output: {os.path.relpath(output_path, PROJECT_ROOT)}",
            f"- samples: {len(unique)}",
            f"- duplicate_removed: {len(samples) - len(unique)}",
            f"- average_quality_score: {round(sum(sample['quality_score'] for sample in unique.values()) / len(unique), 3) if unique else 0}",
            "",
            "## Labels",
        ]
        for label in sorted(counts):
            lines.append(f"- {label}: {counts[label]}")
        with open(report_path, "w", encoding="utf-8") as f:
            f.write("\n".join(lines) + "\n")
        console.print(f"[bold green]lora dataset:[/bold green] {output_path}")
        console.print(f"[bold green]dataset report:[/bold green] {report_path}")

if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--task", type=str)
    parser.add_argument("--self-check", action="store_true")
    parser.add_argument("--tool-self-test", action="store_true")
    parser.add_argument("--status", action="store_true")
    parser.add_argument("--list-jobs", action="store_true")
    parser.add_argument("--validate", action="store_true")
    parser.add_argument("--server-health", action="store_true")
    parser.add_argument("--restart-server", action="store_true")
    parser.add_argument("--archive-skipped", action="store_true")
    parser.add_argument("--archive-resolved-failures", action="store_true")
    parser.add_argument("--report-latest", action="store_true")
    parser.add_argument("--build-corpus", action="store_true")
    parser.add_argument("--council-status", action="store_true")
    parser.add_argument("--scorecard", action="store_true")
    parser.add_argument("--dry-run-auto", action="store_true")
    parser.add_argument("--compact-logs", action="store_true")
    parser.add_argument("--cycle-once", action="store_true")
    parser.add_argument("--cycle-compact-logs", action="store_true")
    parser.add_argument("--loop", action="store_true")
    parser.add_argument("--autopilot", action="store_true")
    parser.add_argument("--self-play", action="store_true")
    parser.add_argument("--self-play-model", action="store_true", help="Generate proposals via real q1 model server (Phase 1 self-play)")
    parser.add_argument("--proposal-autopilot", action="store_true")
    parser.add_argument("--proposal-apply-limit", type=int, default=20)
    parser.add_argument("--list-proposals", action="store_true")
    parser.add_argument("--proposal-status", choices=["pending", "accepted", "rejected"], default="pending")
    parser.add_argument("--show-proposal", type=str)
    parser.add_argument("--gate-proposal", type=str)
    parser.add_argument("--apply-proposal", type=str)
    parser.add_argument("--reject-proposal", type=str)
    parser.add_argument("--reject-reason", default="manual rejection")
    parser.add_argument("--export-lora-dataset", action="store_true")
    parser.add_argument("--dataset-output", default="q1_selfplay.jsonl")
    parser.add_argument("--cycles", type=int, default=1)
    parser.add_argument("--interval", type=int, default=0)
    parser.add_argument("--validate-every", type=int, default=5)
    parser.add_argument("--compact-every", type=int, default=5)
    parser.add_argument("--seed-default-jobs", action="store_true")
    parser.add_argument("--seed-from-failures", action="store_true")
    parser.add_argument("--seed-discovered-jobs", action="store_true")
    parser.add_argument("--seed-static-jobs", action="store_true")
    parser.add_argument("--seed-count", type=int, default=3)
    parser.add_argument("--queue-health", action="store_true")
    parser.add_argument("--failure-latest", action="store_true")
    parser.add_argument("--new-function-job", type=str)
    parser.add_argument("--goal", type=str)
    parser.add_argument("--improve-once", type=str)
    parser.add_argument("--improve-function", type=str)
    parser.add_argument("--auto-improve", action="store_true")
    parser.add_argument("--limit", type=int, default=1)
    parser.add_argument("--max-no-change", type=int, default=0)
    parser.add_argument("--improve-max-tokens", type=int, default=DEFAULT_IMPROVE_MAX_TOKENS)
    parser.add_argument("--task-max-tokens", type=int, default=DEFAULT_TASK_MAX_TOKENS)
    parser.add_argument("--backend", choices=["server", "local"], default="server")
    parser.add_argument("--server-url", default=DEFAULT_SERVER_URL)
    parser.add_argument("--server-timeout", type=int, default=DEFAULT_SERVER_TIMEOUT)
    args = parser.parse_args()
    direct_task = looks_like_direct_task(args.task)
    improve_allowed_paths = DEFAULT_IMPROVE_ALLOWED_PATHS
    if args.improve_function:
        improve_allowed_paths = [parse_function_target(args.improve_function)[0]]
    load_model = args.backend == "local" and not args.self_check and not args.tool_self_test and not args.validate and not args.server_health and not args.restart_server and not args.archive_skipped and not args.archive_resolved_failures and not args.report_latest and not args.build_corpus and not args.council_status and not args.scorecard and not args.dry_run_auto and not args.compact_logs and not args.cycle_once and not args.loop and not args.autopilot and not args.self_play and not args.self_play_model and not args.proposal_autopilot and not args.list_proposals and not args.show_proposal and not args.gate_proposal and not args.apply_proposal and not args.reject_proposal and not args.export_lora_dataset and not args.seed_default_jobs and not args.seed_from_failures and not args.seed_discovered_jobs and not args.seed_static_jobs and not args.queue_health and not args.failure_latest and not direct_task and not args.auto_improve
    engine = Q1Engine(
        load_model=load_model,
        allowed_write_paths=improve_allowed_paths if args.improve_once or args.improve_function or args.auto_improve or args.autopilot else None,
        backend=args.backend,
        server_url=args.server_url,
        server_timeout=args.server_timeout,
        improve_max_tokens=args.improve_max_tokens,
    )
    if args.self_check:
        engine.self_check()
    elif args.tool_self_test:
        engine.tool_self_test()
    elif args.status:
        engine.print_status()
    elif args.list_jobs:
        engine.print_jobs()
    elif args.validate:
        engine.validate()
    elif args.server_health:
        engine.server_health()
    elif args.restart_server:
        engine.restart_server()
    elif args.archive_skipped:
        engine.archive_skipped()
    elif args.archive_resolved_failures:
        engine.archive_resolved_failures()
    elif args.report_latest:
        engine.print_latest_report()
    elif args.build_corpus:
        engine.build_corpus()
    elif args.council_status:
        engine.print_council_status()
    elif args.scorecard:
        engine.build_scorecard()
    elif args.dry_run_auto:
        engine.dry_run_auto(args.limit)
    elif args.compact_logs:
        engine.compact_logs()
    elif args.cycle_once:
        try:
            engine.cycle_once(args.limit, max_no_change=args.max_no_change, compact_logs=args.cycle_compact_logs)
        except RuntimeError as exc:
            console.print(f"[bold red]{exc}[/bold red]")
            raise SystemExit(1)
    elif args.loop:
        try:
            engine.loop(args.cycles, args.interval, args.limit, max_no_change=args.max_no_change, compact_logs=args.cycle_compact_logs)
        except RuntimeError as exc:
            console.print(f"[bold red]{exc}[/bold red]")
            raise SystemExit(1)
    elif args.autopilot:
        try:
            engine.autopilot(
                args.cycles,
                args.interval,
                args.limit,
                args.seed_count,
                max_no_change=args.max_no_change,
                validate_every=args.validate_every,
                compact_every=args.compact_every,
            )
        except RuntimeError as exc:
            console.print(f"[bold red]{exc}[/bold red]")
            raise SystemExit(1)
    elif args.self_play:
        engine.self_play(args.cycles)
    elif args.self_play_model:
        engine.self_play_model(cycles=args.cycles)
    elif args.proposal_autopilot:
        engine.proposal_autopilot(args.cycles, apply_limit=args.proposal_apply_limit)
    elif args.list_proposals:
        engine.list_proposals(args.proposal_status, limit=args.limit)
    elif args.show_proposal:
        engine.show_proposal(args.show_proposal)
    elif args.gate_proposal:
        ok, _, _, _ = engine.gate_proposal(args.gate_proposal)
        if not ok:
            raise SystemExit(1)
    elif args.apply_proposal:
        engine.apply_proposal(args.apply_proposal)
    elif args.reject_proposal:
        engine.reject_proposal(args.reject_proposal, args.reject_reason)
    elif args.export_lora_dataset:
        engine.export_lora_dataset(args.dataset_output)
    elif args.seed_default_jobs:
        engine.seed_default_jobs(args.seed_count)
    elif args.seed_from_failures:
        engine.seed_from_failures(args.seed_count)
    elif args.seed_discovered_jobs:
        engine.seed_discovered_jobs(args.seed_count)
    elif args.seed_static_jobs:
        engine.seed_static_jobs(args.seed_count)
    elif args.queue_health:
        engine.queue_health()
    elif args.failure_latest:
        engine.print_latest_failure()
    elif args.new_function_job:
        if not args.goal:
            console.print("[bold red]--goal is required with --new-function-job[/bold red]")
            raise SystemExit(1)
        engine.create_function_job(args.new_function_job, args.goal)
    elif args.improve_once:
        try:
            engine.improve_once(args.improve_once)
        except RuntimeError as exc:
            console.print(f"[bold red]{exc}[/bold red]")
            raise SystemExit(1)
    elif args.improve_function:
        try:
            engine.improve_function(args.improve_function)
        except RuntimeError as exc:
            console.print(f"[bold red]{exc}[/bold red]")
            raise SystemExit(1)
    elif args.auto_improve:
        try:
            engine.auto_improve(args.limit, max_no_change=args.max_no_change)
        except RuntimeError as exc:
            console.print(f"[bold red]{exc}[/bold red]")
            raise SystemExit(1)
    elif args.task:
        try:
            engine.chat(args.task, max_tokens=args.task_max_tokens)
        except RuntimeError as exc:
            console.print(f"[bold red]{exc}[/bold red]")
            raise SystemExit(1)
    else:
        session = PromptSession(history=FileHistory(os.path.join(PROJECT_ROOT, ".q1_history")))
        while True:
            try:
                inp = session.prompt("Head > ")
                if inp.lower() in ["exit", "quit"]: break
                engine.chat(inp, max_tokens=args.task_max_tokens)
            except RuntimeError as exc:
                console.print(f"[bold red]{exc}[/bold red]")
            except KeyboardInterrupt: break
