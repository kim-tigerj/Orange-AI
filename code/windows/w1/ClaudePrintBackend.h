#pragma once
#include <windows.h>
#include <memory>
#include <string>
#include <functional>
#include <atomic>
#include <process.h> // For _beginthreadex
#include "IClaudeBackend.h" // This should be IBackend.h for common interface
#include "Utils.h"

namespace orange {

class ClaudePrintBackend : public IBackend {
public:
    ClaudePrintBackend() = default;
    ~ClaudePrintBackend() override { Stop(); }

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
    struct WorkerCtx { ClaudePrintBackend* self; std::wstring prompt; };

    static unsigned __stdcall ThreadProc(void* lpParam);
    void RunOneTurn(const std::wstring& prompt);
    void HandleOutput(const std::string& line);
    void Emit(std::wstring text) { if (m_outputCb) m_outputCb(std::move(text)); }

    std::wstring m_claudePath;
    bool m_claudeDirectExe = false;
    std::string m_sessionId;
    OutputCallback m_outputCb;
    TurnDoneCallback m_turnDoneCb;
    RetryCallback m_retryCb;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_busy{false};
    HANDLE m_hProcess = nullptr;
    HANDLE m_hThread = nullptr;
    HANDLE m_hJob = nullptr;
    bool m_ready = false;
};

} // namespace orange
