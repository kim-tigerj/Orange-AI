# Phase 0 Spec — Hermes/Qwen native tool_call 포맷 도입

## 목표
q1의 자체 XML(`<tool_call name="..." path="..." />`)을 버리지 않고, Hermes/Qwen native 포맷(`<tool_call>{"name":"...","arguments":{...}}</tool_call>`)을 **이중 파서**로 추가한다. 신규 self-play 출력과 system prompt는 Hermes로 통일. 기존 proposal/dataset은 절대 깨지 않는다.

## 수정 대상 파일 (이 외엔 손대지 말 것)
- `llm/q1/core/handlers.py` — `parse_tool_calls` 확장
- `llm/q1/q1.py` — `SELF_PLAY_SYSTEM_PROMPT`, `proposal_to_lora_sample`, `self_play()` 결정론 분기 템플릿
- `llm/q1/tests/test_core_fixtures.py` — Hermes 파서 fixture 케이스 추가
- (필요 시) `llm/q1/doc/PHASE0_SPEC.md` 갱신은 금지 — 본 문서는 spec.

## A. handlers.py — `parse_tool_calls` 확장

기존 정규식 기반 파서는 **그대로 둔다** (block_pattern, generic_block_pattern, self-closing). 그 뒤에 Hermes 파서를 추가:

### Hermes 파서 사양
- 정규식: `<tool_call>\s*(\{.*?\})\s*</tool_call>` (DOTALL, 비탐욕)
- 매칭된 문자열을 `json.loads`. 실패 시 그 매치는 skip(예외 흘리지 않음 — 기존 파서 거동과 동일).
- 파싱된 객체:
  - 최상위 `name`(str) 필수. 없으면 skip.
  - `arguments`(dict)가 있으면 그 안의 키/값을 call dict에 평탄화(flatten). `path`, `func_name`, `content`, `start_line`, `end_line`, `command`, `old_string`, `new_string` 등 기존 핸들러가 받는 키들을 그대로 셋팅.
  - `arguments`가 dict가 아니면 skip.
  - 평탄화 시 최상위 `name`과 충돌하면 최상위 우선.
- 결과를 `calls` 리스트에 append.

### 호출 순서
기존 3종 파서 결과를 모은 뒤 Hermes 파서를 마지막에 적용. **동일 텍스트가 두 형식 모두로 매칭되는 일은 없음**(`<tool_call ...>` vs `<tool_call>`로 시작 패턴이 다름) — 그래도 안전하게 Hermes 매치 영역을 마스킹한 뒤 추가하면 좋지만, MVP는 단순 append 허용.

### 안전 규칙
- `arguments` 내부에 `__import__`, `eval(`, `exec(`, `os.system`, `subprocess.`, `shutil.rmtree`, `socket.` 같은 위험 패턴이 들어 있어도 파서 단계에선 감지하지 않음 — 기존 `static_reject_proposal` (q1.py:3174 부근)에서 이미 처리. 단 `static_reject_proposal`이 `proposal["proposed_tool_call"]` 원문을 검사하므로 Hermes 형식에서도 동일하게 동작 (JSON 안에 패턴 문자열이 있으면 잡힘).

## B. q1.py — `SELF_PLAY_SYSTEM_PROMPT` 재작성

기존(잘못된) 내용:
```
"정확히 하나의 write tool_call(replace, replace_function, write_file 중 하나)을 출력한다"
```

신규(Hermes 명시):
```
당신은 q1 정팀장이다. 주어진 함수에 대해 실제 개선이 필요하면, 정확히 하나의 도구 호출을 아래 형식으로 출력한다. 그 외 텍스트나 설명은 출력하지 않는다.

<tool_call>
{"name": "replace_function", "arguments": {"path": "llm/q1/q1.py", "func_name": "함수명", "content": "수정된 함수 전체 소스"}}
</tool_call>

허용 도구: replace_function, replace, write_file (write 계열). read_function, read_file, list_functions, list_directory (read 계열).
허용 경로: llm/q1/q1.py, llm/q1/core/handlers.py, llm/q1/core/analyzer.py, llm/q1/q1_server.py, llm/q1/persona/PROMPT.md, llm/q1/dataset/lora/WRITE_SAMPLES.md.
위험 패턴(os.system, subprocess., shutil.rmtree, eval, exec, __import__, socket.) 사용 금지.

개선이 불필요하면 정확히 "DONE_NO_CHANGE" 한 줄만 출력한다.
```

위 문자열을 모듈 상수 `SELF_PLAY_SYSTEM_PROMPT`에 그대로 박는다.

## C. q1.py — `self_play()` 결정론 분기 템플릿 변경

기존 (q1.py:3079-3081 부근):
```python
tool_call = (
    f'<tool_call name="read_function" path="{path}" func_name="{func_name}" />'
)
```

신규:
```python
tool_call = (
    '<tool_call>\n'
    + json.dumps({"name": "read_function", "arguments": {"path": path, "func_name": func_name}}, ensure_ascii=False)
    + '\n</tool_call>'
)
```

`create_safe_write_proposal()` 의 write_file tool_call도 동일 패턴으로 Hermes 형식으로 변경.

## D. q1.py — `proposal_to_lora_sample` 시스템 프롬프트 동기화

`proposal_to_lora_sample` 함수 내 `system` 변수를 `SELF_PLAY_SYSTEM_PROMPT`와 정확히 동일하게 만든다 (학습/추론 일치). 즉 그 자리에서 `system = SELF_PLAY_SYSTEM_PROMPT`로 단순 참조.

## E. tests — Hermes 파서 케이스 추가

`llm/q1/tests/test_core_fixtures.py`에 다음 케이스 추가 (기존 테스트 보존):

1. `test_parse_tool_calls_hermes_self_closing_equivalent`: Hermes 형식의 read_function이 정확히 1개 call로 파싱되고 `name`, `path`, `func_name`이 평탄화됐는지.
2. `test_parse_tool_calls_hermes_replace_function`: `arguments.content`에 멀티라인 코드 들어간 케이스가 정상 파싱되는지.
3. `test_parse_tool_calls_legacy_xml_still_works`: 기존 자체 XML 형식이 여전히 파싱되는지.
4. `test_parse_tool_calls_hermes_malformed_json_skipped`: `<tool_call>{not json}</tool_call>` 같은 잘못된 JSON은 skip하고 다른 매치는 살아남는지.
5. `test_parse_tool_calls_mixed_formats`: 동일 텍스트에 자체 XML + Hermes 둘 다 있을 때 합산되는지.

## 수락 기준
- `./q1 --validate` PASS (이전 PASS 상태 유지).
- 새 fixture 5개 모두 PASS.
- `./q1 --self-play --cycles 1` 실행 시 결정론 proposal에 Hermes 형식이 들어가고, 그 proposal이 `gate_proposal`을 통과 (`tool_parse=PASS`).
- `./q1 --self-play-model --cycles 1` 실행 시 모델이 Hermes 형식을 자연스럽게 출력하고 `kind=write`(또는 `no_change`) 비율이 0이 아님 (이전 malformed였던 게 개선되는지 확인).
- 기존 `proposal/accepted/*.json` 파일 1건 수동 검증: `parse_proposal_tool_call`이 여전히 calls를 정상 추출 (기존 데이터 깨지지 않음 검증).

## 안전 규칙
- 기존 정규식 3종 파서를 **삭제하지 말 것**. 추가만.
- `replace_function`, `read_function`, `write_file` 등 핸들러 함수 자체는 손대지 말 것 — call dict 입력 기대값을 유지.
- 기존 proposal 파일 (`llm/q1/proposal/**/*.json`)은 절대 손대지 말 것.
- 새로운 의존성 import 금지. `json`, `re`, `html` 등 기존 import 그대로.
- LoRA dataset 파일(`q1_selfplay.jsonl`)은 이번 작업에서 재생성하지 말 것 (Phase 3에서).

## 보고 형식 (작업 끝나고 마지막 메시지)
- 변경된 파일 목록
- 추가된 fixture 테스트 이름 5개
- `./q1 --validate` 결과
- `./q1 --self-play --cycles 1` 결과 한 줄 (생성된 proposal id 1개와 source)
- `./q1 --self-play-model --cycles 1` 결과 한 줄 (`kind_counts` 포함)
