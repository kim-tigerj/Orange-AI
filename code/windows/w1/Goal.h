#pragma once

// CGoal / CProject — 작업 위계의 상위 두 층.
//
// 위계: 목표(Goal) → 프로젝트(Project) → 채팅(Chat).
//   - 목표: 비전·출시 단위 (예: "Orange Code 1.0 출시"). 그 안에 프로젝트 N개.
//   - 프로젝트: 목표를 이루는 작업 묶음 (예: "사이드바 디자인"). 그 안에 채팅 N개.
//   - 채팅: 정팀장 한 세션의 turn 흐름. (기존 session_*.json 의 자리)
//
// 각 층마다 *공유 컨텍스트* (`context.md`) 보유 — 그 층 안 모든 하위 단위가 spawn 시 읽음.
// 멀티 인스턴스 운용: 한 프로젝트를 여러 정팀장 인스턴스가 분담, 같은 컨텍스트 위에서 협업.
//
// 이번 사이클 — 디렉토리 보장 + 메타 read/write + 평면 → 위계 마이그레이션. List/UI 는 다음.

#include <windows.h>
#include <string>
#include <vector>

namespace orange {

// 작업 항목의 *진행 상태* — 사이드바 점 색의 토대.
// planning(회색) / in_progress(오렌지) / blocked(빨강) / paused(노랑) / done(초록) / abandoned(흐림)
enum class CWorkStatus {
    Planning = 0,
    InProgress,
    Blocked,
    Paused,
    Done,
    Abandoned,
};

const char* WorkStatusToString(CWorkStatus s);
CWorkStatus  WorkStatusFromString(const std::string& s);

// 평가 판정 — 평가 기준(criteria) 대비 현재 위치.
// none(평가 전) / pass(통과) / partial(부분) / weak(미흡) / fail(실패)
enum class CVerdict {
    None = 0,
    Pass,
    Partial,
    Weak,
    Fail,
};

const char* VerdictToString(CVerdict v);
CVerdict     VerdictFromString(const std::string& s);

struct CGoal {
    std::string id;             // 슬러그 (영숫자·-·_) — 파일명 안전. "default" 가 첫 마이그레이션 자리.
    std::string title;          // 사람 친화 제목 ("기본 목표" 등)
    std::string createdAtIso;
    std::string lastActiveIso;

    // ── 진척 대시보드 메타 (OR-1317) ──
    std::string purpose;        // 왜 시작했나, 무엇을 이루려 하나 (자유 텍스트)
    std::string criteria;       // 무엇이 충족되면 *성공* 인가 (체크리스트 또는 정성)
    CVerdict    verdict      = CVerdict::None;     // 평가 기준 대비 현재 위치
    int         progress     = 0;                  // 0~100 (자동 신호 + 수동)
    CWorkStatus status       = CWorkStatus::Planning;
    std::string lastEvaluatedIso;                  // 마지막 verdict 갱신 시각

    // 첫 마이그레이션·신규 채팅 기본 자리.
    static const char* kDefaultId;     // "default"
    static const char* kDefaultTitle;  // "기본 목표"

    // 디렉토리: %APPDATA%/OrangeCode/goals/<id>
    static std::wstring Dir(const std::string& id);
    // 그 안 meta.json — 제목·timestamp 등.
    static std::wstring MetaPath(const std::string& id);
    // 공유 컨텍스트 (목표 안 모든 프로젝트·채팅이 spawn 시 읽음).
    static std::wstring ContextPath(const std::string& id);

    // 디렉토리 + meta.json 보장. 없으면 생성. 있으면 그대로.
    static bool EnsureExists(const std::string& id, const std::string& title);

    // meta.json read/write.
    static bool  Save(const CGoal& g);
    static CGoal Load(const std::string& id);

    // id 슬러그 안전화 (영숫자·-·_·.).
    static std::string SanitizeId(const std::string& id);
};

struct CProject {
    std::string id;
    std::string goalId;
    std::string title;
    std::string createdAtIso;
    std::string lastActiveIso;

    // ── 진척 대시보드 메타 (OR-1317) — Goal 과 같은 구조 ──
    std::string purpose;
    std::string criteria;
    CVerdict    verdict      = CVerdict::None;
    int         progress     = 0;
    CWorkStatus status       = CWorkStatus::Planning;
    std::string lastEvaluatedIso;

    static const char* kDefaultId;     // "default"
    static const char* kDefaultTitle;  // "기본 프로젝트"

    // 디렉토리: %APPDATA%/OrangeCode/goals/<goalId>/projects/<id>
    static std::wstring Dir(const std::string& goalId, const std::string& id);
    static std::wstring MetaPath(const std::string& goalId, const std::string& id);
    static std::wstring ContextPath(const std::string& goalId, const std::string& id);
    // 그 안 chats\ — 채팅 파일들 (chat_<chatId>.json) 자리.
    static std::wstring ChatsDir(const std::string& goalId, const std::string& id);

    static bool EnsureExists(const std::string& goalId, const std::string& id,
                             const std::string& title);
    static bool     Save(const CProject& p);
    static CProject Load(const std::string& goalId, const std::string& id);
};

}  // namespace orange
