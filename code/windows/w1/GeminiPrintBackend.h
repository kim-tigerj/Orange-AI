#pragma once
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <fstream>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <vector>
#include <json/json.h>
#include "IClaudeBackend.h" // Use IBackend interface for consistency
#include "CConPTY.h"
#include "Coordination.h"
#include "Utils.h"

namespace orange {

class GeminiPrintBackend : public IBackend {
public:
    GeminiPrintBackend() = default;
    ~GeminiPrintBackend() override { Stop(); }

    bool Start(OutputCallback outputCb,
               TurnDoneCallback turnDoneCb = {},
               ToolUseCallback toolUseCb   = {},
               ErrorCallback   errorCb     = {},
               RetryCallback   retryCb     = {}) override;

    bool SendPrompt(const std::wstring& prompt) override;

    void Stop() override;
    void Cancel() override;
    bool IsReady() const override;

    std::string CurrentSessionId() const override { return m_sessionId; }
    void SetResumeSessionId(const std::string& id) override { m_sessionId = id; }

private:
    struct GeminiLocation { std::wstring path; bool directExe; };
    struct WorkerCtx { GeminiPrintBackend* self; std::wstring prompt; };

    static GeminiLocation FindGeminiPath();

    static unsigned __stdcall WorkerProc(void* lpParam);
    void RunOneTurn(const std::wstring& prompt);
    void HandleJsonLine(const std::string& line);
    void Emit(std::wstring text) { if (m_outputCb) m_outputCb(std::move(text)); }

    GeminiLocation m_geminiPath;
    std::string m_sessionId;
    OutputCallback m_outputCb;
    TurnDoneCallback m_turnDoneCb;
    ErrorCallback m_errorCb;
    std::atomic<bool> m_busy{false};
    std::atomic<bool> m_running{false};
    bool m_quotaErrorEmitted = false;
    bool m_terminalErrorEmitted = false;
    bool m_suppressXtermDump = false;
    bool m_resultSuccess = false;
    bool m_assistantTextEmitted = false;
    bool m_capacityErrorSeen = false;
    bool m_retryNoticeEmitted = false;
    DWORD m_quotaFirstSeenTick = 0;
    int m_quotaRetryCount = 0;
    std::wstring m_currentModel;
    CConPTY* volatile m_pActivePty = nullptr;
    HANDLE m_hActiveProcess = nullptr;
    HANDLE m_hActiveJob = nullptr;
    HANDLE m_hThread = nullptr;
    bool m_ready = false;
};
} // namespace orange
