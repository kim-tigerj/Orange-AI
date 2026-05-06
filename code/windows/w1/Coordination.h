#pragma once

// CCoordination ? SQLite DB 기반의 다중 CLI 협업 및 계층적 상태 관리 메커니즘.
//
// 목표(Goal) -> 프로젝트(Project) -> 채팅(Chat) 위계 구조를 명확히 하고,
// 각 항목에 담당 오감독, 정팀장, Worker을 배정하여 실시간으로 협업 맥락을 공유합니다.
// 파일 기반의 불안정한 동기화를 SQLite 의 ACID 트랜잭션으로 대체하여 무결성을 보장합니다.

#include <windows.h>
#include <string>
#include <vector>
#include "Database.h"

namespace orange {

class CCoordination {
public:
    // --- 1. 계층 구조 (Workflow) 데이터 타입 ---
    
    enum class WorkflowType { Goal, Project, Chat };

    struct WorkflowEntry {
        WorkflowType type;
        std::string  key;            // 'goal:id', 'goal:id:proj:id', 'goal:id:proj:id:chat:key'
        std::string  parentKey;      // 부모 항목의 키
        std::string  title;
        std::string  supervisorId;   // 담당 오감독 ID
        std::string  managerId;        // 담당 정팀장 ID
        std::string  workerId;       // 담당 Worker ID (현재 작업자)
        std::string  status;         // 'planning', 'in_progress', 'review', 'done'
        int          progress = 0;   // 0~100
        std::string  startTime;      // ISO8601
        std::string  endTime;        // ISO8601
        std::string  lastActive;     // ISO8601
    };

    // --- 2. 실시간 활동 (ActorActivity) 데이터 타입 (하위 호환성 유지) ---

    struct ActorStatus {
        std::string  actorId;         // 'manager:pid', 'worker:pid' 등
        std::string  kind;            // 'manager' / 'supervisor' / 'worker' (호환성)
        DWORD        pid = 0;
        std::string  sessionId;       // manager 한정 (호환성)
        std::string  currentWorkflowKey; // 현재 집중하고 있는 목표/프로젝트/채팅 키
        std::string  currentIntent;   // 호환성 (intent 필드와 매핑)
        std::string  intent;          // 현재 수행 중인 구체적인 의도/설명
        
        // 진행 중 turn 정보 (호환성)
        bool         turnInProgress = false;
        std::string  turnPrompt;
        int          eventsCount = 0;
        std::vector<std::string> recentTools;
        std::vector<std::string> claimFiles;

        std::string  lastSeenIso;     // ISO8601 UTC (호환성)
        std::string  lastSeen;        // ISO8601
    };

    // --- 3. 4축 리뷰 (Review) 데이터 타입 ---

    struct ReviewEntry {
        std::string  workflowKey;
        std::string  reviewerId;
        std::string  functional;     // 'pass', 'partial', 'weak', 'fail'
        std::string  implementation;
        std::string  relation;
        std::string  impact;
        std::string  comment;
    };

    // --- 4. Workflow 관리 API ---

    static bool UpsertWorkflow(const WorkflowEntry& entry);
    static WorkflowEntry GetWorkflow(const std::string& key);
    static std::vector<WorkflowEntry> ListWorkflows(const std::string& parentKey = "");
    
    // 상태 및 진척도 업데이트 (간편 API)
    static bool UpdateWorkflowStatus(const std::string& key, const std::string& status, int progress);
    
    // 담당자 배정
    static bool AssignWorkflowRoles(const std::string& key, const std::string& supervisor, const std::string& manager, const std::string& worker);

    // --- 5. Actor 관리 API (기기 내 실시간 동기화) ---

    // 하위 호환성 API
    static bool SetStatus(const ActorStatus& s);
    static std::vector<ActorStatus> ListActors(int staleSeconds = 60);
    static bool RemoveActor(const std::string& actorId);
    static std::string GetActorIntent(const std::string& actorId);

    // 충돌 감지 (호환성)
    struct Conflict {
        std::string                 otherActorId;
        std::string                 otherKind;
        std::string                 otherIntent;
        std::vector<std::string>    sharedFiles;
        std::string                 lastSeenIso;
    };
    static std::vector<Conflict> DetectConflicts(const std::string& selfActorId, int staleSeconds = 300);

    // --- 6. 리뷰 및 메시지 API ---

    static bool SubmitReview(const ReviewEntry& review);
    static std::vector<ReviewEntry> GetReviews(const std::string& workflowKey);
    
    // 호환성: MESSAGES.md 에 기록
    static bool AppendMessage(const std::string& fromActorId, const std::string& toActorId, const std::string& title, const std::string& body);

    // Provider relay command parser. Plain provider name mentions are not relay commands.
    static bool TryParseProviderRelayCommand(const std::wstring& text, std::string& targetProvider);

    // --- 7. 신경망 및 레포 맵 API ---

    static bool LogOperation(const std::string& actorId, const std::string& workflowKey, const std::string& logType, const std::string& content);
    static bool IndexSymbol(const std::string& filePath, const std::string& symbolName, const std::string& kind, int lineStart, int lineEnd, const std::string& signature);

    // --- 8. 유틸리티 ---

    static void Initialize(const std::wstring& dbPath);
    static std::string TimestampNow();
    static std::string GetMyActorId();

private:
    CCoordination() = delete;
};

} // namespace orange
