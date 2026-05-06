# task: gemini_model_upgrade

> 3자회동(2026-05-06) 에서 드러난 Gemini 참여 불가 문제를 해결한다.
> gemini-3-flash-preview quota 고갈 → 모델 교체 + 429 graceful fallback.

## 목적

Gemini가 `gemini-3-flash-preview` quota 초과(429)로 세션 전체에서 실질 발언 0건이었다.
3자 합의 체계가 사실상 Claude + Codex 2자로 운영되었으므로, 안정적인 모델로 교체하고
429 발생 시 무한 재시도 대신 graceful fallback 처리가 필요하다.

## 범위

### 1. GeminiPrintBackend.cpp — 모델명 교체

현재: `gemini-3-flash-preview`
변경: `gemini-2.5-flash` (stable alias 우선; 없으면 `gemini-2.5-flash-preview-05-20`)

`m_model` 초기값 또는 하드코딩된 모델 문자열을 교체한다.
변경 전 정확한 위치를 파일에서 확인한 뒤 최소 1줄만 수정한다.

### 2. GeminiPrintBackend.cpp — 429 감지 후 재시도 제한

현재: 내부에서 무한 재시도 루프.
변경:
- 429 응답 감지 시 재시도 횟수를 카운트한다.
- 재시도 3회 초과 시 ErrorCallback으로 "Gemini 용량 초과, 이번 턴 건너뜀" 메시지를 전달하고 중단한다.
- 재시도 사이 대기는 최대 10초로 제한한다.

### 3. OrangeView.h 또는 MainWindow.cpp — Gemini 429 상태 표시 개선

현재: "[Gemini 상태] 모델 capacity 429를 받아 Gemini CLI가 내부 재시도 중입니다. ..." 형태의 긴 메시지가 채팅에 그대로 출력됨.
변경: 상태 버블 role="gemini-status"로 분류하여 짧은 한 줄 표시:
"Gemini: 용량 초과, 대기 중..."
반복 표시 시 이전 상태 버블을 in-place 갱신하여 채팅이 누적되지 않도록 한다.

## 검증

1. Release x64 빌드 성공 (0 error).
2. `--test-backend gemini` 또는 mock으로 모델명이 새 값으로 전송되는지 확인.
3. 429 시뮬레이션(응답 stub) 후 3회 초과 시 에러 콜백이 호출되는지 확인.
4. 채팅 UI 캡처로 Gemini 상태 버블이 짧고 중복 없이 표시되는지 확인.

## 결과 보고 위치

`reports/gemini_model_upgrade.md`

## 4단계 판단 기준

1. **유효성** — 빌드 성공, 모델명 교체 확인.
2. **안전성 검토** — 새 모델이 기존 prompt 포맷과 호환되는가? stream 방식 차이 있는가?
3. **다른 기능과의 연관** — BackendManager의 provider 선택 로직에 영향 없는가?
4. **최악의 경우** — 새 모델도 quota 초과 시 fallback이 정상 동작하는가?
