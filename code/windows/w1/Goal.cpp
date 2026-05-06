// CGoal / CProject 구현. 디렉토리 위계 + meta.json read/write.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <fstream>
#include <sstream>

#include <json/json.h>

#include "Goal.h"
#include "Utils.h"
#include "Coordination.h"

namespace orange {

const char* CGoal::kDefaultId    = "default";
const char* CGoal::kDefaultTitle = "목표";

const char* CProject::kDefaultId    = "default";
const char* CProject::kDefaultTitle = "프로젝트";

// ── enum ↔ string ──
const char* WorkStatusToString(CWorkStatus s) {
    switch (s) {
        case CWorkStatus::Planning:   return "planning";
        case CWorkStatus::InProgress: return "in_progress";
        case CWorkStatus::Blocked:    return "blocked";
        case CWorkStatus::Paused:     return "paused";
        case CWorkStatus::Done:       return "done";
        case CWorkStatus::Abandoned:  return "abandoned";
    }
    return "planning";
}

CWorkStatus WorkStatusFromString(const std::string& s) {
    if (s == "in_progress") return CWorkStatus::InProgress;
    if (s == "blocked")     return CWorkStatus::Blocked;
    if (s == "paused")      return CWorkStatus::Paused;
    if (s == "done")        return CWorkStatus::Done;
    if (s == "abandoned")   return CWorkStatus::Abandoned;
    return CWorkStatus::Planning;
}

const char* VerdictToString(CVerdict v) {
    switch (v) {
        case CVerdict::None:    return "none";
        case CVerdict::Pass:    return "pass";
        case CVerdict::Partial: return "partial";
        case CVerdict::Weak:    return "weak";
        case CVerdict::Fail:    return "fail";
    }
    return "none";
}

CVerdict VerdictFromString(const std::string& s) {
    if (s == "pass")    return CVerdict::Pass;
    if (s == "partial") return CVerdict::Partial;
    if (s == "weak")    return CVerdict::Weak;
    if (s == "fail")    return CVerdict::Fail;
    return CVerdict::None;
}

namespace {

std::string TimestampNowIso() {
    SYSTEMTIME st;
    GetSystemTime(&st);
    char buf[40];
    sprintf_s(buf, "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ",
              st.wYear, st.wMonth, st.wDay,
              st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    return buf;
}

bool DirExists(const std::wstring& path) {
    DWORD a = GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

bool EnsureDirRec(const std::wstring& path) {
    if (DirExists(path)) return true;
    // 부모 먼저.
    size_t slash = path.find_last_of(L"\\/");
    if (slash != std::wstring::npos) {
        std::wstring parent = path.substr(0, slash);
        if (!parent.empty() && parent != path) EnsureDirRec(parent);
    }
    return CreateDirectoryW(path.c_str(), nullptr) != 0 || DirExists(path);
}

std::wstring AppDataRoot() {
    wchar_t appdata[MAX_PATH] = L"";
    if (GetEnvironmentVariableW(L"APPDATA", appdata, MAX_PATH) == 0) return {};
    std::wstring root = std::wstring(appdata) + L"\\OrangeCode";
    EnsureDirRec(root);
    return root;
}

bool WriteJsonAtomic(const std::wstring& path, const Json::Value& root) {
    Json::StreamWriterBuilder w;
    w["indentation"] = "  ";
    std::string content = Json::writeString(w, root);

    std::string utf8path = WideToUtf8(path);
    std::string tmpPath  = utf8path + ".tmp";
    {
        std::ofstream ofs(tmpPath, std::ios::binary | std::ios::trunc);
        if (!ofs.is_open()) return false;
        ofs.write(content.data(), content.size());
        if (!ofs.good()) return false;
    }
    std::wstring wTmp = path + L".tmp";
    return ::MoveFileExW(wTmp.c_str(), path.c_str(),
                        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
}

bool ReadJson(const std::wstring& path, Json::Value& out) {
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) return false;
    std::ifstream ifs(WideToUtf8(path), std::ios::binary);
    if (!ifs.is_open()) return false;
    std::stringstream buf;
    buf << ifs.rdbuf();
    Json::CharReaderBuilder rb;
    std::string             err;
    std::istringstream      iss(buf.str());
    return Json::parseFromStream(rb, iss, &out, &err);
}

}  // namespace

std::string CGoal::SanitizeId(const std::string& id) {
    std::string out;
    out.reserve(id.size());
    for (char c : id) {
        const bool safe =
            (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.';
        out += safe ? c : '_';
    }
    if (out.empty()) out = kDefaultId;
    return out;
}

std::wstring CGoal::Dir(const std::string& id) {
    std::wstring root = AppDataRoot();
    if (root.empty()) return {};
    std::string safe = SanitizeId(id);
    std::wstring d = root + L"\\goals\\" +
        Utf8ToWide(safe.c_str(), (int)safe.size());
    EnsureDirRec(d);
    return d;
}

std::wstring CGoal::MetaPath(const std::string& id) {
    std::wstring d = Dir(id);
    return d.empty() ? L"" : (d + L"\\meta.json");
}

std::wstring CGoal::ContextPath(const std::string& id) {
    std::wstring d = Dir(id);
    return d.empty() ? L"" : (d + L"\\context.md");
}

bool CGoal::EnsureExists(const std::string& id, const std::string& title) {
    std::wstring meta = MetaPath(id);
    if (meta.empty()) return false;
    if (GetFileAttributesW(meta.c_str()) != INVALID_FILE_ATTRIBUTES) return true;
    CGoal g;
    g.id            = SanitizeId(id);
    g.title         = title;
    g.createdAtIso  = TimestampNowIso();
    g.lastActiveIso = g.createdAtIso;
    return Save(g);
}

bool CGoal::Save(const CGoal& g) {
    Json::Value root;
    root["version"]         = 2;  // v2 = 메타 필드 추가 (purpose/criteria/verdict/progress/status)
    root["kind"]            = "goal";
    root["id"]              = g.id;
    root["title"]           = g.title;
    root["created_at"]      = g.createdAtIso;
    root["last_active"]     = g.lastActiveIso;
    root["purpose"]         = g.purpose;
    root["criteria"]        = g.criteria;
    root["verdict"]         = VerdictToString(g.verdict);
    root["progress"]        = g.progress;
    root["status"]          = WorkStatusToString(g.status);
    // 매 Save 시점에 last_evaluated 를 현재 시각으로 자동 갱신 — 사용자가 메타를
    // 편집한 시점이 곧 *마지막 평가* 자리입니다 (자동 진척 계산이 들어오기 전 단순화).
    root["last_evaluated"]  = CCoordination::TimestampNow();
    return WriteJsonAtomic(MetaPath(g.id), root);
}

CGoal CGoal::Load(const std::string& id) {
    CGoal g;
    Json::Value root;
    if (!ReadJson(MetaPath(id), root)) return g;
    g.id               = root.get("id", id).asString();
    g.title            = root.get("title", "").asString();
    g.createdAtIso     = root.get("created_at", "").asString();
    g.lastActiveIso    = root.get("last_active", "").asString();
    // v2 메타 — v1 파일에서 읽으면 기본값으로 채워짐 (호환).
    g.purpose          = root.get("purpose", "").asString();
    g.criteria         = root.get("criteria", "").asString();
    g.verdict          = VerdictFromString(root.get("verdict", "none").asString());
    g.progress         = root.get("progress", 0).asInt();
    g.status           = WorkStatusFromString(root.get("status", "planning").asString());
    g.lastEvaluatedIso = root.get("last_evaluated", "").asString();
    return g;
}

// ── CProject ──────────────────────────────────────────────

std::wstring CProject::Dir(const std::string& goalId, const std::string& id) {
    std::wstring goalDir = CGoal::Dir(goalId);
    if (goalDir.empty()) return {};
    std::string safe = CGoal::SanitizeId(id);
    std::wstring d = goalDir + L"\\projects\\" +
        Utf8ToWide(safe.c_str(), (int)safe.size());
    EnsureDirRec(d);
    return d;
}

std::wstring CProject::MetaPath(const std::string& goalId, const std::string& id) {
    std::wstring d = Dir(goalId, id);
    return d.empty() ? L"" : (d + L"\\meta.json");
}

std::wstring CProject::ContextPath(const std::string& goalId, const std::string& id) {
    std::wstring d = Dir(goalId, id);
    return d.empty() ? L"" : (d + L"\\context.md");
}

std::wstring CProject::ChatsDir(const std::string& goalId, const std::string& id) {
    std::wstring d = Dir(goalId, id);
    if (d.empty()) return {};
    std::wstring chats = d + L"\\chats";
    EnsureDirRec(chats);
    return chats;
}

bool CProject::EnsureExists(const std::string& goalId, const std::string& id,
                            const std::string& title)
{
    std::wstring meta = MetaPath(goalId, id);
    if (meta.empty()) return false;
    if (GetFileAttributesW(meta.c_str()) != INVALID_FILE_ATTRIBUTES) return true;
    CProject p;
    p.id            = CGoal::SanitizeId(id);
    p.goalId        = CGoal::SanitizeId(goalId);
    p.title         = title;
    p.createdAtIso  = TimestampNowIso();
    p.lastActiveIso = p.createdAtIso;
    return Save(p);
}

bool CProject::Save(const CProject& p) {
    Json::Value root;
    root["version"]        = 2;
    root["kind"]           = "project";
    root["id"]             = p.id;
    root["goal_id"]        = p.goalId;
    root["title"]          = p.title;
    root["created_at"]     = p.createdAtIso;
    root["last_active"]    = p.lastActiveIso;
    root["purpose"]        = p.purpose;
    root["criteria"]       = p.criteria;
    root["verdict"]        = VerdictToString(p.verdict);
    root["progress"]       = p.progress;
    root["status"]         = WorkStatusToString(p.status);
    root["last_evaluated"] = CCoordination::TimestampNow();
    return WriteJsonAtomic(MetaPath(p.goalId, p.id), root);
}

CProject CProject::Load(const std::string& goalId, const std::string& id) {
    CProject p;
    Json::Value root;
    if (!ReadJson(MetaPath(goalId, id), root)) return p;
    p.id               = root.get("id", id).asString();
    p.goalId           = root.get("goal_id", goalId).asString();
    p.title            = root.get("title", "").asString();
    p.createdAtIso     = root.get("created_at", "").asString();
    p.lastActiveIso    = root.get("last_active", "").asString();
    p.purpose          = root.get("purpose", "").asString();
    p.criteria         = root.get("criteria", "").asString();
    p.verdict          = VerdictFromString(root.get("verdict", "none").asString());
    p.progress         = root.get("progress", 0).asInt();
    p.status           = WorkStatusFromString(root.get("status", "planning").asString());
    p.lastEvaluatedIso = root.get("last_evaluated", "").asString();
    return p;
}

}  // namespace orange
