#include "ClaudePrintBackend.h"

#include "ManagerEnvironment.h"

#include <json/json.h>
#include <exception>
#include <vector>

namespace orange {

namespace {

struct PipeSet {
    HANDLE stdinRead = nullptr;
    HANDLE stdinWrite = nullptr;
    HANDLE stdoutRead = nullptr;
    HANDLE stdoutWrite = nullptr;
};

bool CreateChildPipes(PipeSet& pipes) {
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    if (!CreatePipe(&pipes.stdinRead, &pipes.stdinWrite, &sa, 0)) return false;
    if (!CreatePipe(&pipes.stdoutRead, &pipes.stdoutWrite, &sa, 0)) return false;
    SetHandleInformation(pipes.stdinWrite, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(pipes.stdoutRead, HANDLE_FLAG_INHERIT, 0);
    return true;
}

void CloseIfValid(HANDLE& h) {
    if (h) {
        CloseHandle(h);
        h = nullptr;
    }
}

bool IsExePath(const std::wstring& p) {
    return p.size() >= 4 && _wcsicmp(p.c_str() + p.size() - 4, L".exe") == 0;
}

void AppendAssistantContent(const Json::Value& content, std::wstring& out) {
    if (content.isString()) {
        std::string text = content.asString();
        out += Utf8ToWide(text.c_str(), (int)text.size());
        return;
    }
    if (!content.isArray()) return;
    for (const auto& item : content) {
        const std::string type = item.get("type", "").asString();
        if (type == "text" || item.isMember("text")) {
            std::string text = item.get("text", "").asString();
            out += Utf8ToWide(text.c_str(), (int)text.size());
        }
    }
}

} // namespace

bool ClaudePrintBackend::Start(OutputCallback outputCb,
                               TurnDoneCallback turnDoneCb,
                               ToolUseCallback,
                               ErrorCallback,
                               RetryCallback retryCb) {
    m_outputCb = std::move(outputCb);
    m_turnDoneCb = std::move(turnDoneCb);
    m_retryCb = std::move(retryCb);

    static const wchar_t* names[] = { L"claude.exe", L"claude.cmd", L"claude.bat" };
    for (auto* name : names) {
        wchar_t buf[MAX_PATH] = L"";
        DWORD got = SearchPathW(nullptr, name, nullptr, MAX_PATH, buf, nullptr);
        if (got > 0 && got < MAX_PATH) {
            m_claudePath = buf;
            m_claudeDirectExe = IsExePath(m_claudePath);
            break;
        }
    }
    if (m_claudePath.empty()) {
        wchar_t appdata[MAX_PATH] = L"";
        if (GetEnvironmentVariableW(L"APPDATA", appdata, MAX_PATH) > 0) {
            for (auto* name : names) {
                std::wstring p = std::wstring(appdata) + L"\\npm\\" + name;
                if (GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES) {
                    m_claudePath = p;
                    m_claudeDirectExe = IsExePath(m_claudePath);
                    break;
                }
            }
        }
    }

    if (m_claudePath.empty()) {
        Emit(L"[claude CLI瑜?李얠? 紐삵뻽?듬땲?? PATH ?먮뒗 %APPDATA%\\npm ?ㅼ튂瑜??뺤씤?섏꽭??]\r\n");
        return false;
    }
    m_ready = true;
    return true;
}

bool ClaudePrintBackend::SendPrompt(const std::wstring& prompt) {
    if (!m_ready) return false;
    bool expected = false;
    if (!m_busy.compare_exchange_strong(expected, true)) return false;
    auto* ctx = new WorkerCtx{this, prompt};
    unsigned tid = 0;
    m_hThread = (HANDLE)_beginthreadex(nullptr, 0, ThreadProc, ctx, 0, &tid);
    if (!m_hThread) {
        delete ctx;
        m_busy = false;
        return false;
    }
    m_running = true;
    return true;
}

void ClaudePrintBackend::Stop() {
    Cancel();
    if (m_hThread) {
        WaitForSingleObject(m_hThread, INFINITE);
        CloseHandle(m_hThread);
        m_hThread = nullptr;
    }
    m_ready = false;
}

void ClaudePrintBackend::Cancel() {
    CloseIfValid(m_hJob);  // kills job members (LLM process + any cmd.exe children)
    HANDLE proc = m_hProcess;
    if (proc) {
        TerminateProcess(proc, 1);
    }
}

bool ClaudePrintBackend::IsReady() const {
    return m_ready && !m_busy.load();
}

unsigned __stdcall ClaudePrintBackend::ThreadProc(void* lpParam) {
    std::unique_ptr<WorkerCtx> ctx(static_cast<WorkerCtx*>(lpParam));
    try {
        ctx->self->RunOneTurn(ctx->prompt);
    } catch (const std::exception& ex) {
        std::string msg = ex.what();
        ctx->self->Emit(L"[Claude backend exception] " +
                        NormalizeNewlines(Utf8ToWide(msg.c_str(), (int)msg.size())) + L"\r\n");
    } catch (...) {
        ctx->self->Emit(L"[Claude backend exception] Unknown backend error.\r\n");
    }
    ctx->self->m_busy = false;
    ctx->self->m_running = false;
    if (ctx->self->m_turnDoneCb) ctx->self->m_turnDoneCb();
    return 0;
}

void ClaudePrintBackend::RunOneTurn(const std::wstring& prompt) {
    ManagerEnvironment env = EnsureManagerEnvironment();
    std::wstring managerPrompt = BuildProviderPrompt(L"Claude", prompt, env);

    PipeSet pipes;
    if (!CreateChildPipes(pipes)) {
        Emit(L"[claude ?뚯씠???앹꽦 ?ㅽ뙣]\r\n");
        return;
    }

    std::wstring args = L" --print --output-format stream-json --verbose --dangerously-skip-permissions";
    if (IsLikelyUuid(m_sessionId)) {
        args += L" --resume " + Utf8ToWide(m_sessionId.c_str(), (int)m_sessionId.size());
    }
    std::wstring cmd = m_claudeDirectExe
        ? (L"\"" + m_claudePath + L"\"" + args)
        : (L"cmd.exe /c \"\"" + m_claudePath + L"\"" + args + L"\"");

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = pipes.stdinRead;
    si.hStdOutput = pipes.stdoutWrite;
    si.hStdError = pipes.stdoutWrite;
    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> mutableCmd(cmd.begin(), cmd.end());
    mutableCmd.push_back(L'\0');

    std::wstring root = env.root.empty() ? FindProjectRoot() : env.root;
    BOOL ok = CreateProcessW(nullptr, mutableCmd.data(), nullptr, nullptr, TRUE,
                             CREATE_NO_WINDOW, nullptr,
                             root.empty() ? nullptr : root.c_str(), &si, &pi);
    CloseIfValid(pipes.stdinRead);
    CloseIfValid(pipes.stdoutWrite);
    if (!ok) {
        CloseIfValid(pipes.stdinWrite);
        CloseIfValid(pipes.stdoutRead);
        Emit(L"[claude ?꾨줈?몄뒪 ?쒖옉 ?ㅽ뙣]\r\n");
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
    WriteFile(pipes.stdinWrite, promptUtf8.data(), (DWORD)promptUtf8.size(), &written, nullptr);
    CloseIfValid(pipes.stdinWrite);

    char buf[4096];
    DWORD readBytes = 0;
    std::string line;
    while (ReadFile(pipes.stdoutRead, buf, sizeof(buf), &readBytes, nullptr) && readBytes > 0) {
        for (DWORD i = 0; i < readBytes; ++i) {
            if (buf[i] == '\n') {
                if (!line.empty()) HandleOutput(line);
                line.clear();
            } else if (buf[i] != '\r') {
                line += buf[i];
            }
        }
    }
    if (!line.empty()) HandleOutput(line);
    WaitForSingleObject(pi.hProcess, INFINITE);
    CloseIfValid(pipes.stdoutRead);
    CloseHandle(pi.hProcess);
    m_hProcess = nullptr;
    CloseIfValid(m_hJob);
}

void ClaudePrintBackend::HandleOutput(const std::string& line) {
    Json::CharReaderBuilder builder;
    std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
    Json::Value root;
    std::string errs;
    if (!reader->parse(line.data(), line.data() + line.size(), &root, &errs)) return;

    const std::string type = root.get("type", "").asString();
    if (type == "system" && root.isMember("session_id")) {
        m_sessionId = root.get("session_id", "").asString();
        return;
    }
    if (type == "assistant" || (type == "message" && root.get("role", "").asString() == "assistant")) {
        std::wstring out;
        AppendAssistantContent(root["message"]["content"], out);
        AppendAssistantContent(root["content"], out);
        if (!out.empty()) Emit(NormalizeNewlines(out));
        return;
    }
    if (type == "result" && root.get("is_error", false).asBool()) {
        std::string err = root.get("result", "").asString();
        if (!err.empty()) {
            // rate limit / overloaded 패턴 — retryCb 우선, 없으면 일반 에러로 출력
            auto lower = err;
            std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
            bool isRateLimit =
                lower.find("temporarily limiting") != std::string::npos ||
                lower.find("rate limit") != std::string::npos ||
                lower.find("rate_limit") != std::string::npos ||
                lower.find("429") != std::string::npos ||
                lower.find("overloaded") != std::string::npos ||
                lower.find("too many requests") != std::string::npos;
            if (isRateLimit && m_retryCb) {
                m_retryCb(NormalizeNewlines(Utf8ToWide(err.c_str(), (int)err.size())));
            } else {
                Emit(L"[오류] " + NormalizeNewlines(Utf8ToWide(err.c_str(), (int)err.size())) + L"\r\n");
            }
        }
    }
}

} // namespace orange
