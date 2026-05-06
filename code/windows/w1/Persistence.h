#pragma once

// Persistence — 대화 세션을 %APPDATA%\OrangeCode\session_<key>.json 에 저장/복원.
// 형식: { version, session_id, blocks: [ { role, text } ] }
//
// key 는 보통 claude CLI 가 발급한 sessionId (UUID). 첫 응답 전엔 명령행 sessionArg
// 또는 "default". sanitize 로 파일명 안전 문자만 통과.

#include <windows.h>
#include <algorithm>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <json/json.h>

#include "Utils.h"
#include "Goal.h"
#include "Coordination.h"
#include "AttachmentStore.h"

namespace orange {

struct ChatBlock {
    int seq = -1;
    std::wstring role;   // "user" / "assistant"
    std::wstring text;
    std::wstring userName; // Add this line
};

struct ChatSession {
    std::string             sessionId;
    std::vector<ChatBlock>  blocks;

    // ── 채팅 메타 (사이드바 진척 막대·메타 편집 토대) ──
    std::string  title;           // 사용자 친화 제목 (빈 채면 첫 user 블록 첫 줄로 fallback)
    int          progress = 0;    // 0~100
    std::string  summary;         // 채팅 요약 — 백엔드 공유 컨텍스트의 1차 메모리
    std::string  lastActiveIso;   // 마지막 활동 시각 (자동 갱신, mtime 보강용)
    int          totalBlockCount = 0;
    bool         historyPartial = false;
};

// 윈도우 위치·크기·표시 상태. WINDOWPLACEMENT.rcNormalPosition 기반(최대화/최소화 시에도 normal 좌표 유지).
struct WindowState {
    int  x            = 0;
    int  y            = 0;
    int  width        = 0;
    int  height       = 0;
    int  showCmd      = SW_SHOWNORMAL;  // SW_SHOWNORMAL 또는 SW_SHOWMAXIMIZED (최소화는 normal로 환원)
    int  sidebarWidth = 0;              // 0 = 미저장, > 0 = 사이드바 폭 픽셀
    bool valid        = false;
};

class Persistence {
public:
    static std::wstring AppDataDir() {
        wchar_t appdata[MAX_PATH] = L"";
        if (GetEnvironmentVariableW(L"APPDATA", appdata, MAX_PATH) == 0) return L"";
        std::wstring dir = std::wstring(appdata) + L"\\OrangeCode";
        CreateDirectoryW(dir.c_str(), nullptr);
        return dir;
    }

    static std::wstring DatabasePath() {
        std::wstring dir = AppDataDir();
        if (dir.empty()) return L"";
        return dir + L"\\orange_code.db";
    }

    static bool EnsureChatDatabase() {
        static bool opened = false;
        if (opened) return true;
        std::wstring path = DatabasePath();
        if (path.empty()) return false;
        try {
            CDatabase::Instance().Open(path);
            CDatabase::Instance().Execute(
                "CREATE TABLE IF NOT EXISTS ChatSessions ("
                "  key TEXT PRIMARY KEY,"
                "  scope TEXT NOT NULL DEFAULT 'global',"
                "  goal_id TEXT,"
                "  project_id TEXT,"
                "  provider TEXT,"
                "  session_id TEXT,"
                "  title TEXT,"
                "  summary TEXT,"
                "  progress INTEGER NOT NULL DEFAULT 0,"
                "  last_active_iso INTEGER NOT NULL,"
                "  created_at INTEGER NOT NULL,"
                "  updated_at INTEGER NOT NULL,"
                "  last_active_at INTEGER,"
                "  created_at_gmt INTEGER,"
                "  updated_at_gmt INTEGER,"
                "  summary_updated_at_gmt INTEGER"
                ");"
            );
            try {
                CDatabase::Instance().Execute("ALTER TABLE ChatSessions ADD COLUMN summary TEXT;");
            } catch (...) {}
            try { CDatabase::Instance().Execute("ALTER TABLE ChatSessions ADD COLUMN last_active_at INTEGER;"); } catch (...) {}
            try { CDatabase::Instance().Execute("ALTER TABLE ChatSessions ADD COLUMN created_at_gmt INTEGER;"); } catch (...) {}
            try { CDatabase::Instance().Execute("ALTER TABLE ChatSessions ADD COLUMN updated_at_gmt INTEGER;"); } catch (...) {}
            try { CDatabase::Instance().Execute("ALTER TABLE ChatSessions ADD COLUMN summary_updated_at_gmt INTEGER;"); } catch (...) {}
            CDatabase::Instance().Execute(
                "CREATE TABLE IF NOT EXISTS ChatBlocks ("
                "  chat_key TEXT NOT NULL,"
                "  seq INTEGER NOT NULL,"
                "  role TEXT NOT NULL,"
                "  text TEXT NOT NULL,"
                "  created_at INTEGER NOT NULL,"
                "  created_at_gmt INTEGER,"
                "  PRIMARY KEY(chat_key, seq),"
                "  FOREIGN KEY(chat_key) REFERENCES ChatSessions(key) ON DELETE CASCADE"
                ");"
            );
            try { CDatabase::Instance().Execute("ALTER TABLE ChatBlocks ADD COLUMN created_at_gmt INTEGER;"); } catch (...) {}
            CDatabase::Instance().Execute(
                "CREATE TABLE IF NOT EXISTS ChatAttachments ("
                "  chat_key TEXT NOT NULL,"
                "  id TEXT NOT NULL,"
                "  kind TEXT NOT NULL,"
                "  name TEXT NOT NULL,"
                "  original_path TEXT,"
                "  stored_path TEXT NOT NULL,"
                "  thumbnail_path TEXT,"
                "  mime TEXT,"
                "  size_bytes INTEGER NOT NULL DEFAULT 0,"
                "  created_at INTEGER NOT NULL,"
                "  created_at_gmt INTEGER,"
                "  PRIMARY KEY(chat_key, id),"
                "  FOREIGN KEY(chat_key) REFERENCES ChatSessions(key) ON DELETE CASCADE"
                ");"
            );
            try { CDatabase::Instance().Execute("ALTER TABLE ChatAttachments ADD COLUMN created_at_gmt INTEGER;"); } catch (...) {}
            CDatabase::Instance().Execute(
                "CREATE INDEX IF NOT EXISTS idx_chat_sessions_scope "
                "ON ChatSessions(scope, updated_at_gmt DESC);"
            );
            CDatabase::Instance().Execute(
                "CREATE INDEX IF NOT EXISTS idx_chat_sessions_goal "
                "ON ChatSessions(goal_id, project_id, updated_at_gmt DESC);"
            );
            CDatabase::Instance().Execute(
                "CREATE INDEX IF NOT EXISTS idx_chat_attachments_chat "
                "ON ChatAttachments(chat_key, created_at_gmt DESC);"
            );
            opened = true;
            return true;
        } catch (...) {
            return false;
        }
    }

    static FILETIME FileTimeFromIso(const std::string& iso) {
        SYSTEMTIME st{};
        int y = 0, mo = 0, d = 0, h = 0, mi = 0, s = 0;
        if (sscanf_s(iso.c_str(), "%d-%d-%dT%d:%d:%dZ", &y, &mo, &d, &h, &mi, &s) == 6 ||
            sscanf_s(iso.c_str(), "%d-%d-%d %d:%d:%d", &y, &mo, &d, &h, &mi, &s) == 6) {
            st.wYear = (WORD)y;
            st.wMonth = (WORD)mo;
            st.wDay = (WORD)d;
            st.wHour = (WORD)h;
            st.wMinute = (WORD)mi;
            st.wSecond = (WORD)s;
            FILETIME ft{};
            if (SystemTimeToFileTime(&st, &ft)) return ft;
        }
        FILETIME ft{};
        GetSystemTimeAsFileTime(&ft);
        return ft;
    }

    static FILETIME FileTimeFromUnix(long long unixSeconds) {
        if (unixSeconds <= 0) {
            FILETIME ft{};
            GetSystemTimeAsFileTime(&ft);
            return ft;
        }
        ULARGE_INTEGER uli{};
        uli.QuadPart = ((ULONGLONG)unixSeconds * 10000000ULL) + 116444736000000000ULL;
        FILETIME ft{};
        ft.dwLowDateTime = uli.LowPart;
        ft.dwHighDateTime = uli.HighPart;
        return ft;
    }

    static long long TimestampNowGmt() {
        return (long long)std::time(nullptr);
    }

    static Json::Int64 JsonTimestamp(const Json::Value& row, const char* key) {
        const Json::Value v = row[key];
        if (v.isInt64() || v.isInt()) return v.asInt64();
        if (v.isUInt64() || v.isUInt()) return (Json::Int64)v.asUInt64();
        if (v.isString()) {
            try { return (Json::Int64)std::stoll(v.asString()); } catch (...) { return 0; }
        }
        return 0;
    }

    static std::string TimestampExpr(const char* column) {
        std::string c(column);
        return "CASE "
               "WHEN typeof(" + c + ")='integer' THEN " + c + " "
               "WHEN typeof(" + c + ")='text' AND " + c + " GLOB '[0-9]*' THEN CAST(" + c + " AS INTEGER) "
               "ELSE CAST(strftime('%s', " + c + ") AS INTEGER) END";
    }

    static std::string UnixToIsoUtc(Json::Int64 unixSeconds) {
        if (unixSeconds <= 0) return {};
        std::time_t t = (std::time_t)unixSeconds;
        std::tm tm{};
        gmtime_s(&tm, &t);
        char buf[32] = "";
        strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
        return buf;
    }

    // 파일명에 안전한 문자만 통과: A-Z a-z 0-9 - _ .
    static std::string SanitizeKey(const std::string& key) {
        std::string out;
        out.reserve(key.size());
        for (char c : key) {
            const bool safe =
                (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.';
            out += safe ? c : '_';
        }
        if (out.empty()) out = "default";
        return out;
    }

    // 평면 경로 (legacy) — 옛 인스턴스 데이터 호환 read 용. 새 데이터는 ChatFilePath (위계) 사용.
    static std::wstring LegacySessionFilePath(const std::string& key) {
        wchar_t appdata[MAX_PATH] = L"";
        if (GetEnvironmentVariableW(L"APPDATA", appdata, MAX_PATH) == 0) {
            return L"";
        }
        std::wstring dir = std::wstring(appdata) + L"\\OrangeCode";
        CreateDirectoryW(dir.c_str(), nullptr);
        std::string clean = SanitizeKey(key.empty() ? std::string("default") : key);
        return dir + L"\\session_" +
               Utf8ToWide(clean.c_str(), (int)clean.size()) + L".json";
    }

    // 본 경로 — *위계* 우선 (목표/프로젝트 안 chats/).
    //
    // 부모 컨텍스트 인계 자세: 자식 OrangeCode 가 multi-target NewChat 으로 spawn 됐다면
    // 부모(정팀장) 가 `--goal-id` / `--project-id` 명령행을 박아 startup 시 env
    // (`ORANGE_CODE_GOAL_ID` / `ORANGE_CODE_PROJECT_ID`) 가 set 되어 있습니다. 본 함수는
    // 그 env 를 보고 비어있지 않으면 그 위계의 chats/ 에 저장하고, 비어있으면 default 결을
    // 유지합니다. 호출자가 글로벌을 직접 참조하지 않게 해서 layering 을 깨끗하게 둡니다.
    static std::wstring SessionFilePath(const std::string& key) {
        std::string goalId    = CGoal::kDefaultId;
        std::string projectId = CProject::kDefaultId;

        char buf[256];
        DWORD n = GetEnvironmentVariableA("ORANGE_CODE_GOAL_ID", buf, sizeof(buf));
        if (n > 0 && n < sizeof(buf)) {
            std::string v(buf, n);
            if (!v.empty()) goalId = v;
        }
        n = GetEnvironmentVariableA("ORANGE_CODE_PROJECT_ID", buf, sizeof(buf));
        if (n > 0 && n < sizeof(buf)) {
            std::string v(buf, n);
            if (!v.empty()) projectId = v;
        }

        // 위계 디렉토리 보장 (default 또는 부모 인계 컨텍스트).
        CGoal::EnsureExists(goalId,
                            (goalId == CGoal::kDefaultId) ? CGoal::kDefaultTitle : goalId);
        CProject::EnsureExists(goalId, projectId,
                               (projectId == CProject::kDefaultId) ? CProject::kDefaultTitle
                                                                    : projectId);
        return ChatFilePath(goalId, projectId,
                            key.empty() ? std::string("default") : key);
    }

    static bool Save(const ChatSession& session, const std::string& key) {
        if (!EnsureChatDatabase()) return false;
        std::string clean = SanitizeKey(key.empty() ? std::string("default") : key);

        std::string goalId;
        std::string projectId;
        char buf[256];
        DWORD n = GetEnvironmentVariableA("ORANGE_CODE_GOAL_ID", buf, sizeof(buf));
        if (n > 0 && n < sizeof(buf)) goalId.assign(buf, n);
        n = GetEnvironmentVariableA("ORANGE_CODE_PROJECT_ID", buf, sizeof(buf));
        if (n > 0 && n < sizeof(buf)) projectId.assign(buf, n);
        const bool hasGoalScope = !goalId.empty() && goalId != CGoal::kDefaultId;
        std::string scope = hasGoalScope ? "goal" : "global";
        if (!hasGoalScope) {
            goalId.clear();
            projectId.clear();
        } else if (projectId.empty()) {
            projectId = CProject::kDefaultId;
        }

        Json::Int64 nowGmt = (Json::Int64)TimestampNowGmt();
        Json::Value p;
        p["key"] = clean;
        p["scope"] = scope;
        p["goal_id"] = goalId;
        p["project_id"] = projectId;
        p["session_id"] = IsLikelyUuid(session.sessionId) ? session.sessionId : std::string();
        p["title"] = session.title;
        p["summary"] = session.summary;
        p["progress"] = session.progress < 0 ? 0 : (session.progress > 100 ? 100 : session.progress);
        p["last_active_iso"] = nowGmt;
        p["created_at"] = nowGmt;
        p["updated_at"] = nowGmt;
        p["last_active_at"] = nowGmt;
        p["created_at_gmt"] = nowGmt;
        p["updated_at_gmt"] = nowGmt;
        p["summary_updated_at_gmt"] = session.summary.empty() ? Json::Value::null : Json::Value(nowGmt);

        try {
            CDatabase::Instance().Execute("BEGIN IMMEDIATE;");
            CDatabase::Instance().Run(
                "SaveChatSession",
                "INSERT INTO ChatSessions "
                "(key, scope, goal_id, project_id, session_id, title, summary, progress, last_active_iso, created_at, updated_at, last_active_at, created_at_gmt, updated_at_gmt, summary_updated_at_gmt) "
                "VALUES (:key, :scope, NULLIF(:goal_id, ''), NULLIF(:project_id, ''), :session_id, :title, :summary, :progress, :last_active_iso, :created_at, :updated_at, :last_active_at, :created_at_gmt, :updated_at_gmt, :summary_updated_at_gmt) "
                "ON CONFLICT(key) DO UPDATE SET "
                "scope=excluded.scope, goal_id=excluded.goal_id, project_id=excluded.project_id, "
                "session_id=excluded.session_id, title=excluded.title, summary=excluded.summary, progress=excluded.progress, "
                "last_active_iso=excluded.last_active_iso, updated_at=excluded.updated_at, "
                "last_active_at=excluded.last_active_at, updated_at_gmt=excluded.updated_at_gmt, "
                "summary_updated_at_gmt=CASE WHEN excluded.summary='' THEN ChatSessions.summary_updated_at_gmt ELSE excluded.summary_updated_at_gmt END;",
                p
            );
            Json::Value del;
            del["key"] = clean;
            CDatabase::Instance().Run("DeleteChatBlocks", "DELETE FROM ChatBlocks WHERE chat_key=:key;", del);
            int seq = 0;
            for (const auto& b : session.blocks) {
                Json::Value bp;
                bp["chat_key"] = clean;
                bp["seq"] = seq++;
                bp["role"] = WideToUtf8(b.role);
                bp["text"] = WideToUtf8(b.text);
                bp["created_at"] = nowGmt;
                bp["created_at_gmt"] = nowGmt;
                CDatabase::Instance().Run(
                    "InsertChatBlock",
                    "INSERT INTO ChatBlocks (chat_key, seq, role, text, created_at, created_at_gmt) "
                    "VALUES (:chat_key, :seq, :role, :text, :created_at, :created_at_gmt);",
                    bp
                );
            }
            CDatabase::Instance().Execute("COMMIT;");
            return true;
        } catch (...) {
            try { CDatabase::Instance().Execute("ROLLBACK;"); } catch (...) {}
            return false;
        }
    }

    struct SessionMeta {
        std::string  key;          // session_<KEY>.json 의 KEY 부분
        std::wstring fullPath;     // 전체 경로
        FILETIME     mtime;        // 마지막 수정 시각 (정렬용)
        std::wstring previewLabel; // 첫 user 메시지의 첫 줄 (사이드바 사람 친화 라벨)
        // 채팅 메타 — 사이드바 진척 막대·hover 툴팁 인계.
        std::string  title;
        std::string  summary;
        int          progress = 0;
        std::string  lastActiveIso;  // 마지막 활동 시각 — Save 가 자동 갱신.
        std::wstring providerLabel;  // Sidebar label derived from assistant-* roles.
    };

    // mtime 을 짧게 — 오늘이면 HH:MM, 다른 날이면 M/D. 빈 문자열은 변환 실패.
    static std::wstring FormatMtimeShort(FILETIME mtime) {
        FILETIME localFt; SYSTEMTIME local;
        if (!FileTimeToLocalFileTime(&mtime, &localFt)) return {};
        if (!FileTimeToSystemTime(&localFt, &local))    return {};
        SYSTEMTIME now; GetLocalTime(&now);
        wchar_t buf[16];
        if (local.wYear == now.wYear && local.wMonth == now.wMonth &&
            local.wDay == now.wDay) {
            swprintf_s(buf, L"%02d:%02d", local.wHour, local.wMinute);
        } else {
            swprintf_s(buf, L"%d/%d", local.wMonth, local.wDay);
        }
        return buf;
    }

    // 세션 한 건의 첫 user 블록을 짧게 정리해 미리보기 라벨 생성.
    // 비어있으면 빈 문자열 반환 — 호출자는 키 fallback.
    static std::wstring DerivePreviewLabel(const ChatSession& cs, size_t maxLen = 22) {
        std::wstring label;
        for (const auto& b : cs.blocks) {
            if (b.role == L"user") { label = b.text; break; }
        }
        if (label.empty()) return label;
        // DispatchPrompt 가 붙이는 "> " prefix 제거
        if (label.size() >= 2 && label[0] == L'>' && label[1] == L' ') {
            label.erase(0, 2);
        }
        // 첫 줄만
        size_t nl = label.find_first_of(L"\r\n");
        if (nl != std::wstring::npos) label.resize(nl);
        // 길이 제한 — 시각 suffix 8자 정도 붙을 자리 확보용으로 짧게
        if (label.size() > maxLen) {
            label = label.substr(0, maxLen) + L"…";
        }
        return label;
    }

    static std::wstring BuildPreviewLabelFromMeta(const SessionMeta& meta, size_t maxLen = 22) {
        std::wstring label;
        if (!meta.summary.empty()) {
            label = Utf8ToWide(meta.summary.c_str(), (int)meta.summary.size());
        } else if (!meta.title.empty()) {
            label = Utf8ToWide(meta.title.c_str(), (int)meta.title.size());
        } else {
            label = Utf8ToWide(meta.key.c_str(), (int)meta.key.size());
        }
        if (label.size() >= 2 && label[0] == L'>' && label[1] == L' ') {
            label.erase(0, 2);
        }
        size_t nl = label.find_first_of(L"\r\n");
        if (nl != std::wstring::npos) label.resize(nl);
        if (label.size() > maxLen) label = label.substr(0, maxLen) + L"…";
        return label;
    }

    static std::wstring BuildProviderLabelForChat(const std::string& key) {
        if (key.empty() || !EnsureChatDatabase()) return {};
        Json::Value p;
        p["key"] = key;
        bool hasClaude = false;
        bool hasGemini = false;
        bool hasCodex = false;
        bool hasMock = false;
        try {
            CDatabase::Instance().Fetch(
                "ChatProviderLabel",
                "SELECT DISTINCT role FROM ChatBlocks "
                "WHERE chat_key=:key AND role IN ('assistant-claude', 'assistant-gemini', 'assistant-codex', 'assistant-mock');",
                p,
                [&](const Json::Value& row) {
                    std::string role = row.get("role", "").asString();
                    if (role == "assistant-claude") hasClaude = true;
                    else if (role == "assistant-gemini") hasGemini = true;
                    else if (role == "assistant-codex") hasCodex = true;
                    else if (role == "assistant-mock") hasMock = true;
                    return true;
                }
            );
        } catch (...) {}

        std::wstring label;
        auto add = [&](const wchar_t* name) {
            if (!label.empty()) label += L"+";
            label += name;
        };
        const bool hasRealProvider = hasClaude || hasGemini || hasCodex;
        if (hasClaude) add(L"Claude");
        if (hasGemini) add(L"Gemini");
        if (hasCodex) add(L"Codex");
        if (!hasRealProvider && hasMock) add(L"Mock");
        return label;
    }

    // %APPDATA%\OrangeCode\session_*.json 의 모든 세션 메타. 최근 수정 순(내림차순).
    // previewLabel 도 채움 — 각 세션을 Load 하므로 자주 호출하지 말 것.
    // 위계 chats/ 디렉토리 + legacy 평면 둘 다 글롭. 같은 key 면 위계 우선 (dedupe).
    // OR-1317 사이드바 본 작업 — *모든 목표/프로젝트의 채팅* 보기는 다음 사이클 (현재는 default 만).
    static std::vector<SessionMeta> ListSessions() {
        std::vector<SessionMeta> out;
        if (!EnsureChatDatabase()) return out;
        try {
            CDatabase::Instance().Fetch(
                "ListGlobalChats",
                "SELECT key, title, summary, progress, last_active_iso, "
                "COALESCE(updated_at_gmt, CAST(strftime('%s', updated_at) AS INTEGER)) AS updated_at_gmt "
                "FROM ChatSessions WHERE scope='global' "
                "ORDER BY COALESCE(updated_at_gmt, CAST(strftime('%s', updated_at) AS INTEGER)) DESC;",
                Json::Value(),
                [&](const Json::Value& row) {
                    SessionMeta m;
                    m.key = row.get("key", "").asString();
                    m.title = row.get("title", "").asString();
                    m.summary = row.get("summary", "").asString();
                    m.progress = row.get("progress", 0).asInt();
                    m.lastActiveIso = row.get("last_active_iso", "").asString();
                    m.mtime = FileTimeFromUnix(JsonTimestamp(row, "updated_at_gmt"));
                    m.providerLabel = BuildProviderLabelForChat(m.key);
                    std::wstring text = BuildPreviewLabelFromMeta(m);
                    std::wstring time = FormatMtimeShort(m.mtime);
                    m.previewLabel = !text.empty() && !time.empty() ? (text + L" · " + time)
                                   : !text.empty()                  ? text
                                                                     : time;
                    out.push_back(std::move(m));
                    return true;
                }
            );
        } catch (...) {}
        return out;
    }

    static std::string LatestGlobalChatKey() {
        auto sessions = ListSessions();
        if (sessions.empty()) return {};
        return sessions.front().key;
    }

    static bool SaveAttachmentRecord(const std::string& key, const AttachmentRecord& rec) {
        if (!EnsureChatDatabase() || rec.id.empty() || rec.storedPath.empty()) return false;
        Json::Value p;
        p["chat_key"] = SanitizeKey(key.empty() ? std::string("default") : key);
        p["id"] = WideToUtf8(rec.id);
        p["kind"] = WideToUtf8(rec.kind);
        p["name"] = WideToUtf8(rec.name);
        p["original_path"] = WideToUtf8(rec.originalPath);
        p["stored_path"] = WideToUtf8(rec.storedPath);
        p["thumbnail_path"] = WideToUtf8(rec.thumbnailPath);
        p["mime"] = WideToUtf8(rec.mime);
        p["size_bytes"] = Json::UInt64(rec.sizeBytes);
        p["created_at"] = (Json::Int64)TimestampNowGmt();
        p["created_at_gmt"] = (Json::Int64)TimestampNowGmt();
        try {
            CDatabase::Instance().Run(
                "SaveChatAttachment",
                "INSERT INTO ChatAttachments "
                "(chat_key, id, kind, name, original_path, stored_path, thumbnail_path, mime, size_bytes, created_at, created_at_gmt) "
                "VALUES (:chat_key, :id, :kind, :name, :original_path, :stored_path, :thumbnail_path, :mime, :size_bytes, :created_at, :created_at_gmt) "
                "ON CONFLICT(chat_key, id) DO UPDATE SET "
                "kind=excluded.kind, name=excluded.name, original_path=excluded.original_path, "
                "stored_path=excluded.stored_path, thumbnail_path=excluded.thumbnail_path, "
                "mime=excluded.mime, size_bytes=excluded.size_bytes, created_at_gmt=excluded.created_at_gmt;",
                p
            );
            return true;
        } catch (...) {
            return false;
        }
    }

    static std::vector<AttachmentRecord> ListAttachmentRecords(const std::string& key) {
        std::vector<AttachmentRecord> out;
        if (!EnsureChatDatabase()) return out;
        Json::Value p;
        p["key"] = SanitizeKey(key.empty() ? std::string("default") : key);
        try {
            CDatabase::Instance().Fetch(
                "ListChatAttachments",
                "SELECT id, kind, name, original_path, stored_path, thumbnail_path, mime, size_bytes "
                "FROM ChatAttachments WHERE chat_key=:key ORDER BY COALESCE(created_at_gmt, CAST(strftime('%s', created_at) AS INTEGER)) ASC;",
                p,
                [&](const Json::Value& row) {
                    AttachmentRecord rec;
                    rec.id = Utf8ToWide(row.get("id", "").asString().c_str(), (int)row.get("id", "").asString().size());
                    rec.kind = Utf8ToWide(row.get("kind", "").asString().c_str(), (int)row.get("kind", "").asString().size());
                    rec.name = Utf8ToWide(row.get("name", "").asString().c_str(), (int)row.get("name", "").asString().size());
                    rec.originalPath = Utf8ToWide(row.get("original_path", "").asString().c_str(), (int)row.get("original_path", "").asString().size());
                    rec.storedPath = Utf8ToWide(row.get("stored_path", "").asString().c_str(), (int)row.get("stored_path", "").asString().size());
                    rec.thumbnailPath = Utf8ToWide(row.get("thumbnail_path", "").asString().c_str(), (int)row.get("thumbnail_path", "").asString().size());
                    rec.mime = Utf8ToWide(row.get("mime", "").asString().c_str(), (int)row.get("mime", "").asString().size());
                    rec.sizeBytes = row.get("size_bytes", Json::UInt64(0)).asUInt64();
                    if (!rec.id.empty()) out.push_back(std::move(rec));
                    return true;
                }
            );
        } catch (...) {}
        if (out.empty()) {
            std::wstring wkey = Utf8ToWide(p["key"].asString().c_str(), (int)p["key"].asString().size());
            out = AttachmentStore::LoadManifest(wkey);
        }
        return out;
    }

    static Json::Value ChatApiList(int limit = 40) {
        Json::Value root;
        root["api"] = "orange.chat.v1";
        root["action"] = "list";
        root["database_path"] = WideToUtf8(DatabasePath());
        root["limits"]["limit"] = limit <= 0 ? 40 : limit;
        Json::Value chats(Json::arrayValue);
        if (!EnsureChatDatabase()) {
            root["error"] = "chat database unavailable";
            return root;
        }
        Json::Value p;
        p["limit"] = limit <= 0 ? 40 : limit;
        try {
            CDatabase::Instance().Fetch(
                "ChatApiList",
                "SELECT s.key, s.scope, s.goal_id, s.project_id, s.provider, s.session_id, "
                "s.title, s.summary, s.progress, "
                "COALESCE(s.last_active_at, CAST(strftime('%s', s.last_active_iso) AS INTEGER)) AS last_active_at, "
                "COALESCE(s.created_at_gmt, CAST(strftime('%s', s.created_at) AS INTEGER)) AS created_at, "
                "COALESCE(s.updated_at_gmt, CAST(strftime('%s', s.updated_at) AS INTEGER)) AS updated_at, "
                "s.summary_updated_at_gmt, "
                "(SELECT COUNT(*) FROM ChatBlocks b WHERE b.chat_key=s.key) AS block_count, "
                "(SELECT COUNT(*) FROM ChatAttachments a WHERE a.chat_key=s.key) AS attachment_count "
                "FROM ChatSessions s ORDER BY COALESCE(s.updated_at_gmt, CAST(strftime('%s', s.updated_at) AS INTEGER)) DESC LIMIT :limit;",
                p,
                [&](const Json::Value& row) {
                    Json::Value item;
                    for (const auto& name : row.getMemberNames()) item[name] = row[name];
                    item["last_active_at"] = UnixToIsoUtc(JsonTimestamp(row, "last_active_at"));
                    item["created_at"] = UnixToIsoUtc(JsonTimestamp(row, "created_at"));
                    item["updated_at"] = UnixToIsoUtc(JsonTimestamp(row, "updated_at"));
                    item["summary_updated_at"] = UnixToIsoUtc(JsonTimestamp(row, "summary_updated_at_gmt"));
                    item.removeMember("summary_updated_at_gmt");
                    chats.append(item);
                    return true;
                }
            );
        } catch (...) {
            root["error"] = "chat list query failed";
        }
        root["chats"] = chats;
        return root;
    }

    static Json::Value ChatApiShow(const std::string& key, int recentLimit = 80) {
        std::string clean = SanitizeKey(key.empty() ? std::string("default") : key);
        ChatSession session = LoadRecent(clean, recentLimit <= 0 ? 80 : recentLimit);
        Json::Value root;
        root["api"] = "orange.chat.v1";
        root["action"] = "show";
        root["key"] = clean;
        root["title"] = session.title;
        root["summary"] = session.summary;
        root["session_id"] = session.sessionId;
        root["progress"] = session.progress;
        root["last_active_at"] = "";
        Json::Value metaParam;
        metaParam["key"] = clean;
        try {
            CDatabase::Instance().Fetch(
                "ChatApiShowTimes",
                "SELECT COALESCE(last_active_at, CAST(strftime('%s', last_active_iso) AS INTEGER)) AS last_active_at, "
                "COALESCE(created_at_gmt, CAST(strftime('%s', created_at) AS INTEGER)) AS created_at, "
                "COALESCE(updated_at_gmt, CAST(strftime('%s', updated_at) AS INTEGER)) AS updated_at, "
                "summary_updated_at_gmt FROM ChatSessions WHERE key=:key;",
                metaParam,
                [&](const Json::Value& row) {
                    root["last_active_at"] = UnixToIsoUtc(JsonTimestamp(row, "last_active_at"));
                    root["created_at"] = UnixToIsoUtc(JsonTimestamp(row, "created_at"));
                    root["updated_at"] = UnixToIsoUtc(JsonTimestamp(row, "updated_at"));
                    root["summary_updated_at"] = UnixToIsoUtc(JsonTimestamp(row, "summary_updated_at_gmt"));
                    return false;
                }
            );
        } catch (...) {}
        root["total_block_count"] = session.totalBlockCount;
        root["history_partial"] = session.historyPartial;
        Json::Value blocks(Json::arrayValue);
        Json::Value providers;
        for (const auto& b : session.blocks) {
            std::string role = WideToUtf8(b.role);
            Json::Value item;
            item["seq"] = b.seq;
            item["role"] = role;
            item["provider"] =
                role == "assistant-claude" ? "Claude" :
                role == "assistant-gemini" ? "Gemini" :
                role == "assistant-codex" ? "Codex" :
                role == "assistant-mock" ? "Mock" : "";
            item["text"] = WideToUtf8(b.text);
            blocks.append(item);
            if (!item["provider"].asString().empty()) {
                providers[item["provider"].asString()] = providers.get(item["provider"].asString(), 0).asInt() + 1;
            }
        }
        root["blocks"] = blocks;
        root["providers"] = providers;
        Json::Value attachments(Json::arrayValue);
        for (const auto& rec : ListAttachmentRecords(clean)) {
            Json::Value item;
            item["id"] = WideToUtf8(rec.id);
            item["kind"] = WideToUtf8(rec.kind);
            item["name"] = WideToUtf8(rec.name);
            item["original_path"] = WideToUtf8(rec.originalPath);
            item["stored_path"] = WideToUtf8(rec.storedPath);
            item["thumbnail_path"] = WideToUtf8(rec.thumbnailPath);
            item["mime"] = WideToUtf8(rec.mime);
            item["size_bytes"] = Json::UInt64(rec.sizeBytes);
            attachments.append(item);
        }
        root["attachments"] = attachments;
        return root;
    }

    static Json::Value ChatApiAttachments(const std::string& key) {
        std::string clean = SanitizeKey(key.empty() ? std::string("default") : key);
        Json::Value root;
        root["api"] = "orange.chat.v1";
        root["action"] = "attachments";
        root["key"] = clean;
        Json::Value attachments(Json::arrayValue);
        for (const auto& rec : ListAttachmentRecords(clean)) {
            Json::Value item;
            item["id"] = WideToUtf8(rec.id);
            item["kind"] = WideToUtf8(rec.kind);
            item["name"] = WideToUtf8(rec.name);
            item["original_path"] = WideToUtf8(rec.originalPath);
            item["stored_path"] = WideToUtf8(rec.storedPath);
            item["thumbnail_path"] = WideToUtf8(rec.thumbnailPath);
            item["mime"] = WideToUtf8(rec.mime);
            item["size_bytes"] = Json::UInt64(rec.sizeBytes);
            attachments.append(item);
        }
        root["attachments"] = attachments;
        return root;
    }

    static Json::Value ChatApiRename(const std::string& key, const std::string& title) {
        std::string clean = SanitizeKey(key.empty() ? std::string("default") : key);
        Json::Value root;
        root["api"] = "orange.chat.v1";
        root["action"] = "rename";
        root["key"] = clean;
        root["title"] = title;
        if (title.empty()) {
            root["error"] = "title is required";
            return root;
        }
        if (!EnsureChatDatabase()) {
            root["error"] = "chat database unavailable";
            return root;
        }

        bool exists = false;
        Json::Value p;
        p["key"] = clean;
        p["title"] = title;
        p["now"] = (Json::Int64)TimestampNowGmt();
        try {
            CDatabase::Instance().Fetch(
                "ChatApiRenameExists",
                "SELECT key FROM ChatSessions WHERE key=:key LIMIT 1;",
                p,
                [&](const Json::Value&) {
                    exists = true;
                    return false;
                }
            );
            if (!exists) {
                root["error"] = "chat key not found";
                return root;
            }
            CDatabase::Instance().Run(
                "ChatApiRename",
                "UPDATE ChatSessions "
                "SET title=:title, updated_at_gmt=:now, updated_at=:now "
                "WHERE key=:key;",
                p
            );
            root["updated_at"] = UnixToIsoUtc(p["now"].asInt64());
        } catch (...) {
            root["error"] = "chat rename failed";
        }
        return root;
    }

    static Json::Value ChatApiImport(const std::string& key,
                                     const std::string& title,
                                     const std::wstring& handoffText,
                                     bool overwrite = false) {
        std::string clean = SanitizeKey(key.empty() ? std::string("cli-handoff") : key);
        Json::Value root;
        root["api"] = "orange.chat.v1";
        root["action"] = "import";
        root["key"] = clean;
        root["title"] = title.empty() ? std::string("CLI handoff") : title;
        root["block_count"] = 0;
        root["imported_bytes"] = 0;
        if (handoffText.empty()) {
            root["error"] = "empty import input";
            return root;
        }
        if (!EnsureChatDatabase()) {
            root["error"] = "chat database unavailable";
            return root;
        }

        bool exists = false;
        Json::Value p;
        p["key"] = clean;
        try {
            CDatabase::Instance().Fetch(
                "ChatApiImportExists",
                "SELECT key FROM ChatSessions WHERE key=:key LIMIT 1;",
                p,
                [&](const Json::Value&) {
                    exists = true;
                    return false;
                }
            );
        } catch (...) {
            root["error"] = "chat existence query failed";
            return root;
        }
        if (exists && !overwrite) {
            root["error"] = "chat key already exists";
            return root;
        }

        ChatSession session;
        session.title = title.empty() ? std::string("CLI handoff") : title;
        std::string handoffUtf8 = WideToUtf8(handoffText);
        std::wstring summaryWide = handoffText.substr(0, handoffText.size() > 240 ? 240 : handoffText.size());
        session.summary = WideToUtf8(summaryWide);
        session.progress = 0;

        ChatBlock user;
        user.role = L"user";
        user.text = L"이 CLI 세션에서 하던 작업을 이어가자.";
        session.blocks.push_back(std::move(user));

        ChatBlock task;
        task.role = L"task";
        task.text = handoffText;
        session.blocks.push_back(std::move(task));

        if (!Save(session, clean)) {
            root["error"] = "chat save failed";
            return root;
        }

        root["block_count"] = (Json::Int64)session.blocks.size();
        root["imported_bytes"] = (Json::Int64)handoffUtf8.size();
        root["summary"] = session.summary;
        return root;
    }

    // ── 위계 글롭 (OR-1317 사이드바 트리 — 모든 목표/프로젝트/채팅 보기) ──
    //
    // 디스크 구조: %APPDATA%\OrangeCode\goals\<goalId>\projects\<projectId>\chats\chat_<chatId>.json
    // 세 헬퍼 — 사이드바 RefillSidebar 가 트리 그릴 때 위에서부터 글롭.

    // %APPDATA%\OrangeCode\goals\* 디렉토리 글롭 + 각 meta.json 로드. mtime 내림차순.
    // 빈 디렉토리 / 손상 meta 는 skip — 호출자 안전.
    static std::vector<CGoal> ListAllGoals() {
        std::vector<CGoal> out;
        wchar_t appdata[MAX_PATH] = L"";
        if (GetEnvironmentVariableW(L"APPDATA", appdata, MAX_PATH) == 0) return out;
        std::wstring goalsDir = std::wstring(appdata) + L"\\OrangeCode\\goals";
        std::wstring pattern  = goalsDir + L"\\*";

        // 정렬용 (디렉토리 mtime, CGoal) 페어 묶음.
        struct GoalEntry { FILETIME mtime; CGoal g; };
        std::vector<GoalEntry> entries;

        WIN32_FIND_DATAW fd{};
        HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) return out;
        do {
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) continue;
            std::wstring name = fd.cFileName;
            if (name == L"." || name == L"..") continue;

            std::string id = WideToUtf8(name);
            CGoal g = CGoal::Load(id);
            if (g.id.empty()) continue;  // 손상 meta — Load 가 빈 struct 반환 시 skip
            entries.push_back({ fd.ftLastWriteTime, std::move(g) });
        } while (FindNextFileW(h, &fd));
        FindClose(h);

        std::sort(entries.begin(), entries.end(),
                  [](const GoalEntry& a, const GoalEntry& b) {
            return CompareFileTime(&a.mtime, &b.mtime) > 0;
        });
        for (auto& e : entries) out.push_back(std::move(e.g));
        return out;
    }

    // %APPDATA%\OrangeCode\goals\<goalId>\projects\* 글롭 + meta.json 로드. mtime 내림차순.
    static std::vector<CProject> ListProjects(const std::string& goalId) {
        std::vector<CProject> out;
        wchar_t appdata[MAX_PATH] = L"";
        if (GetEnvironmentVariableW(L"APPDATA", appdata, MAX_PATH) == 0) return out;
        std::string safeGoal = CGoal::SanitizeId(goalId);
        std::wstring projsDir = std::wstring(appdata) + L"\\OrangeCode\\goals\\" +
                                Utf8ToWide(safeGoal.c_str(), (int)safeGoal.size()) +
                                L"\\projects";
        std::wstring pattern = projsDir + L"\\*";

        struct ProjEntry { FILETIME mtime; CProject p; };
        std::vector<ProjEntry> entries;

        WIN32_FIND_DATAW fd{};
        HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) return out;
        do {
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) continue;
            std::wstring name = fd.cFileName;
            if (name == L"." || name == L"..") continue;

            std::string id = WideToUtf8(name);
            CProject p = CProject::Load(goalId, id);
            if (p.id.empty()) continue;
            entries.push_back({ fd.ftLastWriteTime, std::move(p) });
        } while (FindNextFileW(h, &fd));
        FindClose(h);

        std::sort(entries.begin(), entries.end(),
                  [](const ProjEntry& a, const ProjEntry& b) {
            return CompareFileTime(&a.mtime, &b.mtime) > 0;
        });
        for (auto& e : entries) out.push_back(std::move(e.p));
        return out;
    }

    // 특정 목표/프로젝트의 chats/chat_*.json 만 글롭 (legacy 평면 미포함).
    // ListSessions() 가 default 한정인 자리의 *모든 위계* 확장. previewLabel 도 채움.
    static std::vector<SessionMeta> ListChatsIn(const std::string& goalId,
                                                const std::string& projectId) {
        std::vector<SessionMeta> out;
        if (!EnsureChatDatabase()) return out;
        Json::Value p;
        p["goal_id"] = goalId;
        p["project_id"] = projectId.empty() ? CProject::kDefaultId : projectId;
        try {
            CDatabase::Instance().Fetch(
                "ListGoalChats",
                "SELECT key, title, summary, progress, last_active_iso, "
                "COALESCE(updated_at_gmt, CAST(strftime('%s', updated_at) AS INTEGER)) AS updated_at_gmt "
                "FROM ChatSessions "
                "WHERE scope='goal' AND goal_id=:goal_id AND project_id=:project_id "
                "ORDER BY COALESCE(updated_at_gmt, CAST(strftime('%s', updated_at) AS INTEGER)) DESC;",
                p,
                [&](const Json::Value& row) {
                    SessionMeta m;
                    m.key = row.get("key", "").asString();
                    m.title = row.get("title", "").asString();
                    m.summary = row.get("summary", "").asString();
                    m.progress = row.get("progress", 0).asInt();
                    m.lastActiveIso = row.get("last_active_iso", "").asString();
                    m.mtime = FileTimeFromUnix(JsonTimestamp(row, "updated_at_gmt"));
                    m.providerLabel = BuildProviderLabelForChat(m.key);
                    std::wstring text = BuildPreviewLabelFromMeta(m);
                    std::wstring time = FormatMtimeShort(m.mtime);
                    m.previewLabel = !text.empty() && !time.empty() ? (text + L" · " + time)
                                   : !text.empty()                  ? text
                                                                     : time;
                    out.push_back(std::move(m));
                    return true;
                }
            );
        } catch (...) {}
        return out;
    }

    // 현재 채팅의 블록만 삭제 (세션 메타는 유지). 뷰 클리어 후 호출.
    static bool ClearBlocks(const std::string& key) {
        if (!EnsureChatDatabase()) return false;
        std::string clean = SanitizeKey(key.empty() ? std::string("default") : key);
        Json::Value p;
        p["key"] = clean;
        try {
            CDatabase::Instance().Execute("BEGIN IMMEDIATE;");
            CDatabase::Instance().Run("ClearChatBlocks",
                "DELETE FROM ChatBlocks WHERE chat_key=:key;", p);
            Json::Int64 nowGmt = (Json::Int64)TimestampNowGmt();
            Json::Value up;
            up["key"] = clean;
            up["now"] = nowGmt;
            CDatabase::Instance().Run("ClearChatBlocksUpdateMeta",
                "UPDATE ChatSessions SET updated_at_gmt=:now, last_active_at=:now "
                "WHERE key=:key;", up);
            CDatabase::Instance().Execute("COMMIT;");
            return true;
        } catch (...) {
            try { CDatabase::Instance().Execute("ROLLBACK;"); } catch (...) {}
            return false;
        }
    }

    // 키에 해당하는 세션 파일 삭제. 없으면 무해. .tmp 파편도 같이.
    static bool Delete(const std::string& key) {
        std::string clean = SanitizeKey(key.empty() ? std::string("default") : key);
        if (EnsureChatDatabase()) {
            Json::Value p;
            p["key"] = clean;
            try {
                CDatabase::Instance().Run("DeleteChatSession", "DELETE FROM ChatSessions WHERE key=:key;", p);
            } catch (...) {}
        }
        AttachmentStore::DeleteSession(Utf8ToWide(clean.c_str(), (int)clean.size()));
        std::wstring path = SessionFilePath(clean);
        if (!path.empty()) {
            std::wstring tmp = path + L".tmp";
            DeleteFileW(tmp.c_str());
            DeleteFileW(path.c_str());
        }
        return true;
    }

    // 빈 세션 파일 일괄 삭제. 크기 < 200 byte 면 빈 세션으로 간주(빈 세션 JSON 은 ~60-100 byte,
    // 짧은 메시지 한 쌍 들어있어도 250 byte 넘음 — 안전한 임계값). 반환값: 삭제된 파일 수.
    static int PurgeEmptySessions() {
        wchar_t appdata[MAX_PATH] = L"";
        if (GetEnvironmentVariableW(L"APPDATA", appdata, MAX_PATH) == 0) return 0;
        std::wstring dir = std::wstring(appdata) + L"\\OrangeCode";
        std::wstring pattern = dir + L"\\session_*.json";

        int deleted = 0;
        WIN32_FIND_DATAW fd{};
        HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) return 0;
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            if (fd.nFileSizeHigh != 0)        continue;  // GB 급은 빈 것 아님
            if (fd.nFileSizeLow >= 200)       continue;  // 임계 초과 — 보존
            std::wstring path = dir + L"\\" + fd.cFileName;
            if (DeleteFileW(path.c_str())) deleted++;
        } while (FindNextFileW(h, &fd));
        FindClose(h);
        return deleted;
    }

    static ChatSession Load(const std::string& key) {
        ChatSession session;
        if (!EnsureChatDatabase()) return session;
        std::string clean = SanitizeKey(key.empty() ? std::string("default") : key);
        Json::Value p;
        p["key"] = clean;
        try {
            CDatabase::Instance().Fetch(
                "LoadChatSession",
                "SELECT session_id, title, summary, progress, last_active_iso "
                "FROM ChatSessions WHERE key=:key;",
                p,
                [&](const Json::Value& row) {
                    std::string sid = row.get("session_id", "").asString();
                    session.sessionId = IsLikelyUuid(sid) ? sid : std::string();
                    session.title = row.get("title", "").asString();
                    session.summary = row.get("summary", "").asString();
                    session.progress = row.get("progress", 0).asInt();
                    session.lastActiveIso = row.get("last_active_iso", "").asString();
                    return false;
                }
            );
            CDatabase::Instance().Fetch(
                "LoadChatBlocks",
                "SELECT seq, role, text FROM ChatBlocks WHERE chat_key=:key ORDER BY seq ASC;",
                p,
                [&](const Json::Value& row) {
                    ChatBlock cb;
                    cb.seq = row.get("seq", -1).asInt();
                    std::string r = row.get("role", "").asString();
                    std::string t = row.get("text", "").asString();
                    cb.role = Utf8ToWide(r.c_str(), (int)r.size());
                    cb.text = Utf8ToWide(t.c_str(), (int)t.size());
                    if (cb.text.find(L"--resume requires a valid session ID")
                        != std::wstring::npos) {
                        return true;
                    }
                    session.blocks.push_back(std::move(cb));
                    return true;
                }
            );
        } catch (...) {
            return ChatSession{};
        }
        session.totalBlockCount = (int)session.blocks.size();
        session.historyPartial = false;
        if (session.progress < 0)   session.progress = 0;
        if (session.progress > 100) session.progress = 100;
        return session;
    }

    static ChatSession LoadRecent(const std::string& key, int limit) {
        ChatSession session;
        if (!EnsureChatDatabase()) return session;
        if (limit <= 0) limit = 120;
        std::string clean = SanitizeKey(key.empty() ? std::string("default") : key);
        Json::Value p;
        p["key"] = clean;
        p["limit"] = limit;
        try {
            CDatabase::Instance().Fetch(
                "LoadRecentChatSessionMeta",
                "SELECT session_id, title, summary, progress, last_active_iso "
                "FROM ChatSessions WHERE key=:key;",
                p,
                [&](const Json::Value& row) {
                    std::string sid = row.get("session_id", "").asString();
                    session.sessionId = IsLikelyUuid(sid) ? sid : std::string();
                    session.title = row.get("title", "").asString();
                    session.summary = row.get("summary", "").asString();
                    session.progress = row.get("progress", 0).asInt();
                    session.lastActiveIso = row.get("last_active_iso", "").asString();
                    return false;
                }
            );
            CDatabase::Instance().Fetch(
                "CountChatBlocks",
                "SELECT COUNT(*) AS count FROM ChatBlocks WHERE chat_key=:key;",
                p,
                [&](const Json::Value& row) {
                    session.totalBlockCount = row.get("count", 0).asInt();
                    return false;
                }
            );
            CDatabase::Instance().Fetch(
                "LoadRecentChatBlocks",
                "SELECT seq, role, text FROM ("
                "  SELECT seq, role, text FROM ChatBlocks "
                "  WHERE chat_key=:key ORDER BY seq DESC LIMIT :limit"
                ") ORDER BY seq ASC;",
                p,
                [&](const Json::Value& row) {
                    ChatBlock cb;
                    cb.seq = row.get("seq", -1).asInt();
                    std::string r = row.get("role", "").asString();
                    std::string t = row.get("text", "").asString();
                    cb.role = Utf8ToWide(r.c_str(), (int)r.size());
                    cb.text = Utf8ToWide(t.c_str(), (int)t.size());
                    if (cb.text.find(L"--resume requires a valid session ID")
                        != std::wstring::npos) {
                        return true;
                    }
                    session.blocks.push_back(std::move(cb));
                    return true;
                }
            );
        } catch (...) {
            return ChatSession{};
        }
        session.historyPartial = session.totalBlockCount > (int)session.blocks.size();
        if (session.progress < 0)   session.progress = 0;
        if (session.progress > 100) session.progress = 100;
        return session;
    }

    static std::vector<ChatBlock> LoadBlocksBefore(const std::string& key, int beforeSeq, int limit) {
        std::vector<ChatBlock> blocks;
        if (!EnsureChatDatabase() || beforeSeq <= 0) return blocks;
        if (limit <= 0) limit = 80;
        std::string clean = SanitizeKey(key.empty() ? std::string("default") : key);
        Json::Value p;
        p["key"] = clean;
        p["before_seq"] = beforeSeq;
        p["limit"] = limit;
        try {
            CDatabase::Instance().Fetch(
                "LoadChatBlocksBefore",
                "SELECT seq, role, text FROM ("
                "  SELECT seq, role, text FROM ChatBlocks "
                "  WHERE chat_key=:key AND seq < :before_seq "
                "  ORDER BY seq DESC LIMIT :limit"
                ") ORDER BY seq ASC;",
                p,
                [&](const Json::Value& row) {
                    ChatBlock cb;
                    cb.seq = row.get("seq", -1).asInt();
                    std::string r = row.get("role", "").asString();
                    std::string t = row.get("text", "").asString();
                    cb.role = Utf8ToWide(r.c_str(), (int)r.size());
                    cb.text = Utf8ToWide(t.c_str(), (int)t.size());
                    if (cb.text.find(L"--resume requires a valid session ID")
                        != std::wstring::npos) {
                        return true;
                    }
                    blocks.push_back(std::move(cb));
                    return true;
                }
            );
        } catch (...) {
            blocks.clear();
        }
        return blocks;
    }

    // ── 윈도우 상태 저장/복원 (대화 세션과 별도 파일: window.json) ──

    static std::wstring WindowStatePath() {
        wchar_t appdata[MAX_PATH] = L"";
        if (GetEnvironmentVariableW(L"APPDATA", appdata, MAX_PATH) == 0) {
            return L"";
        }
        std::wstring dir = std::wstring(appdata) + L"\\OrangeCode";
        CreateDirectoryW(dir.c_str(), nullptr);
        return dir + L"\\window.json";
    }

    static bool SaveWindowState(HWND hwnd, int sidebarWidth = 0) {
        if (!hwnd || !IsWindow(hwnd)) return false;
        WINDOWPLACEMENT wp{};
        wp.length = sizeof(wp);
        if (!GetWindowPlacement(hwnd, &wp)) return false;

        Json::Value root;
        root["version"] = 1;
        root["x"]       = (int)wp.rcNormalPosition.left;
        root["y"]       = (int)wp.rcNormalPosition.top;
        root["width"]   = (int)(wp.rcNormalPosition.right  - wp.rcNormalPosition.left);
        root["height"]  = (int)(wp.rcNormalPosition.bottom - wp.rcNormalPosition.top);
        // 최소화 상태로 저장하면 다음 실행이 트레이로 사라진 듯 보임 → normal 로 환원.
        int sc = (wp.showCmd == SW_SHOWMAXIMIZED) ? SW_SHOWMAXIMIZED : SW_SHOWNORMAL;
        root["showCmd"]      = sc;
        if (sidebarWidth > 0) root["sidebarWidth"] = sidebarWidth;

        Json::StreamWriterBuilder w;
        w["indentation"] = "  ";
        std::string content = Json::writeString(w, root);

        std::wstring path = WindowStatePath();
        if (path.empty()) return false;
        std::string utf8path = WideToUtf8(path);
        std::string tmpPath  = utf8path + ".tmp";
        {
            std::ofstream ofs(tmpPath, std::ios::binary | std::ios::trunc);
            if (!ofs.is_open()) return false;
            ofs.write(content.data(), content.size());
            if (!ofs.good()) return false;
        }
        std::wstring wTmp = path + L".tmp";
        ::MoveFileExW(wTmp.c_str(), path.c_str(),
                      MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
        return true;
    }

    static WindowState LoadWindowState() {
        WindowState ws;
        std::wstring path = WindowStatePath();
        if (path.empty()) return ws;
        if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) return ws;

        std::ifstream ifs(WideToUtf8(path), std::ios::binary);
        if (!ifs.is_open()) return ws;
        std::stringstream buf;
        buf << ifs.rdbuf();

        Json::Value             root;
        Json::CharReaderBuilder rb;
        std::string             err;
        std::istringstream      iss(buf.str());
        if (!Json::parseFromStream(rb, iss, &root, &err)) return ws;

        ws.x            = root.get("x", 0).asInt();
        ws.y            = root.get("y", 0).asInt();
        ws.width        = root.get("width", 0).asInt();
        ws.height       = root.get("height", 0).asInt();
        ws.showCmd      = root.get("showCmd", SW_SHOWNORMAL).asInt();
        ws.sidebarWidth = root.get("sidebarWidth", 0).asInt();
        // 최소 합리성 검사: 너무 작은 값이면 무효
        ws.valid = (ws.width >= 200 && ws.height >= 150);
        return ws;
    }

    // ── 목표/프로젝트 위계 통합 (Goal.h) ──
    //
    // 새 채팅 파일 경로 — `goals/<goal>/projects/<project>/chats/chat_<key>.json`.
    // 기존 SessionFilePath 는 *호환 유지* (다음 사이클에서 위계로 전환).
    static std::wstring ChatFilePath(const std::string& goalId,
                                     const std::string& projectId,
                                     const std::string& key)
    {
        std::wstring chatsDir = CProject::ChatsDir(goalId, projectId);
        if (chatsDir.empty()) return {};
        std::string clean = SanitizeKey(key.empty() ? std::string("default") : key);
        return chatsDir + L"\\chat_" +
               Utf8ToWide(clean.c_str(), (int)clean.size()) + L".json";
    }

    // 평면 `session_*.json` 을 default 목표/프로젝트 위계로 *복사* (한 번 실행).
    // 이동이 아니라 복사 — 옛 파일은 그대로 보존(.legacy.bak 부산물 제외).
    // 멱등: 이미 위계 자리에 같은 키 파일 있으면 건너뜀.
    // 반환: 새로 복사된 파일 수.
    static int MigrateLegacySessionsToHierarchy() {
        wchar_t appdata[MAX_PATH] = L"";
        if (GetEnvironmentVariableW(L"APPDATA", appdata, MAX_PATH) == 0) return 0;
        std::wstring dir = std::wstring(appdata) + L"\\OrangeCode";
        std::wstring pattern = dir + L"\\session_*.json";

        // default 목표/프로젝트 보장.
        CGoal::EnsureExists(CGoal::kDefaultId, CGoal::kDefaultTitle);
        CProject::EnsureExists(CGoal::kDefaultId, CProject::kDefaultId,
                               CProject::kDefaultTitle);

        int copied = 0;
        WIN32_FIND_DATAW fd{};
        HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) return 0;
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            std::wstring name = fd.cFileName;  // session_<KEY>.json
            const std::wstring prefix = L"session_";
            const std::wstring suffix = L".json";
            // .legacy.bak 등 부산물 제외.
            if (name.size() <= prefix.size() + suffix.size()) continue;
            if (name.compare(0, prefix.size(), prefix) != 0) continue;
            if (name.compare(name.size() - suffix.size(), suffix.size(), suffix) != 0) continue;

            std::wstring keyW = name.substr(prefix.size(),
                                            name.size() - prefix.size() - suffix.size());
            std::string  key  = WideToUtf8(keyW);
            std::wstring src  = dir + L"\\" + name;
            std::wstring dst  = ChatFilePath(CGoal::kDefaultId, CProject::kDefaultId, key);
            if (dst.empty()) continue;
            // 이미 있으면 건너뜀 (멱등).
            if (GetFileAttributesW(dst.c_str()) != INVALID_FILE_ATTRIBUTES) continue;
            if (CopyFileW(src.c_str(), dst.c_str(), TRUE /*failIfExists*/)) {
                ++copied;
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
        return copied;
    }
};

}  // namespace orange
