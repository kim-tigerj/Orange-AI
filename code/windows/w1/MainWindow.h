#pragma once
#include <windows.h>
#include <memory>
#include <string>
#include <vector>
#include "OrangeInput.h"
#include "CSidebar.h"
#include "OrangeView.h"
#include "BackendManager.h"
#include "AttachmentStore.h"
#include "Persistence.h"

namespace orange {

struct StartupOptions {
    std::wstring testPrompt;
    std::wstring testResponse;
    std::wstring testCapturePath;
    UINT testCaptureDelayMs = 2500;
    UINT testExitMs = 6000;
    UINT testResponseDelayMs = 900;
    bool testPasteClipboardImage = false;
    std::wstring testAttachmentPath;
    bool useClaude = true;
    bool useGemini = false;
    bool useCodex = false;
    std::string managerProvider = "claude";
    bool useMockBackend = false;
    bool backendFromFlag = false;  // true when --gemini/--codex/--test-backend overrode saved settings
    bool testShowRetry = false;    // inject fake rate-limit → show retry card for UI smoke test
};

struct BuildResultPacket {
    int exitCode = -1;
    DWORD elapsedMs = 0;
    std::wstring output;
};

class CMainWindow {
public:
    CMainWindow();
    ~CMainWindow();

    void SetStartupOptions(StartupOptions options);
    bool Create(HINSTANCE hInst, int nShow);
    void Run();
    HWND Hwnd() const { return m_hwnd; }

private:
    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
    
    void LayoutControls();
    void RefillSidebar();
    void UpdateWindowTitle();
    void CreateGlobalChat();
    void CreateChatDraft(const std::string& goalId = {},
                         const std::string& projectId = {});
    void RenameChatSession(const CSidebar::Row& row);
    void RenameSidebarMeta(const CSidebar::Row& row);
    void ShowSidebarContextMenu(int screenX, int screenY);
    void QueueSidebarAction(const CSidebar::Row& row);
    void RunSidebarAction();
    void QueueLoadOlderHistory();
    void LoadOlderHistory();
    void EnsureCurrentGlobalChat();
    void OpenChatSession(const std::string& key,
                         const std::string& goalId = {},
                         const std::string& projectId = {});
    void RenderChatSession(const ChatSession& session, bool scrollLatest = true);
    void SubmitFromInput();
    void DispatchPrompt(const std::wstring& prompt);
    void RunStartupBackendSync();
    void RunAutoResumePrompt();
    void AppendWorkCard(const std::wstring& prompt, bool useClaude, bool useGemini, bool useCodex,
                        const std::wstring& plan) const;
    void AppendVerificationCard() const;
    void StartBuildCommand();
    void AppendBuildResultCard(int exitCode, DWORD elapsedMs, const std::wstring& output);
    void RelaunchAfterSuccessfulBuild();
    void RunCaptureCommand();
    void AppendCaptureResultCard(HRESULT hr, const std::wstring& path, ULONGLONG fileSize, DWORD elapsedMs);
    void HandleDroppedFiles(HDROP drop);
    void AddAttachmentPaths(const std::vector<std::wstring>& paths);
    void SelectAttachmentFiles();
    bool HasConfiguredGoal() const;
    void ActivateGoal(const std::string& goalId);
    void EnsureGoalChat();
    void SaveChatBlock(const std::wstring& role, const std::wstring& text);
    void MarkResponsePending(const std::wstring& providerLabel);
    void ClearResponsePendingMarkers();
    bool RecoverInterruptedResponse(ChatSession& session, const std::string& key);
    bool CreateGoalFromTitle(const std::wstring& title);
    bool CreateProjectFromTitle(const std::wstring& title);
    void AppendAttachmentCard(const std::vector<AttachmentRecord>& records);
    bool PasteClipboardImage();
    std::wstring BackendLabel(bool useClaude, bool useGemini, bool useCodex) const;
    std::wstring ManagerProviderLabel() const;
    void RunTestCapture();
    void ClearCurrentChat();
    void AppendBackendOutput(const std::string& chatKey, CBackendManager::OutputSource source, const std::wstring& text);
    void HandleBackendDone(const std::string& chatKey);
    void ResetMeetingState();
    void AppendMeetingResultCard();
    bool TryHandleToolRequest(const std::string& chatKey, CBackendManager::OutputSource source, const std::wstring& text);
    bool HandleDelegateTool(const std::string& chatKey,
                            CBackendManager::OutputSource source,
                            const Json::Value& args);
    void HandleExecAdminTool(const std::string& chatKey, CBackendManager::OutputSource source,
                             const std::wstring& command, const std::wstring& cwd, const std::wstring& reason);
    bool IsProviderSelected(CBackendManager::OutputSource source) const;
    void TryRelayProviderMention(CBackendManager::OutputSource source, const std::wstring& text);
    void HandleProviderRelayTimeout();
    std::wstring SourceRole(CBackendManager::OutputSource source) const;
    std::wstring SourceLabel(CBackendManager::OutputSource source) const;
    void ToggleAutoLoop(const std::wstring& arg);
    void TickAutoLoop();

    HWND m_hwnd;
    HINSTANCE m_hInst;
    StartupOptions m_startupOptions;
    
    std::unique_ptr<COrangeInput> m_input;
    std::unique_ptr<CSidebar> m_sidebar;
    std::unique_ptr<OrangeView> m_view;
    std::unique_ptr<CBackendManager> m_backend;
    
    HWND m_hChkClaude;
    HWND m_hChkGemini;
    HWND m_hChkCodex;
    HWND m_hBtnBold, m_hBtnItalic, m_hBtnCode, m_hBtnTable, m_hBtnClear;
    HWND m_hUserLabel = nullptr;
    HFONT m_hUiFont = nullptr;
    std::wstring m_currentAssistantRole;
    std::wstring m_lastPrompt;
    std::wstring m_lastBackendLabel;
    std::string m_activeGoalId;
    std::string m_activeProjectId = "default";
    std::string m_currentChatKey;
    ChatSession m_currentChatSession;
    bool m_buildRunning = false;
    bool m_captureAfterBuild = false;
    bool m_relaunchPending = false;
    bool m_forceRelaunchAfterBuild = false;
    std::string m_deferredOpenChatKey;
    bool m_waitingForFirstOutput = false;
    // rate limit 자동 재시도 상태
    std::wstring m_retryPrompt;
    int m_retryAttempt = 0;
    size_t m_retryBlockIdx = (size_t)-1;
    int m_retryRemaining = 0;
    size_t m_pendingAssistantBlock = (size_t)-1;
    int m_pendingBackendCount = 0;
    bool m_loadingOlderHistory = false;
    bool m_startupBackendSyncDone = false;
    bool m_meetingActive = false;
    std::wstring m_meetingIssue;
    std::wstring m_meetingRoster;
    std::wstring m_meetingManager;
    std::wstring m_meetingClaude;
    std::wstring m_meetingGemini;
    std::wstring m_meetingCodex;
    bool m_buildRequestedThisTurn = false;
    // Auto-loop: periodic provider trigger
    bool m_autoLoopEnabled = false;
    int m_autoLoopIntervalSec = 300;
    // Sequential chain: manager responds first, reviewers see manager's response
    std::vector<CBackendManager::OutputSource> m_seqQueue;
    std::wstring m_seqManagerResponse;
    std::wstring m_seqBasePrompt;
    CBackendManager::OutputSource m_seqManagerSource = CBackendManager::OutputSource::System;
    bool m_seqFinalManagerRequested = false;
    std::vector<std::wstring> m_providerRelayKeys;
    bool m_providerRelayActive = false;
    CBackendManager::OutputSource m_providerRelayTarget = CBackendManager::OutputSource::System;
    CBackendManager::OutputSource m_providerRelayFallback = CBackendManager::OutputSource::System;
    std::wstring m_providerRelayTargetLabel;
    std::wstring m_providerRelayFallbackLabel;
    std::wstring m_providerRelayExcerpt;

    struct SidebarAction {
        CSidebar::RowKind kind = CSidebar::RowKind::Empty;
        std::string chatKey;
        std::string goalId;
        std::string projectId;
    };
    SidebarAction m_pendingSidebarAction;
};

} // namespace orange
