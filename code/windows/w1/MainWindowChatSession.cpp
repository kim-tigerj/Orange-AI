#include "MainWindow.h"

#include "Utils.h"

#include <algorithm>

namespace orange {

namespace {

constexpr int kInitialChatBlockLimit = 140;
constexpr int kOlderChatBlockPageSize = 80;

bool IsPendingResponseRole(const std::wstring& role) {
    return role == L"pending-response";
}

std::wstring FolderOf(const std::wstring& path) {
    size_t pos = path.find_last_of(L"\\/");
    if (pos == std::wstring::npos) return L".";
    return path.substr(0, pos);
}

std::wstring FileUriForFolder(const std::wstring& path) {
    std::wstring folder = FolderOf(path);
    std::wstring uri = L"file:///";
    for (wchar_t ch : folder) {
        if (ch == L'\\') uri += L'/';
        else if (ch == L' ') uri += L"%20";
        else uri += ch;
    }
    return uri;
}

std::wstring FileUriForPath(const std::wstring& path) {
    std::wstring uri = L"file:///";
    for (wchar_t ch : path) {
        if (ch == L'\\') uri += L'/';
        else if (ch == L' ') uri += L"%20";
        else uri += ch;
    }
    return uri;
}

std::string NewChatKey() {
    static LONG counter = 0;
    SYSTEMTIME st{};
    GetLocalTime(&st);
    char buf[96];
    sprintf_s(buf, "chat-%04u%02u%02u-%02u%02u%02u-%lu-%ld",
              st.wYear, st.wMonth, st.wDay,
              st.wHour, st.wMinute, st.wSecond,
              GetCurrentProcessId(), InterlockedIncrement(&counter));
    return buf;
}

} // namespace

void CMainWindow::QueueLoadOlderHistory() {
    if (m_loadingOlderHistory) return;
    if (m_currentChatKey.empty()) return;
    if (!m_currentChatSession.historyPartial) return;
    PostMessageW(m_hwnd, WM_APP + 5, 0, 0);
}

void CMainWindow::LoadOlderHistory() {
    if (m_loadingOlderHistory) return;
    if (m_currentChatKey.empty() || !m_currentChatSession.historyPartial) return;
    if (m_currentChatSession.blocks.empty()) return;

    int beforeSeq = m_currentChatSession.blocks.front().seq;
    if (beforeSeq <= 0) {
        m_currentChatSession.historyPartial = false;
        return;
    }

    m_loadingOlderHistory = true;
    float oldScroll = m_view ? m_view->ScrollY() : 0.0f;
    float oldHeight = m_view ? m_view->TotalHeight() : 0.0f;

    auto older = Persistence::LoadBlocksBefore(m_currentChatKey, beforeSeq, kOlderChatBlockPageSize);
    if (older.empty()) {
        m_currentChatSession.historyPartial = false;
        m_loadingOlderHistory = false;
        RenderChatSession(m_currentChatSession, false);
        if (m_view) m_view->SetScrollY(oldScroll);
        return;
    }

    m_currentChatSession.blocks.insert(m_currentChatSession.blocks.begin(),
                                       older.begin(), older.end());
    m_currentChatSession.historyPartial =
        m_currentChatSession.totalBlockCount > (int)m_currentChatSession.blocks.size();

    RenderChatSession(m_currentChatSession, false);
    if (m_view) {
        float newHeight = m_view->TotalHeight();
        m_view->SetScrollY(oldScroll + (newHeight - oldHeight));
    }
    m_loadingOlderHistory = false;
}

void CMainWindow::CreateChatDraft(const std::string& goalId,
                                  const std::string& projectId) {
    std::string cleanGoal;
    std::string cleanProject;
    if (!goalId.empty()) {
        cleanGoal = CGoal::SanitizeId(goalId);
        cleanProject = projectId.empty() ? CProject::kDefaultId : CGoal::SanitizeId(projectId);
        SetEnvironmentVariableA("ORANGE_CODE_GOAL_ID", cleanGoal.c_str());
        SetEnvironmentVariableA("ORANGE_CODE_PROJECT_ID", cleanProject.c_str());
    } else {
        SetEnvironmentVariableA("ORANGE_CODE_GOAL_ID", nullptr);
        SetEnvironmentVariableA("ORANGE_CODE_PROJECT_ID", nullptr);
    }

    m_activeGoalId = cleanGoal;
    m_activeProjectId = cleanProject.empty() ? CProject::kDefaultId : cleanProject;
    m_currentChatKey = NewChatKey();
    SetEnvironmentVariableA("ORANGE_CODE_SESSION_KEY", m_currentChatKey.c_str());

    m_currentChatSession = ChatSession{};
    m_currentChatSession.title = "새 대화";

    m_currentAssistantRole.clear();
    m_lastPrompt.clear();
    m_lastBackendLabel.clear();
    m_waitingForFirstOutput = false;
    m_pendingAssistantBlock = (size_t)-1;
    m_pendingBackendCount = 0;
    if (m_view) {
        m_view->SetBusy(false);
        m_view->Clear();
    }
    if (m_sidebar) {
        m_sidebar->SetCurrentChat(m_currentChatKey);
    }
    UpdateWindowTitle();
    if (m_input) {
        m_input->SetText(L"");
        SetFocus(m_input->Hwnd());
    }
}

void CMainWindow::EnsureCurrentGlobalChat() {
    if (!m_currentChatKey.empty() && m_currentChatKey != "default") return;
    wchar_t envSession[256] = L"";
    if (GetEnvironmentVariableW(L"ORANGE_CODE_SESSION_KEY", envSession, 256) > 0 && envSession[0] != L'\0') {
        std::string key = WideToUtf8(envSession);
        if (!key.empty() && key != "default") {
            m_currentChatKey = key;
            m_currentChatSession = Persistence::Load(key);
            UpdateWindowTitle();
            return;
        }
    }

    m_currentChatKey = NewChatKey();
    SetEnvironmentVariableA("ORANGE_CODE_SESSION_KEY", m_currentChatKey.c_str());
    SetEnvironmentVariableA("ORANGE_CODE_GOAL_ID", nullptr);
    SetEnvironmentVariableA("ORANGE_CODE_PROJECT_ID", nullptr);
    m_activeGoalId.clear();
    m_activeProjectId = CProject::kDefaultId;
    m_currentChatSession = ChatSession{};
    m_currentChatSession.title = "새 대화";
    m_currentAssistantRole.clear();
    m_lastPrompt.clear();
    m_lastBackendLabel.clear();
    m_waitingForFirstOutput = false;
    m_pendingAssistantBlock = (size_t)-1;
    m_pendingBackendCount = 0;
    if (m_view) {
        m_view->SetBusy(false);
        m_view->Clear();
    }
    UpdateWindowTitle();
    RefillSidebar();
}

void CMainWindow::OpenChatSession(const std::string& key,
                                  const std::string& goalId,
                                  const std::string& projectId) {
    if (key.empty()) return;
    SetEnvironmentVariableA("ORANGE_CODE_SESSION_KEY", key.c_str());
    if (!goalId.empty()) {
        m_activeGoalId = CGoal::SanitizeId(goalId);
        m_activeProjectId = projectId.empty() ? CProject::kDefaultId : CGoal::SanitizeId(projectId);
        SetEnvironmentVariableA("ORANGE_CODE_GOAL_ID", m_activeGoalId.c_str());
        SetEnvironmentVariableA("ORANGE_CODE_PROJECT_ID", m_activeProjectId.c_str());
    } else {
        m_activeGoalId.clear();
        m_activeProjectId = CProject::kDefaultId;
        SetEnvironmentVariableA("ORANGE_CODE_GOAL_ID", nullptr);
        SetEnvironmentVariableA("ORANGE_CODE_PROJECT_ID", nullptr);
    }

    m_currentChatKey = key;
    m_currentChatSession = Persistence::LoadRecent(key, kInitialChatBlockLimit);
    RecoverInterruptedResponse(m_currentChatSession, key);
    m_currentAssistantRole.clear();
    m_lastPrompt.clear();
    m_lastBackendLabel.clear();
    m_waitingForFirstOutput = false;
    m_pendingAssistantBlock = (size_t)-1;
    m_pendingBackendCount = 0;
    m_loadingOlderHistory = false;
    if (m_view) m_view->SetBusy(false);
    RenderChatSession(m_currentChatSession);
    UpdateWindowTitle();
    if (m_sidebar) {
        m_sidebar->SetCurrentChat(key);
        m_sidebar->SetSelectedIdx(m_sidebar->FindChatIndex(key, m_activeGoalId, m_activeProjectId));
    }
}

void CMainWindow::RenderChatSession(const ChatSession& session, bool scrollLatest) {
    if (!m_view) return;
    m_view->SetBulkUpdate(true);
    m_view->SetScreenLogSuppressed(true);
    m_view->Clear();
    if (session.historyPartial && session.totalBlockCount > (int)session.blocks.size()) {
        int hidden = session.totalBlockCount - (int)session.blocks.size();
        m_view->NewBlock(L"task");
        m_view->AppendText(L"이전 대화 " + std::to_wstring(hidden) +
                           L"개는 아직 화면에 불러오지 않았습니다.\n\n"
                           L"- 현재는 최근 " + std::to_wstring((int)session.blocks.size()) +
                           L"개만 표시합니다.\n"
                           L"- 다음 단계: 위로 스크롤할 때 이전 대화 자동 로드");
    }
    for (const auto& block : session.blocks) {
        if (block.text.empty()) continue;
        if (IsPendingResponseRole(block.role)) continue;
        if (block.role == L"attachment") {
            auto records = AttachmentStore::LoadManifest(block.text);
            if (!records.empty()) {
                std::wstring manifest = AttachmentStore::ManifestPath(block.text);
                OrangeView::AttachmentViewBlock viewBlock;
                viewBlock.manifestPath = manifest;
                viewBlock.manifestFileUrl = FileUriForPath(manifest);
                viewBlock.manifestFolderUrl = FileUriForFolder(manifest);
                for (const auto& rec : records) {
                    OrangeView::AttachmentViewItem item;
                    item.kind = rec.kind;
                    item.name = rec.name;
                    item.sizeLabel = std::to_wstring(rec.sizeBytes / 1024) + L" KB";
                    item.originalPath = rec.originalPath;
                    item.storedPath = rec.storedPath;
                    item.thumbnailUrl = rec.thumbnailPath.empty() ? L"" : FileUriForPath(rec.thumbnailPath);
                    item.fileUrl = FileUriForPath(rec.storedPath);
                    item.folderUrl = FileUriForFolder(rec.storedPath);
                    viewBlock.items.push_back(std::move(item));
                }
                m_view->AddAttachmentBlock(viewBlock);
            }
            continue;
        }
        m_view->NewBlock(block.role);
        m_view->AppendText(block.text);
    }
    m_view->SetScreenLogSuppressed(false);
    m_view->SetBulkUpdate(false);
    if (scrollLatest) {
        m_view->ScrollLatestIntoView();
    }
}

void CMainWindow::SaveChatBlock(const std::wstring& role, const std::wstring& text) {
    if (text.empty()) return;
    if (m_currentChatKey.empty()) {
        wchar_t envSession[256] = L"";
        if (GetEnvironmentVariableW(L"ORANGE_CODE_SESSION_KEY", envSession, 256) > 0 && envSession[0] != L'\0') {
            m_currentChatKey = WideToUtf8(envSession);
        }
        if (m_currentChatKey.empty() || m_currentChatKey == "default") {
            m_currentChatKey = NewChatKey();
            SetEnvironmentVariableA("ORANGE_CODE_SESSION_KEY", m_currentChatKey.c_str());
        }
        m_currentChatSession = Persistence::Load(m_currentChatKey);
    }
    if (m_currentChatSession.historyPartial) {
        m_currentChatSession = Persistence::Load(m_currentChatKey);
    }

    ChatBlock block;
    block.role = role;
    block.text = text;
    const bool canMerge =
        role != L"user" &&
        role != L"attachment" &&
        !m_currentChatSession.blocks.empty() &&
        m_currentChatSession.blocks.back().role == role;
    if (canMerge) {
        m_currentChatSession.blocks.back().text += text;
    } else {
        m_currentChatSession.blocks.push_back(std::move(block));
    }

    bool titleChanged = false;
    if ((m_currentChatSession.title.empty() || m_currentChatSession.title == "새 대화") && role == L"attachment") {
        m_currentChatSession.title = "첨부";
        titleChanged = true;
    }
    if ((m_currentChatSession.title.empty() || m_currentChatSession.title == "새 대화") && role == L"user") {
        std::wstring title = text;
        size_t nl = title.find_first_of(L"\r\n");
        if (nl != std::wstring::npos) title.resize(nl);
        if (title.size() > 40) title = title.substr(0, 37) + L"...";
        m_currentChatSession.title = WideToUtf8(title);
        titleChanged = true;
    }
    Persistence::Save(m_currentChatSession, m_currentChatKey);
    if (role == L"user" || role == L"attachment" || titleChanged) {
        if (titleChanged) UpdateWindowTitle();
        RefillSidebar();
    }
}

void CMainWindow::MarkResponsePending(const std::wstring& providerLabel) {
    std::wstring text = L"provider=" + providerLabel;
    SaveChatBlock(L"pending-response", text);
}

void CMainWindow::ClearResponsePendingMarkers() {
    if (m_currentChatKey.empty()) return;
    bool changed = false;
    auto& blocks = m_currentChatSession.blocks;
    blocks.erase(
        std::remove_if(blocks.begin(), blocks.end(),
                       [&](const ChatBlock& b) {
                           if (!IsPendingResponseRole(b.role)) return false;
                           changed = true;
                           return true;
                       }),
        blocks.end());
    if (changed) Persistence::Save(m_currentChatSession, m_currentChatKey);
}

bool CMainWindow::RecoverInterruptedResponse(ChatSession& session, const std::string& key) {
    bool hadPending = false;
    session.blocks.erase(
        std::remove_if(session.blocks.begin(), session.blocks.end(),
                       [&](const ChatBlock& b) {
                           if (!IsPendingResponseRole(b.role)) return false;
                           hadPending = true;
                           return true;
                       }),
        session.blocks.end());
    if (!hadPending) return false;
    Persistence::Save(session, key);
    return true;
}

void CMainWindow::ClearCurrentChat() {
    if (m_currentChatKey.empty()) return;
    if (m_pendingBackendCount > 0 || m_buildRunning) {
        MessageBoxW(m_hwnd, L"백엔드가 응답 중입니다. 완료 후 다시 시도하세요.",
                    L"채팅 기록 삭제", MB_OK | MB_ICONINFORMATION);
        return;
    }
    int res = MessageBoxW(m_hwnd,
        L"현재 채팅의 모든 기록을 삭제하시겠습니까?\n삭제된 내용은 복구할 수 없습니다.",
        L"채팅 기록 삭제", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2);
    if (res != IDYES) return;

    Persistence::ClearBlocks(m_currentChatKey);

    m_currentChatSession.blocks.clear();
    m_currentChatSession.totalBlockCount = 0;
    m_currentChatSession.historyPartial = false;
    m_currentAssistantRole.clear();
    m_lastPrompt.clear();
    m_lastBackendLabel.clear();
    m_waitingForFirstOutput = false;
    m_pendingAssistantBlock = (size_t)-1;
    m_pendingBackendCount = 0;
    if (m_view) {
        m_view->SetBusy(false);
        m_view->Clear();
    }
}

} // namespace orange
