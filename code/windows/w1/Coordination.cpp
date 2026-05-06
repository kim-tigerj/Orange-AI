#include "Coordination.h"
#include "Utils.h"
#include <iostream>
#include <sstream>
#include <iomanip>
#include <ctime>
#include <algorithm>
#include <cwctype>

namespace orange {

namespace {

std::wstring LowerWide(std::wstring text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](wchar_t ch) { return (wchar_t)towlower(ch); });
    return text;
}

std::wstring TrimLeftWide(std::wstring text) {
    while (!text.empty() && iswspace(text.front())) text.erase(text.begin());
    return text;
}

std::wstring RelayProviderToken(const std::wstring& text, size_t start) {
    size_t pos = start;
    while (pos < text.size() && iswspace(text[pos])) ++pos;
    size_t end = pos;
    while (end < text.size() &&
           !iswspace(text[end]) &&
           text[end] != L':' &&
           text[end] != L'\xFF1A' &&
           text[end] != L',' &&
           text[end] != L'.') {
        ++end;
    }
    return LowerWide(text.substr(pos, end - pos));
}

bool ProviderTokenMatches(const std::wstring& token, const wchar_t* english, const wchar_t* korean1, const wchar_t* korean2 = L"") {
    return token == english ||
           token == korean1 ||
           (korean2[0] != L'\0' && token == korean2);
}

bool ProviderFromRelayToken(const std::wstring& token, std::string& targetProvider) {
    if (ProviderTokenMatches(token, L"claude", L"\xD074\xB85C\xB4DC")) {
        targetProvider = "claude";
        return true;
    }
    if (ProviderTokenMatches(token, L"gemini", L"\xC81C\xBBF8\xB2C8", L"\xC824\xBBF8\xB2C8")) {
        targetProvider = "gemini";
        return true;
    }
    if (ProviderTokenMatches(token, L"codex", L"\xCF54\xB371\xC2A4")) {
        targetProvider = "codex";
        return true;
    }
    return false;
}
} // namespace

void CCoordination::Initialize(const std::wstring& dbPath) {
    CDatabase::Instance().Open(dbPath);
}

std::string CCoordination::TimestampNow() {
    auto now = std::time(nullptr);
    std::tm tm;
    gmtime_s(&tm, &now);
    std::ostringstream ss;
    ss << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
    return ss.str();
}

std::string CCoordination::GetMyActorId() {
    char role[64] = "";
    GetEnvironmentVariableA("ORANGE_CODE_ROLE", role, sizeof(role));
    std::string sRole = role[0] ? role : "manager";
    return sRole + ":" + std::to_string(GetCurrentProcessId());
}

// --- Workflow ---

bool CCoordination::UpsertWorkflow(const WorkflowEntry& e) {
    std::string typeStr;
    switch (e.type) {
        case WorkflowType::Goal:    typeStr = "goal"; break;
        case WorkflowType::Project: typeStr = "project"; break;
        case WorkflowType::Chat:    typeStr = "chat"; break;
    }

    Json::Value p, meta;
    p["key"]           = e.key;
    p["type"]          = typeStr;
    p["parent_key"]    = e.parentKey;
    p["title"]         = e.title;
    p["status"]        = e.status;
    p["progress"]      = e.progress;
    p["supervisor_id"] = e.supervisorId;
    p["manager_id"]    = e.managerId;
    p["worker_id"]     = e.workerId;

    meta["startTime"] = e.startTime;
    meta["endTime"]   = e.endTime;
    p["meta"] = meta;

    static const std::string sql =
        "INSERT INTO Workflow (key, type, parent_key, title, status, progress, "
        "  supervisor_id, manager_id, worker_id, meta, updated_at) "
        "VALUES (:key, :type, :parent_key, :title, :status, :progress, "
        "  :supervisor_id, :manager_id, :worker_id, :meta, CURRENT_TIMESTAMP) "
        "ON CONFLICT(key) DO UPDATE SET "
        "  type=excluded.type, parent_key=excluded.parent_key, title=excluded.title, "
        "  status=excluded.status, progress=excluded.progress, "
        "  supervisor_id=excluded.supervisor_id, manager_id=excluded.manager_id, "
        "  worker_id=excluded.worker_id, meta=excluded.meta, "
        "  updated_at=CURRENT_TIMESTAMP;";

    try { CDatabase::Instance().Run("UpsertWorkflow", sql, p); return true; } catch (...) { return false; }
}

CCoordination::WorkflowEntry CCoordination::GetWorkflow(const std::string& key) {
    WorkflowEntry e{};
    Json::Value p;
    p["key"] = key;
    static const std::string sql =
        "SELECT key, type, parent_key, title, status, progress, "
        "  supervisor_id, manager_id, worker_id, meta, updated_at "
        "FROM Workflow WHERE key=:key;";

    try {
        CDatabase::Instance().Fetch("GetWorkflow", sql, p, [&](const Json::Value& row) {
            e.key        = row["key"].asString();
            e.parentKey  = row["parent_key"].asString();
            e.title      = row["title"].asString();
            e.status     = row["status"].asString();
            e.progress   = row["progress"].asInt();
            e.supervisorId = row["supervisor_id"].asString();
            e.managerId    = row["manager_id"].asString();
            e.workerId     = row["worker_id"].asString();
            e.lastActive   = row["updated_at"].asString();

            std::string t = row["type"].asString();
            if      (t == "chat")    e.type = WorkflowType::Chat;
            else if (t == "project") e.type = WorkflowType::Project;
            else                     e.type = WorkflowType::Goal;

            Json::Value meta;
            Json::Reader().parse(row["meta"].asString(), meta);
            e.startTime = meta["startTime"].asString();
            e.endTime   = meta["endTime"].asString();
            return false;
        });
    } catch (...) {}
    return e;
}

std::vector<CCoordination::WorkflowEntry> CCoordination::ListWorkflows(const std::string& parentKey) {
    std::vector<WorkflowEntry> entries;
    Json::Value p;
    std::string sql =
        "SELECT key, type, parent_key, title, status, progress, updated_at FROM Workflow ";
    if (!parentKey.empty()) {
        sql += "WHERE parent_key=:parent ";
        p["parent"] = parentKey;
    }
    sql += "ORDER BY updated_at DESC;";

    try {
        CDatabase::Instance().Fetch("ListWorkflows", sql, p, [&](const Json::Value& row) {
            WorkflowEntry e;
            e.key      = row["key"].asString();
            e.title    = row["title"].asString();
            e.status   = row["status"].asString();
            e.progress = row["progress"].asInt();
            e.lastActive = row["updated_at"].asString();

            std::string t = row["type"].asString();
            if      (t == "chat")    e.type = WorkflowType::Chat;
            else if (t == "project") e.type = WorkflowType::Project;
            else                     e.type = WorkflowType::Goal;

            entries.push_back(std::move(e));
            return true;
        });
    } catch (...) {}
    return entries;
}

bool CCoordination::UpdateWorkflowStatus(const std::string& key, const std::string& status, int progress) {
    Json::Value p;
    p["key"]      = key;
    p["status"]   = status;
    p["progress"] = progress;
    static const std::string sql =
        "UPDATE Workflow SET status=:status, progress=:progress, "
        "  updated_at=CURRENT_TIMESTAMP WHERE key=:key;";
    try { CDatabase::Instance().Run("UpdateWorkflowStatus", sql, p); return true; } catch (...) { return false; }
}

bool CCoordination::AssignWorkflowRoles(const std::string& key,
                                         const std::string& supervisor,
                                         const std::string& manager,
                                         const std::string& worker) {
    WorkflowEntry e = GetWorkflow(key);
    if (e.key.empty()) return false;
    e.supervisorId = supervisor;
    e.managerId    = manager;
    e.workerId     = worker;
    return UpsertWorkflow(e);
}

// --- ActorActivity ---

bool CCoordination::SetStatus(const ActorStatus& s) {
    Json::Value p, payload;
    p["actor_id"]     = s.actorId;
    p["kind"]         = s.kind;
    p["pid"]          = (Json::UInt)s.pid;
    p["session_key"]  = s.sessionId;
    p["workflow_key"] = s.currentWorkflowKey;
    p["intent"]       = s.intent.empty() ? s.currentIntent : s.intent;

    payload["turnInProgress"] = s.turnInProgress;
    payload["turnPrompt"]     = s.turnPrompt;
    payload["eventsCount"]    = s.eventsCount;
    p["payload"] = payload;

    static const std::string sql =
        "INSERT INTO ActorActivity (actor_id, kind, pid, session_key, workflow_key, intent, payload, updated_at) "
        "VALUES (:actor_id, :kind, :pid, :session_key, :workflow_key, :intent, :payload, "
        "  strftime('%Y-%m-%dT%H:%M:%SZ', 'now')) "
        "ON CONFLICT(actor_id) DO UPDATE SET "
        "  kind=excluded.kind, pid=excluded.pid, session_key=excluded.session_key, "
        "  workflow_key=excluded.workflow_key, intent=excluded.intent, "
        "  payload=excluded.payload, updated_at=strftime('%Y-%m-%dT%H:%M:%SZ', 'now');";

    try { CDatabase::Instance().Run("SetStatus", sql, p); return true; } catch (...) { return false; }
}

std::vector<CCoordination::ActorStatus> CCoordination::ListActors(int staleSeconds) {
    std::vector<ActorStatus> actors;
    Json::Value p;
    p["stale"] = "-" + std::to_string(staleSeconds) + " seconds";
    static const std::string sql =
        "SELECT actor_id, kind, pid, session_key, workflow_key, intent, payload, updated_at "
        "FROM ActorActivity "
        "WHERE updated_at > strftime('%Y-%m-%dT%H:%M:%SZ', 'now', :stale) "
        "ORDER BY updated_at DESC;";

    try {
        CDatabase::Instance().Fetch("ListActors", sql, p, [&](const Json::Value& row) {
            ActorStatus s;
            s.actorId           = row["actor_id"].asString();
            s.kind              = row["kind"].asString();
            s.pid               = row["pid"].asUInt();
            s.sessionId         = row["session_key"].asString();
            s.currentWorkflowKey = row["workflow_key"].asString();
            s.intent            = row["intent"].asString();
            s.currentIntent     = s.intent;
            s.lastSeen          = row["updated_at"].asString();
            s.lastSeenIso       = s.lastSeen;

            Json::Value payload;
            Json::Reader().parse(row["payload"].asString(), payload);
            s.turnInProgress = payload["turnInProgress"].asBool();
            s.turnPrompt     = payload["turnPrompt"].asString();
            s.eventsCount    = payload["eventsCount"].asInt();

            actors.push_back(std::move(s));
            return true;
        });
    } catch (...) {}
    return actors;
}

bool CCoordination::RemoveActor(const std::string& actorId) {
    Json::Value p;
    p["actor_id"] = actorId;
    static const std::string sql = "DELETE FROM ActorActivity WHERE actor_id=:actor_id;";
    try { CDatabase::Instance().Run("RemoveActor", sql, p); return true; } catch (...) { return false; }
}

std::string CCoordination::GetActorIntent(const std::string& actorId) {
    std::string intent;
    Json::Value p;
    p["actor_id"] = actorId;
    static const std::string sql = "SELECT intent FROM ActorActivity WHERE actor_id=:actor_id;";
    try {
        CDatabase::Instance().Fetch("GetActorIntent", sql, p, [&](const Json::Value& row) {
            intent = row["intent"].asString();
            return false;
        });
    } catch (...) {}
    return intent;
}

std::vector<CCoordination::Conflict> CCoordination::DetectConflicts(const std::string& /*selfActorId*/, int /*staleSeconds*/) {
    return {};
}

// --- Reviews ---

bool CCoordination::SubmitReview(const ReviewEntry& r) {
    Json::Value p;
    p["workflow_key"]    = r.workflowKey;
    p["reviewer_id"]     = r.reviewerId;
    p["functional"]      = r.functional;
    p["implementation"]  = r.implementation;
    p["relation"]        = r.relation;
    p["impact"]          = r.impact;
    p["comment"]         = r.comment;

    static const std::string sql =
        "INSERT INTO Reviews (workflow_key, reviewer_id, functional, implementation, "
        "  relation, impact, comment) "
        "VALUES (:workflow_key, :reviewer_id, :functional, :implementation, "
        "  :relation, :impact, :comment);";

    try { CDatabase::Instance().Run("SubmitReview", sql, p); return true; } catch (...) { return false; }
}

std::vector<CCoordination::ReviewEntry> CCoordination::GetReviews(const std::string& workflowKey) {
    std::vector<ReviewEntry> reviews;
    Json::Value p;
    p["workflow_key"] = workflowKey;
    static const std::string sql =
        "SELECT reviewer_id, functional, implementation, relation, impact, comment "
        "FROM Reviews WHERE workflow_key=:workflow_key ORDER BY created_at DESC;";

    try {
        CDatabase::Instance().Fetch("GetReviews", sql, p, [&](const Json::Value& row) {
            ReviewEntry r;
            r.workflowKey     = workflowKey;
            r.reviewerId      = row["reviewer_id"].asString();
            r.functional      = row["functional"].asString();
            r.implementation  = row["implementation"].asString();
            r.relation        = row["relation"].asString();
            r.impact          = row["impact"].asString();
            r.comment         = row["comment"].asString();
            reviews.push_back(std::move(r));
            return true;
        });
    } catch (...) {}
    return reviews;
}

bool CCoordination::AppendMessage(const std::string& /*from*/, const std::string& /*to*/,
                                   const std::string& /*title*/, const std::string& /*body*/) {
    return true;
}

bool CCoordination::TryParseProviderRelayCommand(const std::wstring& text, std::string& targetProvider) {
    targetProvider.clear();
    size_t lineStart = 0;
    while (lineStart <= text.size()) {
        size_t lineEnd = text.find(L'\n', lineStart);
        std::wstring line = text.substr(lineStart, lineEnd == std::wstring::npos ? std::wstring::npos : lineEnd - lineStart);
        if (!line.empty() && line.back() == L'\r') line.pop_back();
        line = TrimLeftWide(line);
        std::wstring lower = LowerWide(line);

        size_t tokenStart = std::wstring::npos;
        const std::wstring commands[] = {L"/relay", L"/ask", L"/\xC804\xB2EC", L"/\xD638\xCD9C"};
        for (const auto& command : commands) {
            if (lower.rfind(command, 0) == 0 &&
                (lower.size() == command.size() || iswspace(lower[command.size()]))) {
                tokenStart = command.size();
                break;
            }
        }
        if (tokenStart == std::wstring::npos && lower.rfind(L"@", 0) == 0) {
            tokenStart = 1;
        }

        if (tokenStart != std::wstring::npos) {
            std::wstring token = RelayProviderToken(lower, tokenStart);
            if (ProviderFromRelayToken(token, targetProvider)) {
                return true;
            }
        }

        if (lineEnd == std::wstring::npos) break;
        lineStart = lineEnd + 1;
    }
    return false;
}

// --- TraceLog (audit / symbol index) ---

bool CCoordination::LogOperation(const std::string& actorId, const std::string& workflowKey,
                                  const std::string& logType, const std::string& content) {
    Json::Value p, payload;
    p["actor"]   = actorId;
    p["intent"]  = logType;
    p["command"] = workflowKey;
    payload["content"] = content;
    p["payload"] = payload;

    static const std::string sql =
        "INSERT INTO TraceLog (parent_seq, run_uid, actor, intent, command, payload) "
        "VALUES (:parent_seq, :run_uid, :actor, :intent, :command, :payload);";

    try { CDatabase::Instance().Run("LogOperation", sql, p); return true; } catch (...) { return false; }
}

bool CCoordination::IndexSymbol(const std::string& filePath, const std::string& symbolName,
                                 const std::string& kind, int lineStart, int lineEnd,
                                 const std::string& signature) {
    Json::Value p, payload;
    p["actor"]   = GetMyActorId();
    p["intent"]  = "index_symbol";
    p["command"] = symbolName;
    payload["file"]  = filePath;
    payload["kind"]  = kind;
    payload["range"] = std::to_string(lineStart) + "-" + std::to_string(lineEnd);
    payload["sig"]   = signature;
    p["payload"] = payload;

    static const std::string sql =
        "INSERT INTO TraceLog (parent_seq, run_uid, actor, intent, command, payload) "
        "VALUES (:parent_seq, :run_uid, :actor, :intent, :command, :payload);";

    try { CDatabase::Instance().Run("IndexSymbol", sql, p); return true; } catch (...) { return false; }
}

} // namespace orange
