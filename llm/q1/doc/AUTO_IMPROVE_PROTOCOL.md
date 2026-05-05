# q1 Auto-Improve Protocol

## Purpose
q1의 목표는 대표님 개입 없이 정팀장이 자기 자신을 반복 개선하고, 오감독 기준으로 검증 결과를 남기는 것이다. 단, 무제한 자율 실행은 금지한다. 먼저 저비용 폐쇄 루프를 만든 뒤 반복 범위를 넓힌다.

## Cost Control
- OpenAI/Codex는 매 반복을 관찰하지 않는다. 로컬 q1이 반복 실행하고, Codex는 최신 요약 보고서와 실패 분기만 검수한다.
- 긴 로그(`RESPONSE_HISTORY.md`, `BLACKBOX_*.md`) 전체를 기본 검토 대상으로 삼지 않는다.
- 기본 검토 대상은 `llm/q1/job/report/`의 최신 보고서, 변경 파일 diff, 실패 명령 출력이다.
- 정팀장의 작업 단위는 파일 전체가 아니라 함수 단위로 제한한다.
- 같은 job에서 `NO_CHANGE` 또는 같은 실패가 반복되면 자동 중단한다.
- 모델은 `q1_server.py`로 상주시킨다. 반복 CLI 실행에서 모델을 매번 로드하지 않는다.

## Server Backend
- 대표님이 별도 터미널에서 `./venv/bin/python llm/q1/q1_server.py`를 실행한다.
- q1 클라이언트 기본 백엔드는 `--backend server`이다.
- 서버는 모델 로드, 요청 수신, prompt 크기, 생성 시간, 출력 크기를 화면에 출력한다.
- 서버가 꺼져 있으면 q1 클라이언트는 한 줄 오류와 exit code 1로 실패한다.
- 장애 대기 시간은 `--server-timeout`으로 조정한다.

## Execution Ladder
1. `--server-health`: 대표님이 켜둔 q1 서버 상태와 응답 시간을 먼저 확인한다.
2. `--seed-default-jobs`, `--seed-from-failures`, `--seed-discovered-jobs`: todo가 비었을 때 안전한 함수 단위 job을 만든다.
3. `--cycle-once`: health, archive, dry-run, auto-improve, validate를 한 번 수행한다.
4. `--loop --cycles N --interval SEC`: 제한된 횟수만 반복한다.
5. `latest_summary.md`: 다음 세션과 오감독이 긴 로그 없이 상태를 파악할 수 있는 짧은 요약을 생성한다.

## Guardrails
- 쓰기 허용 파일은 job마다 명시한다.
- 기본 허용 파일은 `llm/q1/q1.py`, `llm/q1/core/handlers.py`, `llm/q1/persona/PROMPT.md`로 제한한다.
- 경로는 프로젝트 루트 밖으로 나갈 수 없다.
- Python 파일 수정은 문법 검증을 통과해야 한다.
- 검증 명령은 오감독 런타임이 실행한다. 정팀장은 검증 명령을 임의로 생략하거나 성공으로 주장할 수 없다.
- 같은 도구 호출 묶음이 반복되면 루프를 중단한다.

## Report Format
각 반복은 `llm/q1/job/report/`에 다음 필드를 남긴다.

```md
verdict: DONE | NO_CHANGE | CONTINUE | FAIL
job:
changed_files:
validation:
failure_reason:
next_recommended_job:
```

## Next Small Goal
다음 구현 목표는 발견된 후보 중 실제 개선 가치가 낮은 단순 함수와 위험한 실행 제어 함수를 더 잘 거르고, 실패 job은 복구 target으로 변환해 반복 비용을 줄이는 것이다.
