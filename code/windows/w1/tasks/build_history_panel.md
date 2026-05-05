# build_history_panel ? 활동 패널 위 *빌드 이력* 헤더

## 직전 채팅 요약 (정팀장의 번역)

사용자가 12:09 한 줄로 박았습니다 ? *"오른편 활동 위에 우리 앱 자체 빌드 재실행된 횟수를 넣으면 좋겠어. 최초 시작 빌드일, 최종 빌드일도 보여주고."*

직전 흐름의 결:
- 11:21 사이클에서 메타 편집 한 묶음 dialog 가 박혔고, 12:08 빌드 + 자기-교체로 새 인스턴스가 떴습니다.
- 12:09 사용자 명령은 `USER_COMMANDS.md` 에 pending 으로 박혀 있습니다.
- OR-1305 비전의 *반복 × 시간* 공식이 화면 자체에 박혀야 한다는 자세입니다 ? 사용자가 본 앱이 *얼마나 자주 굴러갔는지* 를 매 spawn 마다 한눈에 보면 자기-격발 루프의 누적이 시각적으로 인지됩니다.
- 활동 패널은 이미 Direct2D 결로 갈아엎힌 상태(`ActivityPanel.h`, 13:42 사이클)입니다. 같은 결로 박아야 일관성이 깨지지 않습니다.

## 목적

활동 패널 (`ActivityPanel.h`) 의 상단에 *빌드 이력* 헤더 영역을 신설합니다. 본 OrangeCode.exe 가 빌드되어 새로 떠오른 누적 횟수와, 최초 빌드 시각, 최근 빌드 시각 세 정보를 사용자가 한눈에 보이도록 합니다.

*재실행* 의 정의는 "새로 빌드된 exe 가 처음 실행된 때" 입니다 ? 같은 exe 의 단순 재실행은 카운트가 올라가지 않습니다 (사용자 의도: 빌드 자체의 누적). 판정 기준은 본 exe 파일의 LastWriteTime 입니다 ? 디스크에 기록된 마지막 빌드 시각과 다르면 새 빌드로 간주합니다.

## 범위

본 작업이 만질 파일은 다음 세 곳입니다. 다른 파일은 만지지 않습니다.

### 1. `BuildHistory.h` 신설 (header-only)

영속 빌드 카운터의 read/write 책임. 저장 위치 ? `%APPDATA%\OrangeCode\build_history.json`.

JSON 형식 (예시):

```json
{
  "version": 1,
  "count": 17,
  "first_build_iso": "2026-04-30T22:05:13Z",
  "last_build_iso": "2026-05-01T12:08:42Z"
}
```

API:

```cpp
namespace orange {
struct BuildHistoryRecord {
    int          count = 0;
    std::wstring firstBuildIso;
    std::wstring lastBuildIso;
};

class CBuildHistory {
public:
    // 본 exe 의 LastWriteTime 을 읽고 디스크 기록과 비교.
    // 다르면 count++, last 갱신 (first 비어있으면 같이 설정), 디스크 atomic write.
    // 같으면 read-only (단순 재실행 ? 카운트 변동 0).
    // 어느 경우든 최종 record 를 반환.
    static BuildHistoryRecord Tick();

    // 디스크 read 만 ? 시각 표시용.
    static BuildHistoryRecord Load();
};
}
```

내부 구현:
- `GetModuleFileNameW(nullptr, ...)` 로 본 exe 경로 얻기.
- `GetFileAttributesExW` 또는 `CreateFileW + GetFileTime` 으로 LastWriteTime 얻기.
- `FileTimeToSystemTime` + `SystemTime → ISO 8601 UTC 문자열` 포맷.
- `%APPDATA%\OrangeCode\build_history.json` 경로는 `Persistence.h` 의 기존 AppData 헬퍼 결과 일관성 유지 (가능하면 같은 헬퍼 재사용 ? 없으면 단순 `SHGetFolderPathW(CSIDL_APPDATA)` 직접).
- 영속화는 `Persistence::Save` 가 쓰는 atomic 결 (`.tmp` → `MoveFileExW`) 패턴 그대로.
- JSON 은 jsoncpp 사용 (이미 vendored).

### 2. `ActivityPanel.h` 상단 *빌드 이력 헤더* 영역 추가

기존 `kHeaderHeight` (현재 헤더 *활 동* 높이) 위에 새 영역을 넣습니다. 두 결 중 자연 자리는 *맨 위에 빌드 헤더 + 1px 라인 + 기존 활동 헤더* 입니다.

레이아웃 (제안값 ? 자연스러우면 조정 가능):
- 빌드 헤더 영역 높이: 약 52px.
- 좌상단 라벨 (작은 dim 결): *빌드*
- 본문 1행 (큰 결, 본문 베이지 톤): `17회 재빌드` (또는 0/1 일 때 단/복수 자세 ? 한국어 *재빌드* 는 단/복수 같음).
- 본문 2행 (작은 dim 결): `최초 04-30 22:05 · 최근 05-01 12:08`
  - 시각 포맷은 `MM-DD HH:MM` 약식 (한 줄에 들어가도록). 연도가 다르면 `YYYY-MM-DD` 까지.
  - count 가 0/1 면 `최초` 와 `최근` 이 같은 시각이라 `최근` 만 표기 가능.

D2D/DWrite 결:
- 본 패널의 `m_tfBody` / `m_tfDim` / `m_tfHeader` / `m_textBrush` / `m_dimBrush` 재사용.
- 헤더 폰트와 본 헤더 *활 동* 폰트는 같은 결.
- 영역 끝 1px 라인 (옅은 베이지) 으로 활동 영역과 분리.

데이터 흐름:
- `Create` 시 `CBuildHistory::Tick()` 한 번 호출 → 멤버에 record 저장.
- `Poll` 시 `Load()` 로 갱신 (자기-교체 직후 새 인스턴스가 같은 패널을 못 보지만, 안전상 매 폴링 시 read-only 갱신 ? 무거운 작업은 아님).
- `OnPaint` 의 맨 앞에 빌드 헤더 영역 그리는 분기 추가.

기존 행 그리기 자리 (actor 행) 의 시작 Y 좌표가 `kBuildHeaderHeight + kHeaderHeight` 로 밀려야 합니다 ? `m_rowHits` 좌표도 같이 재계산.

### 3. main.cpp ? 통합 자리 0 (호출 경유 X)

`g_activityPanel->Create(...)` 호출은 그대로. `Tick()` 은 `ActivityPanel.h::Create` 안에서 자체 호출하므로 main.cpp 손대지 않습니다.

## 검증

### 기능 (1축)
1. `tools/build.sh` 통과 (Release 빌드).
2. 자기-교체 spawn 후 `OrangeCode.exe --capture tools/last-capture.png` 로 패널 상단에 *빌드 17회 재빌드* (또는 현재 누적값) + 최초/최근 두 줄이 보여야 합니다.
3. `%APPDATA%\OrangeCode\build_history.json` 파일이 존재하고 `count >= 1`, `first_build_iso` / `last_build_iso` 가 ISO 8601 UTC 형식.
4. 같은 exe 를 한 번 더 실행 → count 변동 없음, last_build_iso 변동 없음.
5. 새 빌드 후 실행 → count + 1, last_build_iso 갱신, first_build_iso 보존.

### 구현의 정도 (2축)
- atomic write 패턴 (`.tmp → MoveFileExW`) 지킴.
- JSON 파싱/직렬화는 jsoncpp 사용 (헤더는 `<json/json.h>`).
- v1 형식 미존재 (파일 없음) 시 `count=0` 부터 시작 ? 첫 Tick 에서 1.
- D2D 자원 (브러시·텍스트 포맷) 는 기존 패널 자원 재사용 ? 새로 만들지 말 것.
- UTF-8 BOM 모든 신규 `.h` 파일에 박을 것.

### 다른 기능과의 관계 (3축)
- 활동 패널의 actor 행 그리기 좌표가 빌드 헤더 영역만큼 아래로 밀려야 함. tooltip hit-test 좌표도 같이 재계산.
- `Coordination.h` / `Persistence.h` / `OrangeView.h` 등 다른 자리는 손대지 말 것.

### 시스템 영향도 (4축)
- 디스크 IO 는 시작 시 한 번 (`Tick`) + 폴링 시 1.5초 간격 read 한 번. 부담 0 에 가까움.
- exe 의 LastWriteTime 은 OS 가 항상 갱신하므로 별도 빌드 훅 필요 없음.
- `/MT` 정적 링킹 영향 없음 ? `<windows.h>` + `<shlobj.h>` 표준.

## 결과 보고 자리

작업 완료 시 다음을 박습니다:

1. `reports/build_history_panel.md` ? 변경 자리 / 빌드 결과 / 자가 캡처 PNG 경로 / 4축 자기 평가.
2. `MESSAGES.md` 두 자리 ? `supervisor:host` 와 `manager:62932` (parent_pid 환경변수) 양쪽으로 *[팀원 결과] build_history_panel ? 완료* 메시지.
3. 본 인스턴스 자연 종료.

상태는 `review_pending` 자세로 박고, 4축 통과 판정은 정팀장 또는 오감독이 수행합니다.

## 자세 메모

- 본 작업은 시각 작업입니다 ? *근본 자세* (사용자 04:21 결) 와 일관, 카드 결과 호흡감을 같이 챙겨야 합니다.
- 폰트 크기·색·간격은 본 패널의 *헤더 활 동* 결과 본문 actor 행 결의 *중간* 자리로 자연스럽게.
- 한국어 압축 결 금지 ? 본 명세도 완성된 한국어 문장으로 적었습니다. 코드 코멘트도 같은 결.
