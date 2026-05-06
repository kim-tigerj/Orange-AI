# OrangeCode module ownership

이 문서는 여러 세션이 동시에 정팀장을 수정할 때 충돌을 줄이기 위한 파일 책임 경계다.

## Chat screen lane

- `MainWindowChatSession.cpp`
  - 현재 채팅 선택, 새 대화 초안, 최근 블록 로드, 과거 대화 지연 로드
  - `ChatSession`을 `OrangeView` 블록으로 변환
  - 채팅 블록 저장, pending-response 복구, 현재 채팅 비우기
- `OrangeView.h`
  - 채팅 블록의 실제 그리기, 레이아웃, 스크롤, hit-test
  - 버블 모양, 진행 중 표시, 첨부 카드 렌더링
- `OrangeInput.h`
  - 입력창, provider 선택 UI, 붙여넣기/첨부/캡처 버튼
- `MainWindowAttachments.cpp`
  - 파일 선택, 드래그 앤 드롭, 클립보드 이미지 저장
  - 첨부 manifest 생성과 채팅 첨부 카드 추가

채팅 화면 전담 세션은 위 네 파일을 우선 소유한다. 백엔드 호출 정책이나 빌드 도구 실행을 같이 만지지 않는다.

## Coordination lane

- `MainWindowCoordination.cpp`
  - 작업 카드, 검증 카드, 회의 결과 카드
  - manager provider 표시
  - `orange.delegate` 처리
- `Delegation.cpp`, `Delegation.h`
  - 위임 요청 파싱, 위임 프롬프트, 위임 결과 카드 생성
- `ManagerEnvironment.cpp`
  - 각 백엔드에게 주는 정팀장 환경/도구 안내

## Local tools lane

- `MainWindowBuildCapture.cpp`
  - `/build`, `/capture`, `/verify`
  - 빌드 스레드, 캡처 저장, 성공 빌드 후 앱 재실행
- `Capture.cpp`, `Capture.h`
  - 실제 화면 캡처 구현

## Data lane

- `Persistence.*`
  - SQLite chat/goal/project 저장소
- `ChatSessionOps.*`
  - 특정 chat key에 블록을 저장하는 작은 공용 작업
- `AttachmentStore.*`
  - 첨부 원본/썸네일/manifest 저장 정책

## Main window shell

- `MainWindow.cpp`
  - Win32 메시지 루프와 컨트롤 배치
  - 사이드바 목록/메뉴
  - goal/project 생성
  - 아직 분리되지 않은 입력 dispatch와 tool/admin 처리

다음 분리 후보는 `MainWindowDispatch.cpp`와 `MainWindowTools.cpp`다.
