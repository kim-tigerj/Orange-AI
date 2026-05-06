#pragma once

#include <windows.h>
#include <atomic>
#include <memory>
#include <string>

#include "IClaudeBackend.h"
#include "Utils.h"

namespace orange {

class CodexPrintBackend : public IBackend {
public:
    CodexPrintBackend() = default;
    ~CodexPrintBackend() override { Stop(); }

    bool Start(OutputCallback outputCb,
               TurnDoneCallback turnDoneCb = {},
               ToolUseCallback toolUseCb   = {},
               ErrorCallback   errorCb     = {},
               RetryCallback   retryCb     = {}) override;

    bool SendPrompt(const std::wstring& prompt) override;
    void Stop() override;
    void Cancel() override;
    bool IsReady() const override;

private:
    struct CodexLocation { std::wstring path; bool directExe; };
    struct WorkerCtx { CodexPrintBackend* self; std::wstring prompt; };

    static CodexLocation FindCodexPath();
    static unsigned __stdcall WorkerProc(void* lpParam);
    void RunOneTurn(const std::wstring& prompt);
    void HandleJsonLine(const std::string& line);
    void Emit(std::wstring text) { if (m_outputCb) m_outputCb(std::move(text)); }

    CodexLocation m_codexPath;
    OutputCallback m_outputCb;
    TurnDoneCallback m_turnDoneCb;
    std::atomic<bool> m_busy{false};
    HANDLE m_hProcess = nullptr;
    HANDLE m_hThread = nullptr;
    HANDLE m_hJob = nullptr;
    bool m_ready = false;
};

} // namespace orange
