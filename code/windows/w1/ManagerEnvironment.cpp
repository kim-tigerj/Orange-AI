#include "ManagerEnvironment.h"
#include "AttachmentStore.h"
#include "AppSettings.h"
#include "Database.h"
#include "Persistence.h"

#include <windows.h>
#include <cstring>
#include <fstream>
#include <sstream>

namespace orange {

namespace {

std::wstring GetEnv(const wchar_t* name) {
    wchar_t value[32767] = L"";
    DWORD n = GetEnvironmentVariableW(name, value, static_cast<DWORD>(std::size(value)));
    if (n == 0 || n >= std::size(value)) return {};
    return value;
}

bool FileExists(const std::wstring& path) {
    DWORD attr = GetFileAttributesW(path.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

void WriteUtf8IfMissing(const std::wstring& path, const char* body) {
    if (FileExists(path)) return;
    std::ofstream ofs(WideToUtf8(path), std::ios::binary | std::ios::trunc);
    if (!ofs.is_open()) return;
    ofs.write(body, (std::streamsize)strlen(body));
}

std::string ReadUtf8File(const std::wstring& path) {
    if (path.empty()) return {};
    std::ifstream ifs(WideToUtf8(path), std::ios::binary);
    if (!ifs.is_open()) return {};
    std::ostringstream ss;
    ss << ifs.rdbuf();
    return ss.str();
}

std::wstring ProviderLabelFromId(const std::string& provider) {
    if (provider == "claude") return L"Claude";
    if (provider == "gemini") return L"Gemini";
    if (provider == "codex") return L"Codex";
    return L"Claude";
}

bool EnsurePromptDatabase() {
    if (!Persistence::EnsureChatDatabase()) return false;
    try {
        CDatabase::Instance().Execute(
            "CREATE TABLE IF NOT EXISTS prompt ("
            "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
            "  scope TEXT NOT NULL DEFAULT 'global',"
            "  key TEXT NOT NULL,"
            "  priority INTEGER NOT NULL DEFAULT 100,"
            "  active INTEGER NOT NULL DEFAULT 1,"
            "  source TEXT,"
            "  content TEXT NOT NULL,"
            "  created_at DATETIME DEFAULT CURRENT_TIMESTAMP,"
            "  updated_at DATETIME DEFAULT CURRENT_TIMESTAMP,"
            "  UNIQUE(scope, key)"
            ");"
        );
        CDatabase::Instance().Execute(
            "CREATE TABLE IF NOT EXISTS job ("
            "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
            "  chat_key TEXT NOT NULL,"
            "  title TEXT NOT NULL,"
            "  instruction_type TEXT NOT NULL DEFAULT 'normal',"
            "  priority INTEGER NOT NULL DEFAULT 100,"
            "  status TEXT NOT NULL DEFAULT 'not_started',"
            "  progress INTEGER NOT NULL DEFAULT 0,"
            "  detail_count INTEGER NOT NULL DEFAULT 0,"
            "  started_at DATETIME,"
            "  ended_at DATETIME,"
            "  created_at DATETIME DEFAULT CURRENT_TIMESTAMP,"
            "  updated_at DATETIME DEFAULT CURRENT_TIMESTAMP"
            ");"
        );
        CDatabase::Instance().Execute(
            "CREATE TABLE IF NOT EXISTS job_detail ("
            "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
            "  job_id INTEGER NOT NULL,"
            "  title TEXT NOT NULL,"
            "  status TEXT NOT NULL DEFAULT 'not_started',"
            "  progress INTEGER NOT NULL DEFAULT 0,"
            "  started_at DATETIME,"
            "  ended_at DATETIME,"
            "  created_at DATETIME DEFAULT CURRENT_TIMESTAMP,"
            "  updated_at DATETIME DEFAULT CURRENT_TIMESTAMP,"
            "  FOREIGN KEY(job_id) REFERENCES job(id) ON DELETE CASCADE"
            ");"
        );
        CDatabase::Instance().Execute("CREATE INDEX IF NOT EXISTS idx_prompt_active ON prompt(active, priority DESC);");
        CDatabase::Instance().Execute("CREATE INDEX IF NOT EXISTS idx_job_chat_priority ON job(chat_key, priority DESC, updated_at DESC);");
        CDatabase::Instance().Execute("CREATE INDEX IF NOT EXISTS idx_job_detail_job ON job_detail(job_id);");
        return true;
    } catch (...) {
        return false;
    }
}

void SeedPromptRows() {
    if (!EnsurePromptDatabase()) return;
    auto upsert = [](const char* key, int priority, const char* content) {
        Json::Value p;
        p["scope"] = "global";
        p["key"] = key;
        p["priority"] = priority;
        p["source"] = "orange-default";
        p["content"] = content;
        CDatabase::Instance().Run(
            std::string("UpsertPrompt_") + key,
            "INSERT INTO prompt(scope, key, priority, active, source, content) "
            "VALUES(:scope, :key, :priority, 1, :source, :content) "
            "ON CONFLICT(scope, key) DO UPDATE SET "
            "priority=excluded.priority, active=excluded.active, source=excluded.source, "
            "content=excluded.content, updated_at=CURRENT_TIMESTAMP;",
            p);
    };
    try {
        upsert("db_prompt_priority", 1000,
               "OrangeCode CLI actors must follow rows from the SQLite prompt table first. "
               "After that, read folder markdown files only as supplemental context when the current task requires them.");
        upsert("manager_provider_sync", 950,
               "On every chat start, restart, and UI manager-provider change, OrangeCode tells CLI actors who the appointed manager provider is. "
               "The appointed manager executes; other providers review unless delegated.");
        upsert("provider_authority_source", 940,
               "The appointed manager provider comes only from OrangeCode app settings and ORANGE_CODE_MANAGER_PROVIDER. "
               "If AGENTS.md, CLAUDE.md, MANAGER_HANDOFF.md, chat history, or any other markdown/text names a different provider as manager, treat it as stale supplemental context.");
        upsert("provider_relay_command", 930,
               "Provider-to-provider relay is command-based, not plain text mention-based. "
               "Use /relay <provider>: <message>, /ask <provider>: <message>, /전달 <provider>: <message>, /호출 <provider>: <message>, or @<provider> <message>. "
               "Plain Claude/Gemini/Codex name mentions are discussion text only.");
        upsert("manager_decision_action_closure", 925,
               "After selected providers have given opinions, or when discussion repeats or stalls, the appointed manager must close with a decision and immediately take the next concrete action. "
               "Do not end with one-turn opinions only; reviewers follow the manager's decision as the working direction.");
        upsert("markdown_exchange_scope", 920,
               "If CLI actors must share markdown files, use only OrangeCode-owned exchange paths under ORANGE_CODE_ROOT/reports with the filename pattern "
               "council-<session-key>-<yyyymmddHHMMSS>-<provider>-<topic>.md. Do not create ad hoc shared markdown files elsewhere.");
        upsert("concise_progress_reporting", 910,
               "Progress reports must be concise and evidence-based. Do not repeatedly narrate generic future actions such as fallback build plans or 'I will verify next' unless new evidence changed the plan. "
               "State the actual change, actual verification result, blocker, or requested OrangeCode tool action.");
        upsert("job_briefing", 900,
               "For an existing chat, start from the database job/job_detail briefing instead of a mechanical fixed startup prompt. "
               "Owner instructions use instruction_type=owner and higher priority.");
    } catch (...) {}
}

std::vector<std::wstring> LoadPromptRows() {
    std::vector<std::wstring> rows;
    if (!EnsurePromptDatabase()) return rows;
    SeedPromptRows();
    try {
        CDatabase::Instance().Fetch(
            "LoadActivePromptRows",
            "SELECT scope, key, priority, content FROM prompt "
            "WHERE active=1 ORDER BY priority DESC, id ASC LIMIT 30;",
            Json::Value(),
            [&](const Json::Value& row) {
                std::string scope = row.get("scope", "").asString();
                std::string key = row.get("key", "").asString();
                int priority = row.get("priority", 0).asInt();
                std::string content = row.get("content", "").asString();
                std::wstring line = L"- [" + Utf8ToWide(scope.c_str(), (int)scope.size()) +
                                    L"/" + Utf8ToWide(key.c_str(), (int)key.size()) +
                                    L" p=" + std::to_wstring(priority) + L"] " +
                                    Utf8ToWide(content.c_str(), (int)content.size());
                rows.push_back(std::move(line));
                return true;
            });
    } catch (...) {}
    return rows;
}

std::wstring BuildJobBriefing(const std::wstring& chatKey) {
    if (!EnsurePromptDatabase() || chatKey.empty()) return {};
    Json::Value p;
    std::string key = WideToUtf8(chatKey);
    p["chat_key"] = key;

    int total = 0;
    int done = 0;
    int inProgress = 0;
    int weightedProgress = 0;
    int ownerOpen = 0;
    std::vector<std::wstring> lines;
    try {
        CDatabase::Instance().Fetch(
            "PromptJobBriefing",
            "SELECT id, title, instruction_type, priority, status, progress, detail_count "
            "FROM job WHERE chat_key=:chat_key "
            "ORDER BY priority DESC, updated_at DESC LIMIT 12;",
            p,
            [&](const Json::Value& row) {
                ++total;
                std::string status = row.get("status", "").asString();
                std::string type = row.get("instruction_type", "normal").asString();
                int progress = row.get("progress", 0).asInt();
                if (status == "done" || status == "completed") ++done;
                if (status == "in_progress") ++inProgress;
                if (type == "owner" && status != "done" && status != "completed") ++ownerOpen;
                weightedProgress += progress;

                std::string title = row.get("title", "").asString();
                std::wstring line = L"- p" + std::to_wstring(row.get("priority", 0).asInt()) +
                    L" " + Utf8ToWide(status.c_str(), (int)status.size()) +
                    L" " + std::to_wstring(progress) + L"% " +
                    Utf8ToWide(title.c_str(), (int)title.size()) +
                    L" (details=" + std::to_wstring(row.get("detail_count", 0).asInt()) + L")";
                if (type == "owner") line += L" [owner]";
                lines.push_back(std::move(line));
                return true;
            });
    } catch (...) {}

    if (total == 0) {
        return L"No DB jobs are recorded for this chat yet. If the user gives work, create or update job/job_detail before relying on markdown task files.\n";
    }
    int avg = weightedProgress / total;
    std::wstring out = L"DB job briefing for this chat:\n";
    out += L"- jobs=" + std::to_wstring(total) + L", done=" + std::to_wstring(done) +
           L", in_progress=" + std::to_wstring(inProgress) +
           L", average_progress=" + std::to_wstring(avg) + L"%";
    if (ownerOpen > 0) out += L", open_owner_priority_jobs=" + std::to_wstring(ownerOpen);
    out += L"\n";
    for (const auto& line : lines) out += line + L"\n";
    return out;
}

} // namespace

std::wstring OrangeRootFromModule() {
    wchar_t path[MAX_PATH] = L"";
    DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return {};

    std::wstring p(path);
    for (int i = 0; i < 3; ++i) {
        size_t pos = p.find_last_of(L"\\/");
        if (pos == std::wstring::npos) return {};
        p.resize(pos);
    }
    return p;
}

ManagerEnvironment EnsureManagerEnvironment() {
    std::wstring root = GetEnv(L"ORANGE_CODE_ROOT");
    if (root.empty()) {
        root = OrangeRootFromModule();
        if (!root.empty()) {
            SetEnvironmentVariableW(L"ORANGE_CODE_ROOT", root.c_str());
        }
    }

    SetEnvironmentVariableW(L"ORANGE_CODE_ROLE", L"manager");

    wchar_t pid[32] = L"";
    swprintf_s(pid, L"%lu", GetCurrentProcessId());
    SetEnvironmentVariableW(L"ORANGE_CODE_PID", pid);

    std::wstring sessionKey = GetEnv(L"ORANGE_CODE_SESSION_KEY");
    if (sessionKey.empty()) {
        sessionKey = L"default";
        SetEnvironmentVariableW(L"ORANGE_CODE_SESSION_KEY", sessionKey.c_str());
    }
    EnsureManagerAppContextFiles();
    auto backend = AppSettings::LoadBackendSettings();
    SetEnvironmentVariableW(L"ORANGE_CODE_MANAGER_PROVIDER",
                            ProviderLabelFromId(backend.managerProvider).c_str());
    SeedPromptRows();

    return {
        GetEnv(L"ORANGE_CODE_ROOT"),
        GetEnv(L"ORANGE_CODE_ROLE"),
        GetEnv(L"ORANGE_CODE_PID"),
        GetEnv(L"ORANGE_CODE_SESSION_KEY")
    };
}

std::wstring ManagerAppContextDir() {
    std::wstring root = Persistence::AppDataDir();
    if (root.empty()) return {};
    std::wstring dir = root + L"\\manager_context";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

void EnsureManagerAppContextFiles() {
    std::wstring dir = ManagerAppContextDir();
    if (dir.empty()) return;

    WriteUtf8IfMissing(dir + L"\\APP_CONTEXT.md",
        "# OrangeCode 앱 컨텍스트\n\n"
        "이 디렉터리는 정팀장 앱 실행 전용 컨텍스트 저장소입니다.\n"
        "백엔드는 먼저 이 앱 컨텍스트를 기준으로 응답합니다. 프로젝트 handoff 파일은 기본 컨텍스트가 아닙니다.\n\n"
        "정체성:\n"
        "- 앱 이름: OrangeCode\n"
        "- 사용자 호출명: 정팀장\n"
        "- 코드 역할명: manager\n\n"
        "- Gemini, Claude, Codex는 백엔드 provider일 뿐 정체성이 아닙니다.\n\n"
        "권한 구조:\n"
        "- 최종 의사결정자는 사용자입니다.\n"
        "- 내부 supervisor/review 역할 별칭을 사용자 호칭으로 쓰지 않습니다.\n\n"
        "기본 동작:\n"
        "- 인사, 이름 호출, 짧은 단문에는 빠르게 답합니다.\n"
        "- 단순 호출만으로 프로젝트 작업을 시작하지 않습니다.\n"
        "- 사용자가 정팀장만 부르면 깨어났다고 답하고 다음 지시를 묻습니다.\n");

    WriteUtf8IfMissing(dir + L"\\BACKEND_POLICY.md",
        "# 백엔드 호출 정책\n\n"
        "Gemini, Claude, Codex는 OrangeCode 앱의 백엔드 provider입니다.\n\n"
        "기본 규칙:\n"
        "- 인사, 이름 호출, 짧은 단문에는 도구 없이 텍스트로만 답합니다.\n"
        "- 사용자를 내부 supervisor/review 역할명으로 부르지 않습니다.\n"
        "- 최종 의사결정자는 사용자입니다. 내부 감독/검토 역할은 사용자 호칭이 아닙니다.\n"
        "- 최신 사용자 요청이 프로젝트 작업, handoff 확인, 계획 수립, 이전 맥락 확인을 명시하지 않으면 MANAGER_HANDOFF.md, OH_COUNCIL.md, OH_COUNCIL_STATUS.md, tasks 디렉터리를 읽지 않습니다.\n"
        "- 화면 대화 맥락이 필요할 때만 로컬 Chat API를 사용합니다.\n"
        "- 프롬프트는 작게 유지합니다. 1순위는 협업과 빠른 반응이고, 비용 절감은 그 안에서 가능한 범위로 다룹니다.\n");

    WriteUtf8IfMissing(dir + L"\\SESSION_STATE.md",
        "# 세션 상태\n\n"
        "이 파일은 OrangeCode 실행 중 세션 메모를 위한 앱 전용 파일입니다.\n"
        "앱은 이후 이 파일을 갱신하거나 확장할 수 있습니다.\n\n"
        "현재 기본값:\n"
        "- 정팀장 같은 단독 호출에는 짧은 준비 완료 응답을 합니다.\n"
        "- 프로젝트 파일은 매번 읽지 않고, 필요한 요청이 있을 때만 읽습니다.\n");

    WriteUtf8IfMissing(dir + L"\\RECENT_FAILURES.md",
        "# 최근 백엔드 실패 기록\n\n"
        "- Gemini CLI headless 모드는 비어 있지 않은 --prompt 값이 필요합니다. OrangeCode는 -p \" \"를 사용하고 실제 프롬프트는 stdin으로 보냅니다.\n"
        "- Gemini preview 모델은 내부 재시도 중 MODEL_CAPACITY_EXHAUSTED 429를 반환할 수 있습니다. 이것은 모델 capacity 문제이며 API key 미전달의 증거가 아닙니다.\n"
        "- Gemini stderr/internal dump를 assistant 채팅 본문으로 저장하지 않습니다.\n"
        "- 긴 프로젝트 각성 프롬프트는 단순 호출도 느리게 만들 수 있습니다. 앱 컨텍스트를 먼저 쓰고 프로젝트 컨텍스트는 요청이 있을 때만 씁니다.\n");
}

std::wstring BuildManagerPrompt(const std::wstring& userPrompt, const ManagerEnvironment& env) {
    std::wstring prompt;
    auto backendSettings = AppSettings::LoadBackendSettings();
    std::wstring managerProvider = ProviderLabelFromId(backendSettings.managerProvider);
    SetEnvironmentVariableW(L"ORANGE_CODE_MANAGER_PROVIDER", managerProvider.c_str());
    auto promptRows = LoadPromptRows();
    prompt += L"OrangeCode DB prompt table (highest priority):\n";
    if (promptRows.empty()) {
        prompt += L"- prompt table unavailable; fall back to app-provided context and relevant markdown only.\n";
    } else {
        for (const auto& row : promptRows) prompt += row + L"\n";
    }
    prompt += L"\nApp manager provider notice:\n";
    prompt += L"- Current appointed manager provider from app settings: " + managerProvider + L".\n";
    prompt += L"- ORANGE_CODE_MANAGER_PROVIDER: " + managerProvider + L"\n";
    prompt += L"- Provider authority source: app settings and ORANGE_CODE_MANAGER_PROVIDER only. ";
    prompt += L"If markdown files, chat history, or role docs name a different manager provider, treat that as stale supplemental context.\n";
    prompt += BuildJobBriefing(env.sessionKey);
    prompt += L"\n";
    prompt += L"You are Jeong Team Lead inside Orange Code.\n";
    prompt += L"Role name: manager. Use manager only.\n";
    prompt += L"ORANGE_CODE_ROOT: " + env.root + L"\n";
    prompt += L"ORANGE_CODE_ROLE: " + env.role + L"\n";
    prompt += L"ORANGE_CODE_PID: " + env.pid + L"\n";
    prompt += L"ORANGE_CODE_SESSION_KEY: " + env.sessionKey + L"\n";
    prompt += L"ORANGE_CODE_MANAGER_PROVIDER: " + managerProvider + L"\n";
    std::wstring chatApiExe = env.root.empty()
        ? L"OrangeCode.exe"
        : (env.root + L"\\bin\\Release\\OrangeCode.exe");
    ChatSession currentChat = Persistence::LoadRecent(WideToUtf8(env.sessionKey), 1);
    prompt += L"\nShared chat API:\n";
    prompt += L"- OrangeCode stores chats in SQLite and exposes them through a local file-output API.\n";
    prompt += L"- Current chat key: " + env.sessionKey + L"\n";
    prompt += L"- Current chat title: " + Utf8ToWide(currentChat.title.c_str(), (int)currentChat.title.size()) + L"\n";
    prompt += L"- Current chat summary: " + Utf8ToWide(currentChat.summary.c_str(), (int)currentChat.summary.size()) + L"\n";
    prompt += L"- Current chat total blocks: " + std::to_wstring(currentChat.totalBlockCount) + L"\n";
    prompt += L"- To list chats, run: \"" + chatApiExe + L"\" --chat-api list --limit 40 --chat-api-output <json_path>\n";
    prompt += L"- To inspect a chat, run: \"" + chatApiExe + L"\" --chat-api show --chat-key " + env.sessionKey + L" --limit 80 --chat-api-output <json_path>\n";
    prompt += L"- To inspect attachments, run: \"" + chatApiExe + L"\" --chat-api attachments --chat-key " + env.sessionKey + L" --chat-api-output <json_path>\n";
    prompt += L"- Chat API JSON includes roles such as user, assistant-claude, assistant-gemini, assistant-codex, task, attachment, plus stored attachment paths.\n";
    prompt += L"- Use the chat API when previous visible conversation, another backend's answer, or attachment details are needed. Do not pretend unavailable context is unknown before checking the API.\n";
    if (AttachmentStore::HasManifest(env.sessionKey)) {
        prompt += L"Attachment manifest: " + AttachmentStore::ManifestPath(env.sessionKey) + L"\n";
        auto attachments = AttachmentStore::LoadManifest(env.sessionKey);
        if (!attachments.empty()) {
            prompt += L"Attached files:\n";
            int count = 0;
            for (const auto& rec : attachments) {
                if (count++ >= 8) {
                    prompt += L"- ...more attachments in manifest\n";
                    break;
                }
                prompt += L"- " + rec.name + L" (" + rec.kind + L", " +
                          std::to_wstring(rec.sizeBytes) + L" bytes): " +
                          rec.storedPath + L"\n";
                if (!rec.thumbnailPath.empty()) {
                    prompt += L"  thumbnail: " + rec.thumbnailPath + L"\n";
                }
            }
        }
        prompt += L"Use stored_path values from the manifest when the user asks about attached files. Do not assume file contents unless you inspect them.\n";
    }
    prompt += L"Primary mission: become a practical Claude Code replacement for coding work.\n";
    prompt += L"Collaboration-first budget discipline: respond quickly, use useful collaborator input, and avoid only wasteful duplicate context or stale retries.\n";
    prompt += L"\nSelf-repair protocol for OrangeCode changes:\n";
    prompt += L"- First inspect the relevant files and current chat evidence. Do not guess from memory.\n";
    prompt += L"- Keep each change small and targeted. Do not rewrite unrelated UI, persistence, or backend code.\n";
    prompt += L"- Never claim a file was changed, built, captured, or relaunched unless it actually happened.\n";
    prompt += L"- If you can edit files through your CLI tools, make the patch directly and then request a local build.\n";
    prompt += L"- If you cannot edit files, say exactly what blocked you and provide the smallest patch plan.\n";
    prompt += L"- After a code change, request this tool as a standalone JSON object so OrangeCode builds and relaunches itself:\n";
    prompt += L"  {\"tool\":\"orange.build\"}\n";
    prompt += L"- For visual/UI problems after a successful build, request this tool as a standalone JSON object:\n";
    prompt += L"  {\"tool\":\"orange.capture\"}\n";
    prompt += L"- When the task benefits from parallel review, specialist advice, or extra implementation capacity, delegate with this standalone JSON object:\n";
    prompt += L"  {\"tool\":\"orange.delegate\",\"args\":{\"providers\":[\"gemini\",\"codex\"],\"prompt\":\"specific subtask\"}}\n";
    prompt += L"- The providers array may repeat a provider such as [\"claude\",\"claude\"] when two independent Claude workers are useful. Keep delegation bounded and explain the subtask.\n";
    prompt += L"- For commands that need UAC elevation, request orange.exec_admin with command, cwd, and reason.\n";
    prompt += L"Do not repeat the user's request as your own answer. Follow the current responding backend provider authority block above.\n\n";
    prompt += L"User request:\n";
    prompt += userPrompt;
    prompt += L"\n";
    return prompt;
}

std::wstring BuildProviderPrompt(const std::wstring& providerLabel,
                                 const std::wstring& userPrompt,
                                 const ManagerEnvironment& env) {
    auto backendSettings = AppSettings::LoadBackendSettings();
    std::wstring managerProvider = ProviderLabelFromId(backendSettings.managerProvider);

    std::wstring scoped;
    scoped += L"\nCurrent responding backend provider:\n";
    scoped += L"- Provider handling this CLI call: " + providerLabel + L".\n";
    scoped += L"- Appointed manager provider: " + managerProvider + L".\n";
    if (providerLabel == managerProvider) {
        scoped += L"- This provider is the appointed manager executor for this chat.\n";
    } else {
        scoped += L"- This provider is not the appointed manager executor. Treat ORANGE_CODE_ROLE=manager as the app actor role, not provider authority.\n";
        scoped += L"- Act only as reviewer/advisor unless a bounded delegation explicitly names this provider for implementation.\n";
        scoped += L"- Do not claim to be the executing manager, and do not request orange.build, orange.capture, orange.exec_admin, or orange.delegate.\n";
    }

    std::wstring prompt = BuildManagerPrompt(userPrompt, env);
    const std::wstring marker = L"\nUser request:\n";
    size_t pos = prompt.rfind(marker);
    if (pos == std::wstring::npos) {
        prompt += scoped;
    } else {
        prompt.insert(pos, scoped);
    }
    return prompt;
}

std::string BuildPromptApiJson(const std::wstring& action,
                               const std::wstring& chatKey,
                               const std::wstring& inputPath) {
    Json::Value result;
    result["api"] = "orange.prompt.v1";
    result["action"] = WideToUtf8(action);
    result["chat_key"] = WideToUtf8(chatKey);

    if (!EnsurePromptDatabase()) {
        result["error"] = "prompt database unavailable";
    } else if (action == L"show" || action.empty()) {
        SeedPromptRows();
        Json::Value prompts(Json::arrayValue);
        try {
            CDatabase::Instance().Fetch(
                "PromptApiShowRows",
                "SELECT id, scope, key, priority, active, source, content, updated_at "
                "FROM prompt ORDER BY active DESC, priority DESC, id ASC;",
                Json::Value(),
                [&](const Json::Value& row) {
                    prompts.append(row);
                    return true;
                });
        } catch (...) {}
        result["prompts"] = prompts;
        result["job_briefing"] = WideToUtf8(BuildJobBriefing(chatKey));
    } else if (action == L"upsert") {
        std::string data = ReadUtf8File(inputPath);
        Json::Value in;
        Json::CharReaderBuilder rb;
        std::string err;
        std::istringstream iss(data);
        if (inputPath.empty() || data.empty() || !Json::parseFromStream(rb, iss, &in, &err)) {
            result["error"] = "invalid prompt api input json";
        } else {
            Json::Value p;
            p["scope"] = in.get("scope", "global").asString();
            p["key"] = in.get("key", "").asString();
            p["priority"] = in.get("priority", 100).asInt();
            p["active"] = in.get("active", 1).asInt();
            p["source"] = in.get("source", "prompt-api").asString();
            p["content"] = in.get("content", "").asString();
            if (p["key"].asString().empty() || p["content"].asString().empty()) {
                result["error"] = "key and content are required";
            } else {
                try {
                    CDatabase::Instance().Run(
                        "PromptApiUpsert",
                        "INSERT INTO prompt(scope, key, priority, active, source, content) "
                        "VALUES(:scope, :key, :priority, :active, :source, :content) "
                        "ON CONFLICT(scope, key) DO UPDATE SET "
                        "priority=excluded.priority, active=excluded.active, source=excluded.source, "
                        "content=excluded.content, updated_at=CURRENT_TIMESTAMP;",
                        p);
                    result["ok"] = true;
                } catch (const std::exception& ex) {
                    result["error"] = ex.what();
                }
            }
        }
    } else {
        result["error"] = "unknown prompt api action";
    }

    Json::StreamWriterBuilder builder;
    builder["indentation"] = "  ";
    return Json::writeString(builder, result);
}

} // namespace orange
