# task: auto_retry_rate_limit

사용자 호출 (14:14): API Error: Server is temporarily limiting requests (not your usage limit) — Rate limited 시 전체 1분 후 자동 retry 하는 것.
사용자 결정 (14:15): 고정말고 조건에 맞는 정해진 경우에만 retry — 백오프 + 무한.

## 직전 채팅 요약

사용자가 작업 중에 Anthropic API 서버 측 rate limit 에러를 받았습니다 (사용자 도가 아니라 서버가 일시적으로 제한하는 경우). 매번 사용자가 직접 같은 prompt 를 다시 보내야 한다면 반복 × 사람 시간 × LLM 시간 비전이 깨집니다. 자동 retry 가 연결되어야 합니다.

합의는 백오프 + 무한 retry 세세하게 이고 사용자가 해결을 받았습니다. 단 정해진 경우에만 — rate limit / overloaded 같은 일시적 서버 측 에러에만 retry 하고, 인증 실패·명세 오류 같은 영구 에러는 그대로 사용자에게 보입니다.

## 목적

claude CLI 의 result 메시지에 일시적 서버 제한 패턴이 보이면 자동으로 retry 합니다. 백오프는 1분 → 2분 → 5분 → 10분 → 10분 반복. 사용자가 ESC 또는 카드 클릭으로 취소할 때까지 무한.

## 범위

### 1. ClaudePrintBackend.h — 패턴 감지 + 콜백

기존에 박혀 있는  감지 정리 (CYCLES.md 11:00) 결을 따라 rate limit 패턴도 감지합니다.

감지 패턴 (case-insensitive, substring):
- Server is temporarily limiting requests
- temporarily limiting
- Rate limit
- 429 Too Many Requests
- Overloaded

매칭하면 콜백 RetryRequestedCallback(promptUtf8) 호출. 콜백의 호출자(main.cpp)가 retry 세세하게를 설정합니다. 콜백 인터페이스는 IBackend::Start 또는 같은 setter 로 박습니다.

기존 ErrorCallback 은 그대로 — 영구 에러는 ErrorCallback 으로, 일시적 에러는 RetryRequestedCallback 으로 (분기 X). main 에서 분기 X — 백엔드가 어느 쪽인지 결정합니다.

### 2. main.cpp — retry 카드 + 타이머 + 자동 발화

추가 글로벌:
- g_retryPrompt (std::wstring) — 발화할 prompt.
- g_retryAttempt (int) — 백오프 단계 (0=첫, 1=두 번째 ...).
- kRetryTimerId — timer ID.

RetryRequestedCallback 핸들러 (WM_RETRY_REQUEST 마샬링):
- prompt 박고 g_retryAttempt 증가.
- 백오프 시각 결정 — int delays[5] = {60, 120, 300, 600, 600}. 인덱스 = min(attempt, 4).
- OrangeView 에 role=retry 블록 박기 (예: Rate limit — 60초 후 재시도(1회)).
- SetTimer(kRetryTimerId, 1초) 로 매초 카운트다운 갱신 (블록 텍스트 in-place 갱신 + Invalidate).

0초 도달:
- KillTimer.
- 블록 텍스트를 재시도 중 으로 갱신.
- DispatchPrompt(g_retryPrompt) 지연 호출 하여 turn 시작.

ESC 또는 카드 클릭 시 즉시 취소:
- KillTimer.
- 블록 텍스트를 재시도 취소 로 갱신.
- g_retryPrompt 비우기.

turn 성공 (정상 result) 시 g_retryAttempt = 0 으로 reset (다음 rate limit 발생 시 처음부터).

### 3. role=retry 블록 각각

기존 role=error / tool 결을 따라 dim 색 + 이탤릭 폰트. OrangeView 에 role 분기에 추가 하여 m_retryBgBrush 또는 기존 dim brush 사용.

세세함이 단순 — 시각 테스트가 충분하면 같은 brush 신설 없이 role=tool 로 그대로 (dim 회색).

### 4. 빌드

Release 통과.

## 요약

- claude CLI 의 영구 에러는 그대로 사용자에게 보입니다 (auth 실패, prompt 오류 등). retry 자체가 서버 측 일시 제한 한정.
- 패턴 매칭은 case-insensitive 결로 가벼운 substring 검색.
- 백오프는 같은 프로세스의 현재 turn 이름 변경 — 자기-교체 시 g_retryAttempt 가 0 으로 reset 합니다 (정상 상태).
- 호칭은 감독자를 사용합니다.

## 검증
1. 빌드 통과.
2. 사용자가 처음 turn 에서 rate limit 받으면 Rate limit — 60초 후 재시도(1회) 카드가 자동 박힘.
3. 카운트다운이 매초 줄어드는지.
4. 0초 도달 시 자동으로 같은 prompt 발화됨.
5. 두 번째 rate limit 받으면 120초 후 재시도(2회) 카드.
6. ESC 또는 카드 클릭 시 즉시 취소 + 재시도 취소 카드.
7. retry 이후 정상 답변 받으면 g_retryAttempt 가 0 으로 reset (다음 rate limit 시 1차부터).

## 결과물 위치

- reports/auto_retry_rate_limit.md 에 변경 + 검증 + 4자 기록 여부.
- MESSAGES.md 에 양쪽 알림.

## 4자 기록 여부 가이드

1. 기능 — rate limit 시 retry, 다른 에러는 그대로 사용자에게 보이는가? 백오프가 1·2·5·10·10 으로 진행되는가? ESC 취소 동작?
2. 구현의 도도 — 패턴 매칭이 깔끔한가? 카운트다운 갱신이 깜빡이지 않는가? 백엔드와 main 간의 분리가 자연스러운가?
3. 다른 기능과의 관계 — 기존 ErrorCallback 흐름이 깨지지 않는가? turn 진행 중 입력 또는 결과 충돌 X?
4. 테스트 영향도 — 무한 retry 가 원하지 않는 위험은 없는가? (사용자 ESC 만으로 멈출 수 있고, 자기-교체 시 자연 reset)