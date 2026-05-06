# task: council_execution_guard

> 3자회동(2026-05-06)에서 드러난 실행자 중복 문제를 코드 레벨에서 차단한다.
> Codex가 무단으로 orange.build를 반복 요청한 것이 직접 원인.

## 목적

이번 세션에서 Codex는 코드 변경 없이 orange.build를 3회(seq 101, 105, 110) 요청했다.
`orange.build`는 실행자가 코드 변경 후 정팀장 provider가 요청하는 도구다.
Non-manager provider(Gemini, Codex)가 독립적으로 build를 요청할 수 없도록 coordination 레이어에서 guard가 필요하다.

## 범위

### 1. Coordination.cpp / Coordination.h — build 요청 ownership 체크

`orange.build` 도구 요청을 처리하는 경로에서:
- 요청자가 coordinator(정팀장 provider)인지 확인한다.
- Non-coordinator provider의 build 요청은 무시하고 로그에 "build 요청 거부: 실행자가 아님 (provider: X)" 를 남긴다.
- coordinator 정보는 기존 `m_coordinatorProvider` 필드 또는 동등한 변수를 활용한다.

실제 필드명은 파일 확인 후 맞춘다. 변경은 guard 조건 1개 추가 수준으로 최소화한다.

### 2. BackendManager.cpp — 동일 턴 내 중복 build 요청 차단

같은 사용자 메시지 처리 턴 내에서 `orange.build`가 이미 요청된 경우:
- 두 번째 이후 요청은 무시한다.
- per-turn 플래그 `m_buildRequestedThisTurn` (bool) 을 추가한다.
- 새 사용자 메시지 수신 시 플래그를 리셋한다.

### 3. MainWindow.cpp — Gemini 상태 메시지 routing 개선 (선택)

Non-coordinator의 build 요청이 차단될 때 사용자에게는 표시하지 않는다.
내부 로그(debug)에만 남긴다. 채팅 버블로 출력하면 혼란을 준다.

## 검증

1. Release x64 빌드 성공 (0 error).
2. Codex provider로 `orange.build` 요청 시 거부 로그 확인.
3. Claude provider로 `orange.build` 요청 시 정상 실행 확인.
4. 같은 턴에 build 중복 요청 시 두 번째 요청이 무시되는지 확인.

## 결과 보고 위치

`reports/council_execution_guard.md`

## 4단계 판단 기준

1. **유효성** — coordinator 체크가 정확한 provider 비교를 하는가?
2. **안전성** — coordinator가 바뀌는 시점(새 세션, provider 교체)에 guard가 올바르게 동작하는가?
3. **다른 기능과의 연관** — `orange.capture`, `orange.delegate` 등 다른 도구에도 동일한 guard가 필요한가?
4. **최악의 경우** — guard 버그로 정팀장도 build를 못 하게 되는 경우를 막는 안전장치는?
