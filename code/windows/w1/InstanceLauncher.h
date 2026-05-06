#pragma once

// InstanceLauncher — 자식 Code.exe spawn / 옛 인스턴스 정리 / sessionId 검증을 한 자리에.
//
// 직전까지 main.cpp 안에 흩어져 있던 spawn 책임을 모아둠. 멀티 인스턴스, 사이드바 더블클릭,
// --replace-pid 처리 등이 모두 여기를 거친다 — spawn 정책 변경 시 한 자리만 손보면 됨.
//
// 정적 메서드 전용 클래스 (상태 없음). 추후 spawn 정책(쿨다운·정원 제한 등) 가 생기면 인스턴스화.

#include <windows.h>
#include <string>

namespace orange {

class CInstanceLauncher {
public:
    // claude CLI 의 --resume 은 UUID 또는 *기존* 세션 제목만 받는다. UUID 형식이 아니면
    // demo/임시 키 — --resume 걸면 CLI 가 거부하고 멎는다. 사고 방지용 검증.
    // (UUID v4 형식: 36자, "8-4-4-4-12" 패턴, 16진수 + dash)
    static bool IsLikelyUuid(const std::string& s);

    // 자식 OrangeCode.exe spawn 옵션. 비어있는 필드는 명령행 인자 안 붙음.
    struct SpawnOptions {
        std::wstring sessionKey;        // --session <key>
        DWORD        replacePid = 0;    // --replace-pid <pid>
        std::wstring bootstrap;         // --bootstrap "<msg>"
        std::wstring goalId;            // --goal-id <id>     (multi-target 채팅의 부모 목표)
        std::wstring projectId;         // --project-id <id>  (multi-target 채팅의 부모 프로젝트)
    };

    // 현재 모듈(OrangeCode.exe) 경로로 새 자식 spawn. ShellExecute "open".
    // 성공 시 true. (자식 PID 는 반환 안 함 — ShellExecute 가 그대로는 안 줌)
    static bool Spawn(const SpawnOptions& opts);

    // 팀원 인스턴스 spawn 옵션 — 정팀장이 작업 분배할 때.
    struct SpawnMemberOptions {
        std::wstring taskId;        // --task-id <id> (필수, 예: "task_1730454321")
        std::wstring taskSpecPath;  // --task-spec <path> (tasks/<id>.md 경로)
        std::wstring sessionKey;    // --session <key> (없으면 새 unique 키)
        // bootstrap 은 자동 — "tasks/<task_id>.md 읽고 작업 수행 후 reports/<task_id>.md 박아라"
    };

    // 팀원 자식 spawn — `--role member` + ORANGE_CODE_PARENT_PID 환경변수 set + bootstrap.
    // 부모(정팀장) PID 가 자식 env 로 인계됨 — 자식이 자기가 누구의 팀원인지 인지.
    // 자식의 actor entry 는 `member:<pid>` 로 자동 (Coordination).
    //
    // 명세 강제: `taskSpecPath` 가 가리키는 파일이 `## 목적|배경` · `## 범위` · `## 검증`
    // 세 섹션을 모두 포함하고 본문이 200자 이상일 때만 spawn 합니다. 미달이면 false 반환 +
    // outErrorMessage 에 한국어 사유를 채웁니다 (NULL 이면 사유 무시).
    // 정팀장의 *번역 역할* (CLAUDE.md §2) 을 시스템에서 강제하는 장치입니다.
    static bool SpawnMember(const SpawnMemberOptions& opts,
                            std::wstring* outErrorMessage = nullptr);

    // 오감독 자식 spawn 옵션 — 정팀장이 호스트 오감독 응답 없을 때 새 오감독 임명.
    // 사용자 박음 — *"오감독은 유일한 존재가 아니라 누군가 오감독 역할 수행"*.
    struct SpawnSupervisorOptions {
        std::wstring reason;        // 왜 spawn 하는지 (호스트 오감독 timeout / 평행 리뷰 등)
        std::wstring sessionKey;    // --session <key> (없으면 unique 키)
        std::wstring contextHint;   // 어떤 작업의 4축 종합 평가를 할지 (선택)
    };

    // 오감독 인스턴스 spawn — `--role supervisor` + actor_id `supervisor:<pid>`.
    // bootstrap 으로 *오감독 자세* (4축 종합 + 시스템 복원) 명시. 정팀장이 LLM 으로 호출.
    static bool SpawnSupervisor(const SpawnSupervisorOptions& opts);

    // 옛 인스턴스(PID) 정리: 메인 윈도우(주어진 클래스명) 찾아 WM_CLOSE 보내고 최대 5초 종료 대기.
    // wWinMain 의 --replace-pid 처리에서 호출. self-PID 또는 0 이면 no-op.
    static void ReplaceOldInstance(DWORD oldPid, const wchar_t* windowClass);

    // 떠있는 *모든 다른* OrangeCode.exe 윈도우 인스턴스 일괄 정리 — selfPid 만 보존.
    // 사용자 박음 (2026-05-01) — *자체 업데이트할 때는 기존 걸 다 내려야지*. 자기-교체 사이클이
    // 부모 한 명만 정리하던 흐름에서 옛 좀비가 누적되던 사고 회복.
    // 동작: 같은 윈도우 클래스의 모든 윈도우 enum → 동시 WM_CLOSE → 합쳐 5초 대기 → 안 죽은
    // 프로세스만 TerminateProcess fallback. 좀비 N 개여도 5초 + 짧은 강제 종료 한 번으로 끝.
    static void CleanupOtherInstances(DWORD selfPid, const wchar_t* windowClass);

    // 충돌 없는 임시 세션 키 ("new_<ms>") — 새 세션 spawn 시 사용.
    // claude CLI 가 첫 응답으로 진짜 sessionId 발급하면 SaveSession 이 그 키로 마이그레이션.
    static std::wstring NewUniqueSessionKey();

    // 현재 모듈(Code.exe) 절대 경로. 실패 시 빈 문자열.
    static std::wstring SelfModulePath();

private:
    CInstanceLauncher() = delete;
};

}  // namespace orange
