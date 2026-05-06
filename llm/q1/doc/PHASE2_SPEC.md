# Phase 2 Spec — proposal_autopilot에 model 모드 통합

## 목표
`proposal_autopilot`이 결정론 self-play 대신 **모델 self-play**를 돌릴 수 있게 한다. 기존 결정론 모드는 fallback으로 유지.

## 수정 대상 (이 외엔 손대지 말 것)
- `llm/q1/q1.py` — `proposal_autopilot` 함수, argparse, dispatch
- `llm/q1/tests/test_core_fixtures.py` — Phase 2 테스트 추가

## A. `proposal_autopilot` 시그니처 확장

기존:
```python
def proposal_autopilot(self, cycles=1, apply_limit=20):
```
신규:
```python
def proposal_autopilot(self, cycles=1, apply_limit=20, mode="deterministic"):
```

`mode` 허용 값: `"deterministic"`, `"model"`. 그 외 값은 `raise SystemExit(f"unknown autopilot mode: {mode}")`.

## B. 동작 분기

### `mode == "deterministic"` (기존 동작 유지)
- `self.self_play(1)` 호출 → 기존 그대로.

### `mode == "model"` (신규)
- 사이클 시작 전 서버 헬스체크 1회: `if not self._check_server_health(): raise SystemExit("q1 server not reachable; refusing silent fallback")`.
- 각 사이클:
  - target/problem 선택은 `self_play_model` 함수 내부의 기존 선택 로직과 동일 패턴 사용. **중복 금지** — `self_play_model_one(target, problem)` 한 번 호출.
  - 헬퍼: `_pick_self_play_target(existing_count, index)` 라는 private 메서드를 만들어 `self_play_model`과 `proposal_autopilot(mode="model")` 둘 다에서 호출. 기존 `self_play_model` 안의 target 선택 코드를 그 헬퍼로 추출(refactor). 기존 `self_play_model` 동작은 100% 동일하게 유지.
  - proposal 생성 후 곧장 기존 `apply_proposal(proposal_id)` 호출 (기존 결정론 분기와 동일 흐름).

## C. 통계 수집

`proposal_autopilot`이 다음을 추적해 마지막에 출력:
- `cycles`
- `applied_or_accepted` (기존)
- `failed_or_rejected` (기존)
- **신규** `kind_counts`: `{"write": N, "no_change": N, "read_only": N, "malformed": N}` — `model` 모드에서만 채움. `deterministic` 모드는 빈 dict.

`kind_counts`는 각 사이클 직후 proposal의 `model_output_raw`를 다시 `parse_model_proposal_text`로 분류해서 누적. raw가 없으면 (deterministic 분기) skip.

## D. CLI

argparse에 추가:
```python
parser.add_argument("--autopilot-mode", choices=["deterministic", "model"], default="deterministic",
                    help="proposal autopilot mode (default deterministic)")
```

dispatch:
```python
elif args.proposal_autopilot:
    engine.proposal_autopilot(args.cycles, apply_limit=args.proposal_apply_limit, mode=args.autopilot_mode)
```

(기존 `args.cycles`, `args.proposal_apply_limit`은 그대로.)

## E. 테스트 (추가만, 기존 테스트 손대지 말 것)

`test_core_fixtures.py`에:
1. `test_autopilot_mode_validation`: `proposal_autopilot(mode="invalid")` 호출 시 SystemExit 발생.
2. `test_autopilot_deterministic_kind_counts_empty`: deterministic 모드 1 사이클 호출 후 `kind_counts == {}` (또는 모든 값 0). 외부 의존성 회피를 위해 `_pick_self_play_target`을 mock으로 고정해도 됨.

테스트는 unittest 스타일, 기존 fixture 패턴 따라 작성. 모델 서버 mock이 어려우면 `mode="model"` 통합 테스트는 생략하고 `--validate` 통합으로 대체.

## 수락 기준
- `./q1 --validate` PASS.
- 새 fixture 2개 PASS.
- `./q1 --proposal-autopilot --cycles 1 --autopilot-mode deterministic` 기존 동작과 동일하게 작동 (1 cycle 결정론 사이클 + 기존 apply 흐름).
- `./q1 --proposal-autopilot --cycles 1 --autopilot-mode model` 호출:
  - 서버 살아있으면 1건 모델 proposal 생성 + apply_proposal 호출까지 진행.
  - 서버 죽이면 SystemExit으로 즉시 종료.

## 안전 규칙
- 기존 `self_play()` (결정론), `self_play_model()`, `self_play_model_one()`, `apply_proposal()`, `gate_proposal()` 함수 본문 변경 금지.
- 단 `_pick_self_play_target` refactor를 위해 `self_play_model` 내부의 target 선택 블록만 헬퍼 호출로 치환하는 것은 허용. 동작 동일성 유지 필수.
- 새 import 금지.
- LoRA dataset, proposal/* 파일 손대지 말 것.

## 보고 형식
- 변경된 파일 목록
- 추가된 fixture 이름
- `./q1 --validate` 결과
- `./q1 --proposal-autopilot --cycles 1 --autopilot-mode deterministic` 마지막 줄
- (서버 살아있으면) `./q1 --proposal-autopilot --cycles 1 --autopilot-mode model` 마지막 줄
