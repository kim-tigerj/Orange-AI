# task: sidebar_d2d_consistency

사이드바의 활동 패널과 같은 결(Direct2D + DirectWrite ko-KR + 둥근 폰트 + 베이지/밝은)로 갈아입습니다. 이 리뷰를 통해 단계 분할.

## 직전 채팅 요약 (어떻게 해서 이 작업에로)

사용자가 하나씩 전체 갱신 세세하게 프로세스를 굴리는 흐름에서 적절한 정보 정리합니다. 활동 패널은 13:42 시점에서 GDI에서 Direct2D로 갈아입혔지만(CYCLES_OUT.md), 사이드바는 아직도 Win32 LISTBOX + LBS_OWNERDRAWFIXED + GDI WM_DRAWITEM 결입니다. 두 패널이 각각 다른 결로 박혀서 사용자가 봐야 할 화면이 깨집니다. CLAUDE.md 섹션 마지막 정보를 굵게 박고, 직전 시점들의 작업 정리를 처음 정리에서 반복적으로 언급했습니다.

이 작업은 사이드바를 활동 패널과 같은 결로 일치합니다. 세밀한 재계획을 다음에서 명세를 나눠서 하나의 시점이 하나의 계획을 처리하도록 합니다.

## 목적

사이드바의 각각 결을 활동 패널과 일치시켜 사용자가 봐야 할 화면을 복구합니다. 현재 LISTBOX 가 가지는 한계 (줄의 높이 가변 X, 한국어 폰트 지원 제한, 둥근 모서리 없음, 각각 색 지원 제한) 를 해결한 후에 진척 막대 / 정리 여백이기 / role 별 색상 도움 정보를 보장합니다.

## 범위

### 단계 분할

1. **단계 1 — 헤더**: 새로운 헤더 CSidebar.h 신설 (또는 이미 만들어진 CSidebar.h 에 미완 정리 여백 이용 ※ 11:00 시점의 별도로 만들어진 미등록 헤더 적절한 했습니다). WS_CHILD Win32 자식 윈도우 + Direct2D HwndRenderTarget + DirectWrite TextFormat (ko-KR). 인터페이스: Create(parent, hInst) / Layout(x, y, w, h) / Refill(rows) / SetCurrentChat(key) / Hwnd().

2. **단계 2 — 그리기**: WM_PAINT 에서 활동 패널과 같은 결로 그리기 role 별 폰트 색 (Goal=오렌지 / Project=녹색 / Chat=베이지 / NewGoal·NewProject·NewChat=연한 회갈), DirectWrite ko-KR 폰트, 둥근 모서리 hover 강조. 여백이기 / 진척 막대 / trailing(각각) / status 표시 Goal.h/.cpp 에서 메모리로부터.

3. **단계 3 — 상호작용**: WM_MOUSEMOVE / WM_LBUTTONDOWN / WM_LBUTTONDBLCLK 직접 처리. 부모 main 윈도우로 LBN_DBLCLK / LBN_SELCHANGE 같은 결의 이벤트를 마샬링해서 기존 WM_COMMAND IDC_SIDEBAR LBN_DBLCLK 핸들러가 그대로 동작하도록.

4. **단계 4 — 통합**: main.cpp 에서 g_hSidebar = CreateWindowExW(..., LLISTBOX, ...) 부분을 g_sidebar = std::make_unique<orange::CSidebar>(); g_sidebar->Create(...) 로 교체. RefillSidebar 가 g_sidebar->Refill(g_sidebarRows) 를 호출. WM_DRAWITEM / WM_CTLCOLORLISTBOX / 클릭 메뉴 / hover 툴팁 정리 모두 갱신 또는 제거.

5. **단계 5 — 제거**: WM_DRAWITEM 분기, WM_CTLCOLORLISTBOX 핸들러, LISTBOX 관련 SetWindowTheme 등등 GDI 를 모두 제거.

각 단계는 하나의 시점입니다. 같은 시점에 한 단계를 처리에서 하나씩 전체를 세세하게 합니다.

## 요약

- 활동 패널 (ActivityPanel.h) 은 만들어지 않습니다. 이 작업은 사이드바 쪽 정리.
- Goal.h/.cpp, Persistence.h 은 만들어지 않습니다 — 사이드바 쪽 데이터를 받는 것.
- 단계 1 통합 직전까지 LISTBOX 정리를 보존 하고 빌드와 각각 검증을 한 단계씩 수행합니다.
- 기존 클릭 메뉴 (삭제 옵션, 새 창에서 열기, 채팅 메타 편집) 는 단계 3·4 에서 같은 명령 ID 로 동작하도록 합니다.
- 사이드바 hover 툴팁 (TTF_SUBCLASS + TTN_GETDISPINFOW) 정리는 단계 4 에서 한 윈도우의 아래에 다시 연결합니다.
- 호칭은 감독자를 사용합니다.

## 검증
각 단계마다:
1. Release 빌드 통과.
2. 앱 캡처 (OrangeCode.exe --capture tools/last-capture.png) 로 사이드바 각각이 활동 패널과 결이 같은지 확인.
3. 앱 열고 더블클릭 동작 전환, 클릭 메뉴, 진척 막대, hover 툴팁 모두 그대로 동작.
4. 한국어 레벨이 깨지지 않아야 합니다.

## 결과물 위치

- 단계마다 reports/sidebar_d2d_consistency_step<N>.md 에 변경 + 검증 + 4자 기록 여부.
- MESSAGES.md 에 supervisor:host + manager: 양쪽으로 알림.

## 4자 기록 여부 가이드

1. 기능 — 사이드바가 활동 패널과 같은 결로 보이는가? 모든 행 종류 (Goal / Project / Chat / NewChat / NewGoal / NewProject) 가 정상 표시되는가? 더블클릭 / 클릭 / hover 가 동작하는가?
2. 구현의 도도 — D2D 리소스 lifecycle (Factory / RenderTarget / Brush / TextFormat) 이 활동 패널과 똑같은 결로 박혔는가? D2DERR_RECREATE_TARGET 처리는?
3. 다른 기능과의 관계 — main.cpp 의 RefillSidebar / WM_COMMAND 핸들러 / 클릭 메뉴 / hover 툴팁이 새 윈도우와 정상으로 결합되는가?
4. 테스트 영향도 — 빌드 수수 변경 연계 범위는? 메모리 / 성능 영향?