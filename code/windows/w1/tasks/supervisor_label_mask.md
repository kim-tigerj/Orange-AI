# task: supervisor_label_mask

> 대표님 지시(3자회동 2026-05-06): "오감독은 숨겨진 우리 앱의 감독자야. 드러내면 안 됨."
> 사용자 화면에 노출되는 "오감독" 텍스트 3곳을 중립 명칭으로 교체한다.

## 목적

오감독(Oh-Council supervisor)은 내부 운영 체계 개념이다.
사용자 화면에 이 명칭이 노출되면 내부 페르소나가 외부에 드러나 일관성이 무너진다.
소스 조사 완료 결과(seq 84, Claude 발언), 노출 위치는 아래 3곳이다.

## 범위

### 노출 위치 및 변경 내용

| 파일 | 라인(참고) | 현재 텍스트 | 변경 텍스트 |
|------|-----------|------------|------------|
| `ActivityPanel.h` | 466 | `"오감독"` (role 배지) | `"감독자"` 또는 제거 |
| `MainWindow.cpp` | 1094~1095 | `"오감독 각성"` 카드 렌더링 | `"시스템 초기화"` 또는 완전 제거 |
| `MainWindow.cpp` | 1224 | `AppendText("오감독 호출\n\n...")` | 호출 텍스트 제거 또는 내부 로그로 전환 |

각 위치를 파일에서 직접 확인한 뒤 최소 변경으로 처리한다.
내부 코드(BackendManager, GeminiPrintBackend, ManagerEnvironment 등)의 "오감독" 언급은
유지해도 된다 — 사용자 화면에 직접 렌더링되지 않기 때문이다.

### 처리 원칙

- 텍스트를 숨기는 것이 목적이므로 로직 변경은 최소화한다.
- role="supervisor" 배지 자체를 없애거나, 표시 텍스트만 중립화한다.
- "오감독 각성" 카드는 UI에서 완전 제거하는 것이 가장 깔끔하다 (내부 각성은 프롬프트에서만 처리).

## 검증

1. Release x64 빌드 성공 (0 error).
2. orange.capture로 채팅 화면 캡처 후 "오감독" 텍스트가 보이지 않는지 확인.
3. ActivityPanel에서 supervisor role 배지가 "오감독" 대신 변경된 텍스트 또는 빈 값으로 표시.
4. supervisor 관련 내부 로직(DB, prompt, Coordination)은 변경 없이 유지.

## 결과 보고 위치

`reports/supervisor_label_mask.md`

## 4단계 판단 기준

1. **유효성** — 세 위치 모두 변경되었는가?
2. **안전성** — ActivityPanel role 배지를 제거할 때 다른 role과 rendering 충돌 없는가?
3. **다른 기능과의 연관** — supervisor role 배지가 클릭 이벤트나 다른 UI와 연동되는가?
4. **최악의 경우** — 변경 후에도 reports/, tasks/ 등 저장 파일에 "오감독"이 남아 있는 경로는?
