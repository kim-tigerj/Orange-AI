#include "CodexPrintBackend.h"

#include "ManagerEnvironment.h"

#include <json/json.h>
#include <exception>
#include <process.h>
#include <vector>

namespace orange {

namespace {

void CloseIfValid(HANDLE& h) {
    if (h) {
        CloseHandle(h);
        h = nullptr;
    }
}

bool IsExePath(const std::wstring& p) {
    return p.size() >= 4 && _wcsicmp(p.c_str() + p.size() - 4, L".exe") == 0;
}

std::wstring QuoteArg(const std::wstring& s) {
    std::wstring out = L"\"";
    for (wchar_t ch : s) {
        if (ch == L'"') out += L'\\';
        out += ch;
    }
    out += L"\"";
    return out;
}

void AppendTextContent(const Json::Value& content, std::wstring& out) {
    if (content.isString()) {
        std::string text = content.asString();
        out += Utf8ToWide(text.c_str(), (int)text.size());
        return;
    }
    if (!content.isArray()) return;
    for (const auto& part : content) {
        const std::string type = part.get("type", "").asString();
        if (type == "output_text" || type == "text" || part.isMember("text")) {
            std::string text = part.get("text", "").asString();
            if (!text.empty()) out += Utf8ToWide(text.c_str(), (int)text.size());
        }
    }
}

std::wstring StringField(const Json::Value& value, const char* key) {
    if (!value.isObject() || !value[key].isString()) return {};
    std::string text = value[key].asString();
    if (text.empty()) return {};
    return Utf8ToWide(text.c_str(), (int)text.size());
}

std::wstring ExtractAssistantMessage(const Json::Value& value) {
    if (!value.isObject()) return {};

    const std::string role = value.get("role", "").asString();
    const std::string type = value.get("type", "").asString();
    const bool assistantLike =
        role == "assistant" ||
        type == "agent_message" ||
        type == "assistant" ||
        type == "assistant_message" ||
        (type == "message" && role == "assistant");
    if (!assistantLike) return {};

    std::wstring out;
    AppendTextContent(value["message"], out);
    AppendTextContent(value["content"], out);
    AppendTextContent(value["delta"], out);
    if (out.empty()) out = StringField(value, "text");
    if (out.empty()) out = StringField(value, "msg");
    return out;
}

std::wstring ExtractCodexText(const Json::Value& root) {
    const std::string type = root.get("type", "").asString();

    if (type == "event_msg" && root["payload"].isObject()) {
        return ExtractAssistantMessage(root["payload"]);
    }

    if (type == "response_item" && root["payload"].isObject()) {
        const Json::Value& payload = root["payload"];
        const std::string itemType = payload.get("type", "").asString();
        if (itemType == "function_call_output" || itemType == "function_call" ||
            itemType == "tool_call" || itemType == "tool_result") {
            return {};
        }
        return ExtractAssistantMessage(payload);
    }

    if (root["item"].isObject()) {
        const Json::Value& item = root["item"];
        const std::string itemType = item.get("type", "").asString();
        if (itemType == "function_call_output" || itemType == "function_call" ||
            itemType == "tool_call" || itemType == "tool_result") {
            return {};
        }
        return ExtractAssistantMessage(item);
    }

    return ExtractAssistantMessage(root);
}

} // namespace

CodexPrintBackend::CodexLocation CodexPrintBackend::FindCodexPath() {
    static const wchar_t* names[] = { L"codex.exe", L"codex.cmd", L"codex.bat" };
    for (auto* name : names) {
        wchar_t buf[MAX_PATH] = L"";
        DWORD got = SearchPathW(nullptr, name, nullptr, MAX_PATH, buf, nullptr);
        if (got > 0 && got < MAX_PATH) return { std::wstring(buf), IsExePath(buf) };
    }
    wchar_t appdata[MAX_PATH] = L"";
    if (GetEnvironmentVariableW(L"APPDATA", appdata, MAX_PATH) > 0) {
        for (auto* name : names) {
            std::wstring p = std::wstring(appdata) + L"\\npm\\" + name;
            if (GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES) {
                return { p, IsExePath(p) };
            }
        }
    }
    return { {}, false };
}

bool CodexPrintBackend::Start(OutputCallback outputCb,
                              TurnDoneCallback turnDoneCb,
                              ToolUseCallback,
                              ErrorCallback,
                              RetryCallback) {
    m_outputCb = std::move(outputCb);
    m_turnDoneCb = std::move(turnDoneCb);
    m_codexPath = FindCodexPath();
    if (m_codexPath.path.empty()) {
        Emit(L"[Codex CLI를 찾지 못했습니다. PATH 또는 %APPDATA%\\npm 설치를 확인하세요.]\r\n");
        return false;
    }
    m_ready = true;
    return true;
}

bool CodexPrintBackend::SendPrompt(const std::wstring& prompt) {
    if (!m_ready) return false;
    bool expected = false;
    if (!m_busy.compare_exchange_strong(expected, true)) return false;
    auto* ctx = new WorkerCtx{this, prompt};
    unsigned tid = 0;
    m_hThread = (HANDLE)_beginthreadex(nullptr, 0, WorkerProc, ctx, 0, &tid);
    if (!m_hThread) {
        delete ctx;
        m_busy = false;
        return false;
    }
    return true;
}

void CodexPrintBackend::Stop() {
    Cancel();
    if (m_hThread) {
        WaitForSingleObject(m_hThread, 5000);
        CloseHandle(m_hThread);
        m_hThread = nullptr;
    }
    m_ready = false;
}

void CodexPrintBackend::Cancel() {
    CloseIfValid(m_hJob);  // kills job members (LLM process + any cmd.exe children)
    HANDLE proc = m_hProcess;
    if (proc) TerminateProcess(proc, 1);
}

bool CodexPrintBackend::IsReady() const {
    return m_ready && !m_busy.load();
}

unsigned __stdcall CodexPrintBackend::WorkerProc(void* lpParam) {
    std::unique_ptr<WorkerCtx> ctx(static_cast<WorkerCtx*>(lpParam));
    try {
        ctx->self->RunOneTurn(ctx->prompt);
    } catch (const std::exception& ex) {
        std::string msg = ex.what();
        ctx->self->Emit(L"[Codex backend exception] " +
                        NormalizeNewlines(Utf8ToWide(msg.c_str(), (int)msg.size())) + L"\r\n");
    } catch (...) {
        ctx->self->Emit(L"[Codex backend exception] Unknown backend error.\r\n");
    }
    ctx->self->m_busy = false;
    if (ctx->self->m_turnDoneCb) ctx->self->m_turnDoneCb();
    return 0;
}

void CodexPrintBackend::RunOneTurn(const std::wstring& prompt) {
    ManagerEnvironment env = EnsureManagerEnvironment();
    std::wstring managerPrompt = BuildProviderPrompt(L"Codex", prompt, env);

    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE stdinRead = nullptr;
    HANDLE stdinWrite = nullptr;
    HANDLE stdoutRead = nullptr;
    HANDLE stdoutWrite = nullptr;
    if (!CreatePipe(&stdinRead, &stdinWrite, &sa, 0) ||
        !CreatePipe(&stdoutRead, &stdoutWrite, &sa, 0)) {
        Emit(L"[Codex 파이프 생성 실패]\r\n");
        CloseIfValid(stdinRead);
        CloseIfValid(stdinWrite);
        CloseIfValid(stdoutRead);
        CloseIfValid(stdoutWrite);
        return;
    }
    SetHandleInformation(stdinWrite, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(stdoutRead, HANDLE_FLAG_INHERIT, 0);

    std::wstring root = env.root.empty() ? FindProjectRoot() : env.root;
    std::wstring args = L" exec --json --color never -C " + QuoteArg(root) +
                        L" --sandbox danger-full-access -";
    std::wstring cmd = m_codexPath.directExe
        ? (L"\"" + m_codexPath.path + L"\"" + args)
        : (L"cmd.exe /c \"\"" + m_codexPath.path + L"\"" + args + L"\"");

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = stdinRead;
    si.hStdOutput = stdoutWrite;
    si.hStdError = stdoutWrite;
    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> mutableCmd(cmd.begin(), cmd.end());
    mutableCmd.push_back(L'\0');

    BOOL ok = CreateProcessW(nullptr, mutableCmd.data(), nullptr, nullptr, TRUE,
                             CREATE_NO_WINDOW, nullptr,
                             root.empty() ? nullptr : root.c_str(), &si, &pi);
    CloseIfValid(stdinRead);
    CloseIfValid(stdoutWrite);
    if (!ok) {
        CloseIfValid(stdinWrite);
        CloseIfValid(stdoutRead);
        Emit(L"[Codex 프로세스 시작 실패]\r\n");
        return;
    }
    m_hProcess = pi.hProcess;
    CloseHandle(pi.hThread);

    // Assign child to a Job Object so it dies if OrangeCode exits uncleanly
    HANDLE hJob = CreateJobObjectW(nullptr, nullptr);
    if (hJob) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION info{};
        info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(hJob, JobObjectExtendedLimitInformation, &info, sizeof(info));
        if (!AssignProcessToJobObject(hJob, pi.hProcess)) { CloseHandle(hJob); hJob = nullptr; }
        m_hJob = hJob;
    }

    std::string promptUtf8 = WideToUtf8(managerPrompt);
    DWORD written = 0;
    WriteFile(stdinWrite, promptUtf8.data(), (DWORD)promptUtf8.size(), &written, nullptr);
    CloseIfValid(stdinWrite);

    char buf[4096];
    DWORD readBytes = 0;
    std::string line;
    while (ReadFile(stdoutRead, buf, sizeof(buf), &readBytes, nullptr) && readBytes > 0) {
        for (DWORD i = 0; i < readBytes; ++i) {
            if (buf[i] == '\n') {
                if (!line.empty()) HandleJsonLine(line);
                line.clear();
            } else if (buf[i] != '\r') {
                line += buf[i];
            }
        }
    }
    if (!line.empty()) HandleJsonLine(line);
    WaitForSingleObject(pi.hProcess, INFINITE);
    CloseIfValid(stdoutRead);
    CloseHandle(pi.hProcess);
    m_hProcess = nullptr;
    CloseIfValid(m_hJob);
}

void CodexPrintBackend::HandleJsonLine(const std::string& line) {
    Json::CharReaderBuilder builder;
    std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
    Json::Value root;
    std::string errs;
    if (!reader->parse(line.data(), line.data() + line.size(), &root, &errs)) {
        Emit(NormalizeNewlines(Utf8ToWide(line.c_str(), (int)line.size())) + L"\r\n");
        return;
    }
    if (!root.isObject()) return;

    std::wstring text = ExtractCodexText(root);
    if (!text.empty()) {
        Emit(NormalizeNewlines(text));
        return;
    }

    std::string type = root.get("type", "").asString();
    if (type.find("error") != std::string::npos) {
        std::string dump = "Codex CLI returned an error; internal JSON dump suppressed.";
        Emit(L"[Codex 오류] " + NormalizeNewlines(Utf8ToWide(dump.c_str(), (int)dump.size())) + L"\r\n");
    }
}

} // namespace orange
