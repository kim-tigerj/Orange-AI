#include "GeminiPrintBackend.h"

#include "AppSettings.h"
#include "ManagerEnvironment.h"

#include <json/json.h>
#include <exception>
#include <process.h>
#include <array>
#include <fstream>
#include <strsafe.h>
#include <vector>

namespace orange {

namespace {


std::wstring QuoteArg(const std::wstring& s) {
    std::wstring out = L"\"";
    for (wchar_t ch : s) {
        if (ch == L'"') out += L'\\';
        out += ch;
    }
    out += L"\"";
    return out;
}

bool IsExePath(const std::wstring& p) {
    return p.size() >= 4 && _wcsicmp(p.c_str() + p.size() - 4, L".exe") == 0;
}

bool LooksLikeGeminiInternalDump(const std::string& line) {
    return line.rfind("    at ", 0) == 0 ||
           line.rfind("  at ", 0) == 0 ||
           line.find("node_modules/@google/gemini-cli") != std::string::npos ||
           line.find("cloudcode-pa.googleapis.com") != std::string::npos ||
           line.find("streamGenerateContent") != std::string::npos ||
           line.find("gaxios") != std::string::npos ||
           line.find("axios") != std::string::npos ||
           line.find("params:") != std::string::npos ||
           line.find("data: '[") != std::string::npos ||
           line.find("rateLimitExceeded") != std::string::npos ||
           line.find("RESOURCE_EXHAUSTED") != std::string::npos ||
           line.find("MODEL_CAPACITY_EXHAUSTED") != std::string::npos ||
           line.find("No capacity available for model") != std::string::npos ||
           line.find("errorRedactor") != std::string::npos ||
           line.find("AbortSignal") != std::string::npos ||
           line.find("validateStatus") != std::string::npos ||
           line.find("paramsSerializer") != std::string::npos ||
           line.find("responseType: 'stream'") != std::string::npos ||
           line.find("Authorization:") != std::string::npos ||
           line.find("User-Agent: 'GeminiCLI") != std::string::npos ||
           line.find("x-goog-api-client") != std::string::npos ||
           line.find("body: '<<REDACTED>>") != std::string::npos ||
           line.find("config: {") != std::string::npos ||
           line.find("response: {") != std::string::npos ||
           line.find("headers:") != std::string::npos ||
           line.find("method: 'POST'") != std::string::npos ||
           line.find("url: 'https://") != std::string::npos ||
           line.find("retry: false") != std::string::npos ||
           line.find("Symbol(gaxios") != std::string::npos;
}

std::wstring GetProcessEnv(const wchar_t* name) {
    wchar_t value[32767] = L"";
    DWORD n = GetEnvironmentVariableW(name, value, static_cast<DWORD>(std::size(value)));
    if (n == 0 || n >= std::size(value)) return {};
    return value;
}

std::wstring ReadRegistryString(HKEY root, const wchar_t* subkey, const wchar_t* name) {
    DWORD type = 0;
    DWORD bytes = 0;
    LSTATUS rc = RegGetValueW(root, subkey, name, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ,
                              &type, nullptr, &bytes);
    if (rc != ERROR_SUCCESS || bytes < sizeof(wchar_t)) return {};

    std::wstring value(bytes / sizeof(wchar_t), L'\0');
    rc = RegGetValueW(root, subkey, name, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ,
                      &type, value.data(), &bytes);
    if (rc != ERROR_SUCCESS) return {};
    while (!value.empty() && value.back() == L'\0') value.pop_back();
    return value;
}

void EnsureGeminiAuthEnvironment() {
    static const std::array<const wchar_t*, 5> names = {
        L"GEMINI_API_KEY",
        L"GOOGLE_API_KEY",
        L"GOOGLE_GENAI_USE_VERTEXAI",
        L"GOOGLE_CLOUD_PROJECT",
        L"GOOGLE_CLOUD_LOCATION"
    };

    for (const wchar_t* name : names) {
        if (!GetProcessEnv(name).empty()) continue;

        std::wstring value = ReadRegistryString(HKEY_CURRENT_USER, L"Environment", name);
        if (value.empty()) {
            value = ReadRegistryString(HKEY_LOCAL_MACHINE,
                                      L"SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Environment",
                                      name);
        }
        if (!value.empty()) {
            SetEnvironmentVariableW(name, value.c_str());
        }
    }
}

std::wstring AuthEnvironmentSummary() {
    static const std::array<const wchar_t*, 2> keyNames = {
        L"GEMINI_API_KEY",
        L"GOOGLE_API_KEY"
    };
    for (const wchar_t* name : keyNames) {
        std::wstring value = GetProcessEnv(name);
        if (!value.empty()) {
            return std::wstring(name) + L" 전달됨(len=" + std::to_wstring(value.size()) + L")";
        }
    }
    return L"GEMINI_API_KEY/GOOGLE_API_KEY 없음";
}

std::wstring TrimCopy(std::wstring value) {
    while (!value.empty() && iswspace(value.front())) value.erase(value.begin());
    while (!value.empty() && iswspace(value.back())) value.pop_back();
    return value;
}

std::wstring GeminiDiagLogPath() {
    std::wstring dir = ManagerAppContextDir();
    if (dir.empty()) return {};
    return dir + L"\\GEMINI_DIAGNOSTICS.jsonl";
}

void AppendGeminiDiag(const std::string& event, const std::string& details) {
    std::wstring path = GeminiDiagLogPath();
    if (path.empty()) return;
    SYSTEMTIME st{};
    GetSystemTime(&st);
    char ts[40] = "";
    sprintf_s(ts, "%04u-%02u-%02uT%02u:%02u:%02uZ",
              st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    std::ofstream ofs(WideToUtf8(path), std::ios::binary | std::ios::app);
    if (!ofs.is_open()) return;
    ofs << "{\"ts\":\"" << ts << "\",\"event\":\"" << event << "\"";
    if (!details.empty()) ofs << "," << details;
    ofs << "}\n";
}

} // namespace

GeminiPrintBackend::GeminiLocation GeminiPrintBackend::FindGeminiPath() {
    static const wchar_t* names[] = { L"gemini.exe", L"gemini.cmd", L"gemini.bat" };
    for (auto* name : names) {
        wchar_t buf[MAX_PATH] = L"";
        DWORD got = SearchPathW(nullptr, name, nullptr, MAX_PATH, buf, nullptr);
        if (got > 0 && got < MAX_PATH) return { std::wstring(buf), IsExePath(buf) };
    }
    return { {}, false };
}

bool GeminiPrintBackend::Start(OutputCallback outputCb,
                               TurnDoneCallback turnDoneCb,
                               ToolUseCallback,
                               ErrorCallback errorCb,
                               RetryCallback) {
    m_outputCb = std::move(outputCb);
    m_turnDoneCb = std::move(turnDoneCb);
    m_errorCb = std::move(errorCb);
    m_geminiPath = FindGeminiPath();
    if (m_geminiPath.path.empty()) {
        Emit(L"[Gemini CLI瑜?李얠? 紐삵뻽?듬땲?? PATH ?ㅼ튂瑜??뺤씤?섏꽭??]\r\n");
        return false;
    }
    m_ready = true;
    return true;
}

bool GeminiPrintBackend::SendPrompt(const std::wstring& prompt) {
    if (!m_ready) return false;
    bool expected = false;
    if (!m_busy.compare_exchange_strong(expected, true)) return false;
    m_quotaErrorEmitted = false;
    m_terminalErrorEmitted = false;
    m_suppressXtermDump = false;
    m_resultSuccess = false;
    m_assistantTextEmitted = false;
    m_capacityErrorSeen = false;
    m_retryNoticeEmitted = false;
    m_quotaFirstSeenTick = 0;
    m_quotaRetryCount = 0;
    m_currentModel.clear();
    auto* ctx = new WorkerCtx{this, prompt};
    unsigned tid = 0;
    m_hThread = (HANDLE)_beginthreadex(nullptr, 0, WorkerProc, ctx, 0, &tid);
    if (!m_hThread) {
        delete ctx;
        m_busy = false;
        return false;
    }
    m_running = true;
    return true;
}

void GeminiPrintBackend::Stop() {
    Cancel();
    if (m_hThread) {
        WaitForSingleObject(m_hThread, 5000);
        CloseHandle(m_hThread);
        m_hThread = nullptr;
    }
    m_ready = false;
}

void GeminiPrintBackend::Cancel() {
    CConPTY* pty = m_pActivePty;
    if (pty) pty->Destroy();
    HANDLE job = m_hActiveJob;
    if (job) CloseHandle(job);
    m_hActiveJob = nullptr;
    HANDLE process = m_hActiveProcess;
    if (process) TerminateProcess(process, 1);
}

bool GeminiPrintBackend::IsReady() const {
    return m_ready && !m_busy.load();
}

unsigned __stdcall GeminiPrintBackend::WorkerProc(void* lpParam) {
    std::unique_ptr<WorkerCtx> ctx(static_cast<WorkerCtx*>(lpParam));
    try {
        ctx->self->RunOneTurn(ctx->prompt);
    } catch (const std::exception& ex) {
        std::string msg = ex.what();
        ctx->self->Emit(L"[Gemini backend exception] " +
                        NormalizeNewlines(Utf8ToWide(msg.c_str(), (int)msg.size())) + L"\r\n");
    } catch (...) {
        ctx->self->Emit(L"[Gemini backend exception] Unknown backend error.\r\n");
    }
    ctx->self->m_busy = false;
    ctx->self->m_running = false;
    if (ctx->self->m_turnDoneCb) ctx->self->m_turnDoneCb();
    return 0;
}

void GeminiPrintBackend::RunOneTurn(const std::wstring& prompt) {
    ManagerEnvironment env = EnsureManagerEnvironment();
    EnsureManagerAppContextFiles();
    std::wstring appContextDir = ManagerAppContextDir();
    std::wstring userRequest = TrimCopy(prompt);
    if (userRequest == L"정팀장") {
        userRequest =
            L"Reply in Korean: I am Jeong Team Lead and I am awake. Ask what to do next. "
            L"Do not say that Gemini is the manager; Gemini is only the backend provider. "
            L"Do not address the user as any internal supervisor/review role. "
            L"Do not use tools.";
    } else if (!userRequest.empty() &&
        userRequest.find_first_of(L" \t\r\n,.;:!?？！，。") == std::wstring::npos &&
        userRequest.size() <= 12) {
        userRequest =
            L"Reply briefly and helpfully in Korean to this short user message. "
            L"If it is only a name or call sign, acknowledge it and ask what to do next. "
            L"Do not use tools.\nUser message:\n" + userRequest;
    }
    std::wstring managerPrompt = BuildProviderPrompt(L"Gemini", userRequest.empty() ? prompt : userRequest, env);
    managerPrompt += L"\nGemini backend notes:\n";
    managerPrompt += L"- Gemini is only the backend provider name; the user-facing actor name remains 정팀장(manager).\n";
    managerPrompt += L"- App-only context directory: " + appContextDir + L"\n";
    managerPrompt += L"- Default context files: APP_CONTEXT.md, BACKEND_POLICY.md, SESSION_STATE.md, RECENT_FAILURES.md.\n";

    SetEnvironmentVariableW(L"CI", L"true");
    SetEnvironmentVariableW(L"GEMINI_CLI_TRUST_WORKSPACE", L"true");
    EnsureGeminiAuthEnvironment();

    GeminiSettings geminiSettings = AppSettings::LoadGeminiSettings();
    std::wstring args;
    if (!geminiSettings.model.empty()) {
        args += L" -m " + QuoteArg(geminiSettings.model);
    }
    // Gemini CLI requires a non-empty --prompt value to enter headless mode.
    // The real manager prompt is still streamed through stdin to avoid the
    // Windows command-line length limit.
    args += L" -p \" \"";
    args += L" -o stream-json --skip-trust --approval-mode=yolo";

    std::wstring pApp, pArg;
    if (m_geminiPath.directExe) {
        pApp = m_geminiPath.path;
        pArg = args.substr(1); // leading space 제거
    } else {
        pApp = L"cmd.exe";
        pArg = L"/c \"\"" + m_geminiPath.path + L"\"" + args + L"\"";
    }

    std::wstring root = env.root.empty() ? FindProjectRoot() : env.root;

    std::string rawBuf;  // chunk-boundary 처리용 raw 버퍼
    std::string lineBuf; // ANSI 제거 후 라인 누적

    auto handleOutput = [&](const char* data, DWORD size) -> bool {
        if (!data) {
            if (!lineBuf.empty()) HandleJsonLine(lineBuf);
            lineBuf.clear();
            return false;
        }
        rawBuf.append(data, size);
        size_t i = 0;
        while (i < rawBuf.size()) {
            unsigned char ch = (unsigned char)rawBuf[i];
            if (ch == 0x1b) {
                // 청크 경계에서 시퀀스가 잘린 경우 다음 청크까지 보류
                if (i + 1 >= rawBuf.size()) { rawBuf = rawBuf.substr(i); return true; }
                unsigned char nx = (unsigned char)rawBuf[i + 1];
                if (nx == '[') {
                    // CSI: 최종 바이트(0x40–0x7E)까지 스킵
                    size_t j = i + 2;
                    while (j < rawBuf.size() && ((unsigned char)rawBuf[j] < 0x40 || (unsigned char)rawBuf[j] > 0x7E)) j++;
                    if (j >= rawBuf.size()) { rawBuf = rawBuf.substr(i); return true; }
                    i = j + 1;
                } else if (nx == ']') {
                    // OSC: ST(ESC \) 또는 BEL까지 스킵
                    size_t j = i + 2;
                    bool found = false;
                    while (j < rawBuf.size()) {
                        if (rawBuf[j] == '\x07') { j++; found = true; break; }
                        if (rawBuf[j] == '\x1b' && j + 1 < rawBuf.size() && rawBuf[j + 1] == '\\') { j += 2; found = true; break; }
                        j++;
                    }
                    if (!found) { rawBuf = rawBuf.substr(i); return true; }
                    i = j;
                } else {
                    i += 2; // ESC + 단일 문자
                }
            } else if (rawBuf[i] == '\n') {
                if (!lineBuf.empty()) HandleJsonLine(lineBuf);
                lineBuf.clear();
                i++;
            } else if (rawBuf[i] == '\r') {
                i++;
            } else {
                lineBuf += rawBuf[i++];
            }
        }
        rawBuf.clear();
        return true;
    };

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE outRead = nullptr, outWrite = nullptr;
    HANDLE inRead = nullptr, inWrite = nullptr;
    if (!CreatePipe(&outRead, &outWrite, &sa, 0) ||
        !CreatePipe(&inRead, &inWrite, &sa, 0)) {
        if (outRead) CloseHandle(outRead);
        if (outWrite) CloseHandle(outWrite);
        if (inRead) CloseHandle(inRead);
        if (inWrite) CloseHandle(inWrite);
        Emit(L"[Gemini 프로세스 파이프 생성 실패]\r\n");
        return;
    }
    SetHandleInformation(outRead, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(inWrite, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = inRead;
    si.hStdOutput = outWrite;
    si.hStdError = outWrite;

    PROCESS_INFORMATION pi{};
    std::wstring commandLine;
    LPCWSTR appName = nullptr;
    if (m_geminiPath.directExe) {
        appName = m_geminiPath.path.c_str();
        commandLine = QuoteArg(m_geminiPath.path) + args;
    } else {
        appName = nullptr;
        commandLine = L"cmd.exe /d /s /c \"\"" + m_geminiPath.path + L"\"" + args + L"\"";
    }

    std::vector<wchar_t> mutableCmd(commandLine.begin(), commandLine.end());
    mutableCmd.push_back(L'\0');
    BOOL created = CreateProcessW(
        appName,
        mutableCmd.data(),
        nullptr,
        nullptr,
        TRUE,
        CREATE_NO_WINDOW,
        nullptr,
        root.empty() ? nullptr : root.c_str(),
        &si,
        &pi);

    if (!created) {
        DWORD error = GetLastError();
        CloseHandle(outWrite);
        CloseHandle(inRead);
        CloseHandle(inWrite);
        CloseHandle(outRead);
        Emit(L"[Gemini 프로세스 시작 실패] Windows 오류 코드: " + std::to_wstring(error) + L"\r\n");
        return;
    }

    CloseHandle(outWrite);
    CloseHandle(inRead);
    std::string promptUtf8 = WideToUtf8(managerPrompt);
    DWORD startTick = GetTickCount();
    bool firstOutputSeen = false;
    AppendGeminiDiag("start",
        "\"model\":\"" + WideToUtf8(geminiSettings.model.empty() ? L"(default)" : geminiSettings.model) +
        "\",\"prompt_bytes\":" + std::to_string(promptUtf8.size()) +
        ",\"cmd\":\"" + WideToUtf8(commandLine) + "\"");
    DWORD written = 0;
    if (!promptUtf8.empty()) {
        WriteFile(inWrite, promptUtf8.data(), static_cast<DWORD>(promptUtf8.size()), &written, nullptr);
    }
    AppendGeminiDiag("stdin_written", "\"bytes\":" + std::to_string(written));
    CloseHandle(inWrite);

    m_hActiveProcess = pi.hProcess;
    CloseHandle(pi.hThread);

    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION info{};
        info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (SetInformationJobObject(job, JobObjectExtendedLimitInformation, &info, sizeof(info)) &&
            AssignProcessToJobObject(job, pi.hProcess)) {
            m_hActiveJob = job;
        } else {
            CloseHandle(job);
            job = nullptr;
        }
    }

    static constexpr DWORD kQuotaRetryTimeoutMs = 10000; // 429 첫 수신 후 10초 초과 시 종료
    char buf[32768];
    DWORD read = 0;
    while (ReadFile(outRead, buf, sizeof(buf), &read, nullptr) && read > 0) {
        if (!firstOutputSeen) {
            firstOutputSeen = true;
            AppendGeminiDiag("first_output", "\"elapsed_ms\":" + std::to_string(GetTickCount() - startTick) +
                                             ",\"bytes\":" + std::to_string(read));
        }
        handleOutput(buf, read);
        if (m_terminalErrorEmitted) {
            break;
        }
        if (m_quotaFirstSeenTick != 0 && !m_assistantTextEmitted &&
            GetTickCount() - m_quotaFirstSeenTick > kQuotaRetryTimeoutMs) {
            AppendGeminiDiag("quota_timeout", "\"elapsed_since_quota_ms\":" +
                             std::to_string(GetTickCount() - m_quotaFirstSeenTick));
            Cancel();
            break;
        }
    }
    handleOutput(nullptr, 0);
    if (m_quotaErrorEmitted && !m_resultSuccess && !m_assistantTextEmitted && !m_terminalErrorEmitted) {
        Emit(L"Gemini: 용량 초과, 이번 턴 응답 없음\r\n");
    }
    AppendGeminiDiag("process_done",
        "\"elapsed_ms\":" + std::to_string(GetTickCount() - startTick) +
        ",\"assistant_text\":" + std::string(m_assistantTextEmitted ? "true" : "false") +
        ",\"result_success\":" + std::string(m_resultSuccess ? "true" : "false") +
        ",\"quota_seen\":" + std::string(m_quotaErrorEmitted ? "true" : "false") +
        ",\"capacity_seen\":" + std::string(m_capacityErrorSeen ? "true" : "false"));
    CloseHandle(outRead);
    WaitForSingleObject(pi.hProcess, 5000);
    if (m_hActiveJob) {
        CloseHandle(m_hActiveJob);
        m_hActiveJob = nullptr;
    }
    CloseHandle(pi.hProcess);
    m_hActiveProcess = nullptr;
}

void GeminiPrintBackend::HandleJsonLine(const std::string& line) {
    if (line.find("color support not detected") != std::string::npos ||
        line.find("256-color support not detected") != std::string::npos ||
        line.find("YOLO mode is enabled") != std::string::npos ||
        line.find("Ripgrep is not available") != std::string::npos ||
        line.find("Falling back to GrepTool") != std::string::npos) {
        return;
    }

    if (line.find("xterm.js: Parsing error") != std::string::npos) {
        m_suppressXtermDump = true;
        return;
    }
    if (m_suppressXtermDump) {
        if (line == "}") {
            m_suppressXtermDump = false;
        }
        return;
    }
    if (line.find("AttachConsole failed") != std::string::npos ||
        line.find("conpty_console_list_agent.js") != std::string::npos ||
        line.find("getConsoleProcessList") != std::string::npos ||
        line.find("Path not in workspace") != std::string::npos ||
        line.find("@lydell") != std::string::npos ||
        line.find("node-pty") != std::string::npos) {
        m_suppressXtermDump = true;
        return;
    }
    if (m_terminalErrorEmitted &&
        (line.rfind("    at ", 0) == 0 ||
         line.find("Node.js v") != std::string::npos ||
         line.find("Module.") != std::string::npos ||
         line.find("node:internal") != std::string::npos ||
         line.find("^") != std::string::npos)) {
        return;
    }

    static constexpr int kMaxQuotaRetries = 3;
    auto emitQuotaError = [&]() {
        ++m_quotaRetryCount;
        if (!m_quotaErrorEmitted) {
            m_quotaErrorEmitted = true;
            m_quotaFirstSeenTick = GetTickCount();
        }
        if (m_quotaRetryCount > kMaxQuotaRetries && !m_terminalErrorEmitted) {
            m_terminalErrorEmitted = true;
            Emit(L"Gemini: 용량 초과, 이번 턴 건너뜀\r\n");
        }
    };

    auto emitRetryNotice = [&]() {
        if (m_retryNoticeEmitted || m_assistantTextEmitted) return;
        m_retryNoticeEmitted = true;
        Emit(L"Gemini: 용량 초과, 대기 중...\r\n");
    };

    if (line.find("MODEL_CAPACITY_EXHAUSTED") != std::string::npos ||
        line.find("No capacity available for model") != std::string::npos) {
        m_capacityErrorSeen = true;
        emitQuotaError();
        emitRetryNotice();
        return;
    }

    if (line.find("QUOTA_EXHAUSTED") != std::string::npos ||
        line.find("TerminalQuotaError") != std::string::npos ||
        line.find("status: 429") != std::string::npos ||
        line.find("\"status\": 429") != std::string::npos ||
        line.find("status:429") != std::string::npos ||
        line.find("code: 429") != std::string::npos ||
        line.find("\"code\": 429") != std::string::npos ||
        line.find("code:429") != std::string::npos ||
        line.find("Too Many Requests") != std::string::npos ||
        line.find("quota will reset") != std::string::npos ||
        line.find("exhausted your capacity") != std::string::npos) {
        emitQuotaError();
        emitRetryNotice();
        return;
    }
    if (m_quotaErrorEmitted) {
        size_t first = line.find_first_not_of(" \t");
        if (first == std::string::npos || line[first] != '{') {
            return;
        }
    }
    if (m_quotaErrorEmitted &&
        (line.rfind("    at ", 0) == 0 ||
         line.rfind("    cause:", 0) == 0 ||
         line.rfind("  cause:", 0) == 0 ||
         line.rfind("  retryDelayMs:", 0) == 0 ||
         line.rfind("  reason:", 0) == 0 ||
         line.rfind("  data:", 0) == 0 ||
         line.rfind("  error:", 0) == 0 ||
         line.find("status: 429") != std::string::npos ||
         line.find("code: 429") != std::string::npos ||
         LooksLikeGeminiInternalDump(line) ||
         line == "}" ||
         line == "  }," ||
         line == "}") ) {
        return;
    }
    if (LooksLikeGeminiInternalDump(line)) {
        return;
    }

    Json::CharReaderBuilder builder;
    std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
    Json::Value root;
    std::string errs;
    if (!reader->parse(line.data(), line.data() + line.size(), &root, &errs)) {
        return;
    }
    if (!root.isObject()) return;

    if (root.isMember("session_id")) {
        m_sessionId = root.get("session_id", "").asString();
    }
    if (root.isMember("model") && root["model"].isString()) {
        std::string model = root["model"].asString();
        m_currentModel = Utf8ToWide(model.c_str(), static_cast<int>(model.size()));
    }

    if (root.get("type", "").asString() == "message" &&
        root.get("role", "").asString() != "assistant") {
        return;
    }
    if (root.get("type", "").asString() == "result" &&
        root.get("status", "").asString() == "success") {
        m_resultSuccess = true;
        return;
    }

    std::string text;
    if (root.isMember("text")) text = root.get("text", "").asString();
    if (text.empty() && root["content"].isString()) text = root["content"].asString();
    if (text.empty() && root["message"].isObject() && root["message"]["content"].isString()) {
        text = root["message"]["content"].asString();
    }
    if (!text.empty()) {
        m_assistantTextEmitted = true;
        Emit(NormalizeNewlines(Utf8ToWide(text.c_str(), (int)text.size())));
    }
}

} // namespace orange
