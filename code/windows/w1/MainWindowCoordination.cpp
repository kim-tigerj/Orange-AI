#include "MainWindow.h"

#include "ChatSessionOps.h"
#include "Delegation.h"
#include "Utils.h"

#include <algorithm>
#include <cwctype>

namespace orange {

namespace {

std::wstring CurrentExeBuildLabel() {
    wchar_t path[MAX_PATH] = L"";
    DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return L"unknown";

    WIN32_FILE_ATTRIBUTE_DATA attr{};
    if (!GetFileAttributesExW(path, GetFileExInfoStandard, &attr)) return L"unknown";

    FILETIME localFt{};
    SYSTEMTIME st{};
    if (!FileTimeToLocalFileTime(&attr.ftLastWriteTime, &localFt) ||
        !FileTimeToSystemTime(&localFt, &st)) {
        return L"unknown";
    }

    wchar_t buf[64] = L"";
    swprintf_s(buf, L"%04u-%02u-%02u %02u:%02u",
               st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute);
    return buf;
}

std::wstring ShortPathLabel(const std::wstring& path) {
    if (path.empty()) return L"none";
    if (path.size() <= 54) return path;
    return L"..." + path.substr(path.size() - 51);
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

std::wstring PathWithFolderOpenIcon(const std::wstring& path) {
    if (path.empty()) return L"none";
    return ShortPathLabel(path) + L"\n  [폴더 열기](" + FileUriForFolder(path) + L")";
}

} // namespace

void CMainWindow::AppendWorkCard(const std::wstring& prompt,
                                 bool useClaude,
                                 bool useGemini,
                                 bool useCodex,
                                 const std::wstring& plan) const {
    if (!m_view) return;

    std::wstring backend = BackendLabel(useClaude, useGemini, useCodex);

    std::wstring preview = prompt;
    for (auto& ch : preview) {
        if (ch == L'\r' || ch == L'\n' || ch == L'\t') ch = L' ';
    }
    if (preview.size() > 72) {
        preview.resize(72);
        preview += L"...";
    }

    std::wstring card;
    card += L"작업\n\n";
    card += L"- 요청: " + preview + L"\n";
    card += L"- 백엔드: " + backend + L"\n";
    card += L"- 정팀장: " + ManagerProviderLabel() + L"\n";
    card += L"- 계획: " + (plan.empty() ? L"요청 유형에 맞춰 다음 실행 단계를 정리" : plan) + L"\n";
    card += L"- 상태: 정팀장 응답 대기";

    m_view->NewBlock(L"task");
    m_view->AppendText(card);
}

void CMainWindow::AppendVerificationCard() const {
    if (!m_view) return;

    std::wstring card;
    card += L"검증\n\n";
    card += L"- 응답: 수신 완료\n";
    card += L"- 백엔드: " + (m_lastBackendLabel.empty() ? L"unknown" : m_lastBackendLabel) + L"\n";
    card += L"- 빌드: 현재 실행 파일 " + CurrentExeBuildLabel() + L"\n";
    if (!m_startupOptions.testCapturePath.empty()) {
        card += L"- 캡처: " + PathWithFolderOpenIcon(m_startupOptions.testCapturePath) + L"\n";
    } else {
        card += L"- 캡처: 없음\n";
    }
    card += L"- 다음: build -> mock/capture -> 필요 시 실제 백엔드 1회";

    m_view->NewBlock(L"task");
    m_view->AppendText(card);
}

std::wstring CMainWindow::BackendLabel(bool useClaude, bool useGemini, bool useCodex) const {
    if (m_startupOptions.useMockBackend) {
        return L"mock";
    }
    std::wstring label;
    if (useClaude) label += L"Claude";
    if (useGemini) label += (label.empty() ? L"" : L" + ") + std::wstring(L"Gemini");
    if (useCodex) label += (label.empty() ? L"" : L" + ") + std::wstring(L"Codex");
    if (!label.empty()) return label;
    return L"none";
}

std::wstring CMainWindow::ManagerProviderLabel() const {
    if (m_startupOptions.managerProvider == "gemini") return L"Gemini";
    if (m_startupOptions.managerProvider == "codex") return L"Codex";
    return L"Claude";
}

void CMainWindow::ResetMeetingState() {
    m_meetingActive = false;
    m_meetingIssue.clear();
    m_meetingRoster.clear();
    m_meetingManager.clear();
    m_meetingClaude.clear();
    m_meetingGemini.clear();
    m_meetingCodex.clear();
    m_seqQueue.clear();
    m_seqManagerResponse.clear();
    m_seqBasePrompt.clear();
    m_seqFinalManagerRequested = false;
    m_seqManagerSource = CBackendManager::OutputSource::System;
}

void CMainWindow::AppendMeetingResultCard() {
    ResetMeetingState();
    return;

    if (!m_view || !m_meetingActive) {
        ResetMeetingState();
        return;
    }

    auto excerpt = [](const std::wstring& text) {
        std::wstring out = text;
        while (!out.empty() && (out.front() == L'\r' || out.front() == L'\n' || out.front() == L' ')) out.erase(out.begin());
        while (!out.empty() && (out.back() == L'\r' || out.back() == L'\n' || out.back() == L' ')) out.pop_back();
        if (out.size() > 900) out = out.substr(0, 900) + L"...";
        return out.empty() ? L"(응답 없음)" : out;
    };

    std::wstring card;
    card += L"회의 결과\n\n";
    card += L"- 안건: " + m_meetingIssue + L"\n";
    card += L"- 참여자: " + m_meetingRoster + L"\n";
    card += L"- 정팀장: " + m_meetingManager + L"\n\n";
    if (!m_meetingClaude.empty()) card += L"Claude\n" + excerpt(m_meetingClaude) + L"\n\n";
    if (!m_meetingGemini.empty()) card += L"Gemini\n" + excerpt(m_meetingGemini) + L"\n\n";
    if (!m_meetingCodex.empty()) card += L"Codex\n" + excerpt(m_meetingCodex) + L"\n\n";
    card += L"결정\n";
    card += L"- 각 발언은 실제 백엔드 응답에서 수집한 내용입니다.\n";
    card += L"- 최종 실행 권한은 정팀장 provider가 갖고, 나머지는 검토/대안 역할입니다.";

    m_view->NewBlock(L"task");
    m_view->AppendText(card);
    SaveChatBlock(L"task", card);
    ResetMeetingState();
}

bool CMainWindow::HandleDelegateTool(const std::string& chatKey,
                                     CBackendManager::OutputSource source,
                                     const Json::Value& args) {
    DelegationRequest request;
    if (!ParseDelegationRequest(args, request)) {
        std::wstring err = BuildDelegationErrorCard(L"providers 또는 prompt가 비어 있습니다.");
        if (chatKey == m_currentChatKey && m_view) {
            m_view->NewBlock(L"error");
            m_view->AppendText(err);
            m_view->ScrollLatestIntoView();
        }
        if (chatKey == m_currentChatKey) SaveChatBlock(L"error", err);
        else SaveBlockToChatKey(chatKey, L"error", err);
        return true;
    }

    std::wstring lowerTask = request.prompt;
    std::transform(lowerTask.begin(), lowerTask.end(), lowerTask.begin(), towlower);
    std::wstring lowerLast = m_lastPrompt;
    std::transform(lowerLast.begin(), lowerLast.end(), lowerLast.begin(), towlower);
    if (lowerTask.find(L"auto_retry_rate_limit") != std::wstring::npos &&
        lowerLast.find(L"auto_retry_rate_limit") == std::wstring::npos) {
        std::wstring err = BuildDelegationErrorCard(
            L"현재 사용자 요청과 무관한 과거 작업(auto_retry_rate_limit) 위임은 차단했습니다.");
        if (chatKey == m_currentChatKey && m_view) {
            m_view->NewBlock(L"error");
            m_view->AppendText(err);
            m_view->ScrollLatestIntoView();
        }
        if (chatKey == m_currentChatKey) SaveChatBlock(L"error", err);
        else SaveBlockToChatKey(chatKey, L"error", err);
        return true;
    }

    std::wstring delegator = DelegationSourceLabel(source);
    std::wstring delegatedPrompt = BuildDelegatedPrompt(delegator, chatKey, request.prompt, request.scope);
    int sent = m_backend ? m_backend->SendToProviders(chatKey, delegatedPrompt, request.providers) : 0;
    std::wstring card = BuildDelegationCard(delegator, request.roster, sent, request.prompt);
    if (chatKey == m_currentChatKey && m_view) {
        m_view->NewBlock(sent > 0 ? L"tool" : L"error");
        m_view->AppendText(card);
        m_view->ScrollLatestIntoView();
    }
    if (chatKey == m_currentChatKey) SaveChatBlock(sent > 0 ? L"tool" : L"error", card);
    else SaveBlockToChatKey(chatKey, sent > 0 ? L"tool" : L"error", card);

    if (sent > 0) {
        m_pendingBackendCount += sent;
        if (m_view) m_view->SetBusy(true);
    }
    return true;
}

} // namespace orange
