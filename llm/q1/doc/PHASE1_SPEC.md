# Phase 1 Spec — 진짜 self-play MVP

## 목표
q1 모델 서버(`/generate_stream`)를 호출해 *모델이 직접 생성한* proposal JSON을 pending에 떨군다. 정해진 lean 결정 4가지가 적용됨.

## 적용된 결정 (재확인 금지, 그대로 진행)

1. self-play 호출 시 **응답 캐시 우회** — `disable_cache=True`로 server 호출.
2. problem 문장은 **기존 hardcoded goal 재사용** (`DEFAULT_SEED_FUNCTION_JOBS` 등).
3. 모델 잡음도 **proposal로 저장** — gate가 reject해서 학습 신호로 보존.
4. CLI는 **별도 명령** `--self-play-model`. 기존 `--self-play`(결정론)는 그대로 유지.

## 변경 대상

파일: `llm/q1/q1.py` 만. 다른 파일은 손대지 않는다.

### A. 시그니처 확장 (기존 호출자 영향 0)

`generate_response_server(self, messages, max_tokens)` →
`generate_response_server(self, messages, max_tokens, *, disable_cache=False)`

변경 의도:
- `disable_cache=True`이면 `read_response_cache` / `write_response_cache` 건너뜀.
- 기본 `False` 이므로 기존 호출자 영향 없음.
- `generate_response(messages, max_tokens)`도 동일하게 `disable_cache` keyword-only 인자를 추가하여 backend 분기에 그대로 패스. 단 `generate_response_local`은 캐시를 안 쓰니 인자만 받고 무시.

### B. 신규 메서드 (Q1Tool 클래스 내, self_play 근처 위치)

#### `build_self_play_prompt(self, target, func_source, problem)` → `list[dict]`

반환:
```python
[
  {"role": "system", "content": SELF_PLAY_SYSTEM_PROMPT},
  {"role": "user",   "content": user_block},
]
```

`SELF_PLAY_SYSTEM_PROMPT` (모듈 상수로 정의):
```
당신은 q1 정팀장이다. 주어진 함수에 대해 실제 개선이 필요하면 정확히 하나의 write tool_call(replace, replace_function, write_file 중 하나)을 출력한다.
개선이 불필요하면 "DONE_NO_CHANGE"만 출력한다.
그 외 텍스트, 설명, 주석은 출력하지 않는다.
허용 경로 밖, 위험 패턴(os.system, subprocess., shutil.rmtree, eval, exec, __import__, socket.)은 사용하지 않는다.
```

`user_block`:
```
target: {target}
problem: {problem}
function source:
---
{func_source}
---
출력 규칙: 위 시스템 지침 그대로.
```

#### `parse_model_proposal_text(self, text)` → `dict`

반환 dict:
```python
{
  "kind": "write" | "no_change" | "read_only" | "malformed",
  "calls": [...],   # tool_handler.parse_tool_calls 결과
  "reason": str,    # "ok" | "empty" | "no_tool_call" | "exception: ..."
}
```

규칙:
- 입력 정규화: `text.strip()`.
- `text == "DONE_NO_CHANGE"` (단독): kind=`no_change`, calls=[].
- 그 외엔 `self.tool_handler.parse_tool_calls(text)` 호출. 예외 발생 시 kind=`malformed`, reason=`f"exception: {exc}"`.
- calls 비어있으면 kind=`malformed`, reason=`no_tool_call`.
- calls 중 write tool(`replace|replace_function|write_file`) 1개 이상이면 kind=`write`. 정확히 1개여야 한다는 강제는 하지 않음(이 검사는 기존 gate가 함).
- write 0개 + read 1개 이상이면 kind=`read_only`.

#### `self_play_model_one(self, target, problem)` → `dict | None`

흐름:
1. `path, func_name = parse_function_target(target)`
2. `tool_handler.read_function(path, func_name)` 호출하여 함수 소스 확보. 실패 시 `console.print(...)` + None 반환 (한 사이클 스킵).
3. `messages = self.build_self_play_prompt(target, func_source, problem)`
4. `started = time.monotonic()`; `text = self.generate_response(messages, max_tokens=1024, disable_cache=True)`; `elapsed_ms`.
5. `parsed = self.parse_model_proposal_text(text)`
6. proposed_tool_call 결정:
   - kind=`no_change`: `"DONE_NO_CHANGE"`
   - kind=`write`/`read_only`: 모델 출력 `text` 그대로 (parse_tool_calls가 받아낼 수 있음)
   - kind=`malformed`: `text` 그대로 — gate가 `tool_parse`에서 fail하게 둠
7. `proposal = self.create_proposal(...)` 호출. 새 인자 추가:
   - `source="self-play-model-v1"`
   - `model_call_metadata={"server_url": self.server_url, "elapsed_ms": elapsed_ms, "cache_hit": False, "prompt_hash": <sha256(messages)>, "max_tokens": 1024}`
   - `model_output_raw=text`
8. `console.print(...)` 1줄: id, target, kind, elapsed.
9. proposal dict 반환.

#### `self_play_model(self, cycles=1)` → None

- 서버 헬스체크 1회: `_check_server_health()` 헬퍼 사용. 실패 시 `raise SystemExit("q1 server not reachable; refusing silent fallback")` — 결정론 fallback 금지.
- target 선택은 기존 deterministic 흐름 재사용:
  - `next_manual_job_command()` 또는 `DEFAULT_SEED_FUNCTION_JOBS` 순환.
  - 단 시작 인덱스는 `proposal_count() % len(DEFAULT_SEED_FUNCTION_JOBS)`로 (기존 self_play와 동일 패턴).
- 각 사이클마다 `self_play_model_one(target, problem)` 호출.
- 마지막에 생성 결과 요약 출력 (id, kind 분포).

(`_check_server_health()`이 없으면 `urllib.request.urlopen(f"{self.server_url}/health", timeout=5)` 한 줄로 즉석 추가. 이미 있으면 그걸 사용.)

### C. `create_proposal` 시그니처 확장

```python
def create_proposal(self, target, problem, proposed_tool_call, rationale,
                    role="worker", risk="low", source="self-play",
                    model_call_metadata=None, model_output_raw=None):
```

변경:
- `model_call_metadata`가 not None이면 proposal dict에 그 키 추가.
- `model_output_raw`가 not None이면 proposal dict에 그 키 추가.
- 기본 None이므로 기존 호출자 영향 없음.

### D. CLI 분기 추가

argparse 블록(파일 끝 `if __name__ == "__main__":` 영역)에 다음 추가:

```python
parser.add_argument("--self-play-model", action="store_true",
                    help="Generate proposals via real q1 model server (Phase 1 self-play)")
parser.add_argument("--cycles", type=int, default=1,
                    help="Number of cycles for --self-play-model (default 1)")
```

`--cycles`는 기존에 다른 명령에서 쓰면 그 자리 인자명 그대로 둘 것. 충돌하면 `--model-cycles`로 분리.

dispatch 분기에 추가:
```python
elif args.self_play_model:
    tool.self_play_model(cycles=args.cycles)
```

기존 `--self-play` 분기는 절대 변경 금지.

## 수락 기준 (Codex 끝나고 검증할 항목)

`./q1 --validate` 가 PASS여야 한다 (기존 자가 테스트 보호).

추가 manual 검증:
1. `./q1 --self-play-model --cycles 1` 성공 실행. pending에 `source: "self-play-model-v1"` proposal 1건 생성.
2. 그 proposal JSON에 `model_call_metadata`, `model_output_raw` 필드 존재.
3. `./q1 --self-play` (기존 결정론) 그대로 작동, 결과물의 source는 `self-play`.
4. server 죽인 상태에서 `./q1 --self-play-model --cycles 1` 호출 시 SystemExit으로 즉시 종료 (silent fallback 없음).

## 안전 규칙 (Codex가 반드시 지킬 것)

- **git commit, git push 절대 금지.** 변경은 working tree에만.
- 다른 파일 손대지 말 것 (`q1.py`만 수정).
- 기존 함수 시그니처를 keyword-only 추가 외 방식으로 깨지 말 것.
- 기존 `--self-play` 분기, `self_play()` 메서드, `proposal_autopilot` 메서드는 손대지 말 것 (Phase 2에서 처리).
- 새 코드에서 `os.system`, `subprocess.`, `eval`, `exec`, `shutil.rmtree` 사용 금지.
- 작업 끝나고 `./q1 --validate` 실행하여 PASS 확인 후 종료 보고.

## 보고 형식

작업 끝나고 다음 출력:
- 추가/변경된 함수 목록
- `./q1 --validate` 결과
- `./q1 --self-play-model --cycles 1` 결과 한 줄
- 생성된 proposal id 1개 (있으면)
