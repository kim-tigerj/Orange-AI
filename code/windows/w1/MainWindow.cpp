#include "MainWindow.h"
#include "Capture.h"
#include "ClaudePrintBackend.h"
#include "AttachmentStore.h"
#include "AppSettings.h"
#include "Persistence.h"
#include "ManagerEnvironment.h"
#include "Delegation.h"
#include "ChatSessionOps.h"
#include "NameInputDialog.h"
#include "Utils.h"
#include "Coordination.h"
#include "resource.h"

#include <json/json.h>
#include <process.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>
#include <windowsx.h>
#include <wincodec.h>
#include <algorithm>
#include <cwctype>
#include <fstream>
#include <vector>

#pragma comment(lib, "comdlg32.lib")

namespace orange {

namespace {
constexpr UINT_PTR kTimerTestPrompt = 4101;
constexpr UINT_PTR kTimerTestCapture = 4102;
constexpr UINT_PTR kTimerTestExit = 4103;
constexpr UINT_PTR kTimerMockOutput = 4104;
constexpr UINT_PTR kTimerTestPasteClipboardImage = 4105;
constexpr UINT_PTR kTimerTestAttachment = 4106;
constexpr UINT_PTR kTimerRetryCountdown = 4107;
constexpr UINT_PTR kTimerStartupBackendSync = 4108;
constexpr UINT_PTR kTimerTestShowRetry = 4109;
constexpr UINT_PTR kTimerAutoLoop      = 4110;
constexpr UINT_PTR kTimerProviderRelayTimeout = 4111;
constexpr UINT kProviderRelayTimeoutMs = 90000;
constexpr wchar_t kWindowClass[] = L"OrangeCodeMainWindow";
constexpr wchar_t kAppTitleBase[] = L"Orange Code " APP_VER_WSTR;
constexpr UINT kSidebarMenuOpen = 5101;
constexpr UINT kSidebarMenuNewChat = 5102;
constexpr UINT kSidebarMenuRename = 5103;
constexpr UINT kMsgRunSidebarAction = WM_APP + 4;
constexpr UINT kMsgLoadOlderHistory = WM_APP + 5;
constexpr UINT kMsgAdminToolDone = WM_APP + 6;
constexpr UINT kMsgDeferredOpenChat = WM_APP + 8;
#ifndef BI_ALPHABITFIELDS
constexpr DWORD BI_ALPHABITFIELDS = 6;
#endif

UINT ChatSessionsChangedMessage() {
    static UINT msg = RegisterWindowMessageW(L"OrangeCode.ChatSessionsChanged.v1");
    return msg;
}

UINT ShutdownForUpdateMessage() {
    static UINT msg = RegisterWindowMessageW(L"OrangeCode.ShutdownForUpdate.v1");
    return msg;
}

struct AdminToolResultPacket {
    std::string chatKey;
    std::wstring provider;
    std::wstring command;
    DWORD exitCode = 0;
    DWORD elapsedMs = 0;
    bool launched = false;
};

struct AdminToolThreadPacket {
    HWND hwnd = nullptr;
    std::string chatKey;
    std::wstring provider;
    std::wstring command;
    std::wstring cwd;
};

std::wstring JsonStringWide(const Json::Value& value, const char* key) {
    if (!value.isObject() || !value[key].isString()) return {};
    std::string text = value[key].asString();
    return Utf8ToWide(text.c_str(), (int)text.size());
}

bool ParseToolRequest(const std::wstring& text, Json::Value& root) {
    std::wstring trimmed = text;
    while (!trimmed.empty() && iswspace(trimmed.front())) trimmed.erase(trimmed.begin());
    while (!trimmed.empty() && iswspace(trimmed.back())) trimmed.pop_back();
    if (trimmed.rfind(L"```json", 0) == 0) {
        size_t firstNl = trimmed.find(L'\n');
        size_t fence = trimmed.rfind(L"```");
        if (firstNl != std::wstring::npos && fence != std::wstring::npos && fence > firstNl) {
            trimmed = trimmed.substr(firstNl + 1, fence - firstNl - 1);
        }
    }
    size_t start = trimmed.find(L'{');
    size_t end = trimmed.rfind(L'}');
    if (start == std::wstring::npos || end == std::wstring::npos || end <= start) return false;
    std::string json = WideToUtf8(trimmed.substr(start, end - start + 1));
    Json::CharReaderBuilder builder;
    std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
    std::string errs;
    return reader->parse(json.data(), json.data() + json.size(), &root, &errs) &&
           root.isObject() && root["tool"].isString();
}

int SelectedBackendCount(bool useClaude, bool useGemini, bool useCodex) {
    return (useClaude ? 1 : 0) + (useGemini ? 1 : 0) + (useCodex ? 1 : 0);
}

std::wstring CouncilRoster(bool useClaude, bool useGemini, bool useCodex) {
    std::wstring roster;
    if (useClaude) roster += L"Claude";
    if (useGemini) roster += (roster.empty() ? L"" : L", ") + std::wstring(L"Gemini");
    if (useCodex) roster += (roster.empty() ? L"" : L", ") + std::wstring(L"Codex");
    return roster.empty() ? L"none" : roster;
}

std::wstring BuildCouncilRoutingPrompt(const std::wstring& prompt,
                                       const std::wstring& roster,
                                       const std::wstring& coordinator) {
    std::wstring routed;
    // 비정팀장 역할 명확화 — 프롬프트 최상단에 배치하여 시스템 컨텍스트보다 우선 인식
    routed += L"━━━ 역할 명확화 (ROLE CLARIFICATION) ━━━\n";
    routed += L"이 회의의 정팀장 provider(실행 책임자): " + coordinator + L"\n";
    routed += L"사용자 명령이 수정/구현/빌드/검증을 요구하면 정팀장 provider는 토론을 끝내고 실행 모드로 전환합니다.\n";
    routed += L"당신이 " + coordinator + L"이 아닌 provider라면:\n";
    routed += L"  - 당신은 검토자/조언자입니다. 정팀장이 아닙니다.\n";
    routed += L"  - ORANGE_CODE_ROLE=manager 환경변수는 앱 역할명이며, 이 회의에서 당신의 provider 역할을 정의하지 않습니다.\n";
    routed += L"  - orange.build / orange.capture / orange.exec_admin / orange.delegate 도구를 직접 요청하지 마세요.\n";
    routed += L"  - 파일 수정, 빌드, 캡처를 했다고 말하지 마세요. 의견·근거·위험·제안만 작성합니다.\n";
    routed += L"━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n\n";
    routed += L"OrangeCode multi-provider coordination routing.\n";
    routed += L"선택된 참여자: " + roster + L"\n";
    routed += L"정팀장 provider: " + coordinator + L"\n\n";
    routed += L"규칙:\n";
    routed += L"1. 사용자가 의견/상태/논의를 요청한 경우에만 회의 발언으로 답합니다.\n";
    routed += L"2. 사용자가 고쳐라/수정해/구현해/추가해/빌드해/검증해처럼 실행을 지시하면 정팀장 provider가 즉시 파일 확인과 최소 패치로 진입합니다.\n";
    routed += L"3. 정팀장 provider는 실행 전에 관련 파일과 현재 채팅 근거만 짧게 확인하고, 불필요한 추가 회의를 열지 않습니다.\n";
    routed += L"4. 다른 provider 발언을 지어내지 않습니다. 자기 발언만 작성합니다.\n";
    routed += L"5. 비정팀장 provider는 실행을 주장하지 않고, 요청받은 경우에만 짧은 리뷰/위험 지적을 남깁니다.\n\n";
    routed += L"6. 회담을 통해 도출된 결론은 정팀장 provider가 내부 지침으로 기록해 이후 세션과 다음 실행 판단에 반영합니다.\n";
    routed += L"7. 의견이 모였거나 반복/정체되면 정팀장 provider가 결론을 선언하고 즉시 다음 구체 행동으로 넘어갑니다. 한 턴씩 의견만 내고 종료하지 않습니다.\n";
    routed += L"8. 각 단계를 진행하며 반복 적용 가능한 합리적 운영 방식이 나오면 룰 후보로 판단하고, 관련 durable file에 짧게 기록해 즉시 적용합니다.\n\n";
    routed += L"9. Provider relay는 일반 이름 언급이 아니라 명령어로만 작동합니다: `/relay Codex: ...`, `/ask Gemini: ...`, `/전달 Claude: ...`, `/호출 Codex: ...`, `@Claude ...`. 대상자가 일정 시간 응답하지 않으면 질문한 provider가 대신 처리합니다.\n\n";
    routed += L"자가수정 기준:\n";
    routed += L"- 코드 변경 전 관련 파일과 현재 채팅 근거를 먼저 확인합니다.\n";
    routed += L"- 실행자는 한 명만 둡니다. 나머지는 검토/위험 지적 역할입니다.\n";
    routed += L"- 명시적 실행 명령이 있으면 의견 수렴보다 패치와 검증을 우선합니다.\n";
    routed += L"- 회담 결론을 지침으로 남길 때 정팀장/팀원이 다음 세션에서 읽는 파일은 `MANAGER_HANDOFF.md` 또는 `TOKEN_BUDGET_PROTOCOL.md`입니다.\n";
    routed += L"- `OH_COUNCIL.md`는 대표님과 오감독 사이의 상위 협업 규칙 파일이므로, 정팀장/팀원은 기본 작업 흐름에서 읽지 않습니다.\n";
    routed += L"- 새 운영 방식은 한 번의 사례가 아니라 이후에도 반복 적용할 가치가 있을 때만 룰화하고, 현재 작업 범위를 벗어난 대형 정리는 만들지 않습니다.\n";
    routed += L"- 정팀장 provider가 직접 파일을 수정할 수 없으면 수정했다고 말하지 말고 가장 작은 패치 계획을 제시합니다.\n";
    routed += L"- 변경 후에는 정팀장 provider가 standalone JSON {\"tool\":\"orange.build\"}를 요청해 빌드와 앱 갱신을 맡깁니다.\n";
    routed += L"- UI 문제는 빌드 후 standalone JSON {\"tool\":\"orange.capture\"}로 화면 검증을 요청합니다.\n\n";
    routed += L"출력 형식:\n";
    routed += L"- 회의/리뷰 모드: 의견, 근거, 위험, 제안/위임을 짧게 작성합니다.\n";
    routed += L"- 실행 모드: 변경 대상, 실제 변경, 검증 요청만 짧게 보고합니다.\n\n";
    routed += L"사용자 명령:\n";
    routed += prompt;
    return routed;
}

std::wstring CurrentSessionKey() {
    wchar_t buf[256] = L"";
    if (GetEnvironmentVariableW(L"ORANGE_CODE_SESSION_KEY", buf, 256) > 0 && buf[0] != L'\0') {
        return buf;
    }
    return L"default";
}

std::wstring TrimWide(std::wstring v) {
    while (!v.empty() && iswspace(v.front())) v.erase(v.begin());
    while (!v.empty() && iswspace(v.back())) v.pop_back();
    return v;
}

bool ConsumePrefix(const std::wstring& text, const std::wstring& prefix, std::wstring& rest) {
    if (text.rfind(prefix, 0) != 0) return false;
    rest = TrimWide(text.substr(prefix.size()));
    return !rest.empty();
}

bool IsSupervisorCall(const std::wstring& text) {
    std::wstring trimmed = TrimWide(text);
    return trimmed == L"/supervisor" ||
           trimmed.rfind(L"/supervisor ", 0) == 0 ||
           trimmed.rfind(L"/supervisor:", 0) == 0;
}

std::string SlugFromTitle(const std::wstring& title) {
    std::string utf8 = WideToUtf8(title);
    std::string out;
    out.reserve(utf8.size());
    bool lastDash = false;
    for (unsigned char ch : utf8) {
        bool safe =
            (ch >= 'a' && ch <= 'z') ||
            (ch >= 'A' && ch <= 'Z') ||
            (ch >= '0' && ch <= '9');
        if (safe) {
            out.push_back((char)tolower(ch));
            lastDash = false;
        } else if (!lastDash) {
            out.push_back('-');
            lastDash = true;
        }
    }
    while (!out.empty() && out.front() == '-') out.erase(out.begin());
    while (!out.empty() && out.back() == '-') out.pop_back();
    if (out.empty()) {
        SYSTEMTIME st{};
        GetLocalTime(&st);
        char buf[40];
        sprintf_s(buf, "item-%04u%02u%02u-%02u%02u%02u",
                  st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
        out = buf;
    }
    return CGoal::SanitizeId(out);
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

}

CMainWindow::CMainWindow() : m_hwnd(nullptr), m_hInst(nullptr) {}

CMainWindow::~CMainWindow() {
    if (m_hUiFont) {
        DeleteObject(m_hUiFont);
        m_hUiFont = nullptr;
    }
}

void CMainWindow::SetStartupOptions(StartupOptions options) {
    m_startupOptions = std::move(options);
}

bool CMainWindow::Create(HINSTANCE hInst, int nShow) {
    m_hInst = hInst;
    HICON icon = LoadIconW(hInst, MAKEINTRESOURCEW(IDI_APPICON));
    HICON smallIcon = (HICON)LoadImageW(hInst, MAKEINTRESOURCEW(IDI_APPICON),
                                        IMAGE_ICON, 16, 16, LR_DEFAULTCOLOR);
    WNDCLASSEXW wc{sizeof(wc), CS_HREDRAW|CS_VREDRAW, WindowProc, 0, 0, hInst, icon,
                   LoadCursor(0, IDC_ARROW), (HBRUSH)(COLOR_WINDOW+1), 0, kWindowClass, smallIcon};
    RegisterClassExW(&wc);

    m_hwnd = CreateWindowExW(0, kWindowClass, kAppTitleBase, WS_OVERLAPPEDWINDOW,
                             CW_USEDEFAULT, 0, 1024, 768, 0, 0, hInst, this);
    if (!m_hwnd) return false;
    if (icon) SendMessageW(m_hwnd, WM_SETICON, ICON_BIG, (LPARAM)icon);
    if (smallIcon) SendMessageW(m_hwnd, WM_SETICON, ICON_SMALL, (LPARAM)smallIcon);

    {
        orange::WindowState ws = orange::Persistence::LoadWindowState();
        if (ws.valid) {
            WINDOWPLACEMENT wp{};
            wp.length   = sizeof(wp);
            wp.showCmd  = (UINT)ws.showCmd;
            wp.rcNormalPosition = { ws.x, ws.y, ws.x + ws.width, ws.y + ws.height };
            SetWindowPlacement(m_hwnd, &wp);
        } else {
            ShowWindow(m_hwnd, nShow);
        }
    }
    return true;
}

void CMainWindow::Run() {
    MSG msg;
    while (GetMessageW(&msg, 0, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

LRESULT CALLBACK CMainWindow::WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    if (uMsg == WM_CREATE) {
        CREATESTRUCT* pCreate = reinterpret_cast<CREATESTRUCT*>(lParam);
        CMainWindow* pThis = static_cast<CMainWindow*>(pCreate->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(pThis));
        pThis->m_hwnd = hwnd;
    }
    CMainWindow* pThis = reinterpret_cast<CMainWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (pThis) return pThis->HandleMessage(hwnd, uMsg, wParam, lParam);
    return DefWindowProcW(hwnd, uMsg, wParam, lParam);
}

LRESULT CMainWindow::HandleMessage(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    if (uMsg == ChatSessionsChangedMessage()) {
        if (!m_currentChatKey.empty()) {
            ChatSession fresh = Persistence::LoadRecent(m_currentChatKey, 1);
            if (!fresh.title.empty()) m_currentChatSession.title = fresh.title;
            m_currentChatSession.summary = fresh.summary;
            m_currentChatSession.progress = fresh.progress;
            m_currentChatSession.lastActiveIso = fresh.lastActiveIso;
            UpdateWindowTitle();
        }
        RefillSidebar();
        return 0;
    }
    if (uMsg == ShutdownForUpdateMessage()) {
        PostMessageW(hwnd, WM_CLOSE, 0, 0);
        return 0;
    }

    switch (uMsg) {
    case WM_CREATE: {
        m_input = std::make_unique<COrangeInput>();
        m_input->Create(hwnd, m_hInst);
        m_input->SetSubmitCallback([this](const std::wstring&){ SubmitFromInput(); });
        m_input->SetTextChangeCallback([this]() { LayoutControls(); });
        m_input->SetPasteImageCallback([this]() { return PasteClipboardImage(); });
        m_input->SetAttachFileCallback([this]() { SelectAttachmentFiles(); });
        m_input->SetCaptureCallback([this]() { RunCaptureCommand(); });
        m_input->SetLoopToggleCallback([this]() { ToggleAutoLoop(L""); });
        m_input->SetFilesDroppedCallback([this](const std::vector<std::wstring>& paths) {
            AddAttachmentPaths(paths);
        });
        m_input->SetProviderState(m_startupOptions.useClaude, m_startupOptions.useGemini, m_startupOptions.useCodex);
        m_input->SetManagerProvider(m_startupOptions.managerProvider);
        {
            std::wstring provider = Utf8ToWide(m_startupOptions.managerProvider.c_str(), (int)m_startupOptions.managerProvider.size());
            SetEnvironmentVariableW(L"ORANGE_CODE_MANAGER_PROVIDER", provider.c_str());
        }
        m_input->SetProviderToggleCallback([this](bool useClaude, bool useGemini, bool useCodex) {
            bool managerEnabled =
                (m_startupOptions.managerProvider == "claude" && useClaude) ||
                (m_startupOptions.managerProvider == "gemini" && useGemini) ||
                (m_startupOptions.managerProvider == "codex" && useCodex);
            if (!managerEnabled) {
                if (useClaude) m_startupOptions.managerProvider = "claude";
                else if (useGemini) m_startupOptions.managerProvider = "gemini";
                else if (useCodex) m_startupOptions.managerProvider = "codex";
                if (m_input) m_input->SetManagerProvider(m_startupOptions.managerProvider);
            }
            std::wstring provider = Utf8ToWide(m_startupOptions.managerProvider.c_str(), (int)m_startupOptions.managerProvider.size());
            SetEnvironmentVariableW(L"ORANGE_CODE_MANAGER_PROVIDER", provider.c_str());
            SendMessageW(m_hChkClaude, BM_SETCHECK, useClaude ? BST_CHECKED : BST_UNCHECKED, 0);
            SendMessageW(m_hChkGemini, BM_SETCHECK, useGemini ? BST_CHECKED : BST_UNCHECKED, 0);
            SendMessageW(m_hChkCodex, BM_SETCHECK, useCodex ? BST_CHECKED : BST_UNCHECKED, 0);
            if (m_backend) {
                m_backend->SetUseClaude(useClaude);
                m_backend->SetUseGemini(useGemini);
                m_backend->SetUseCodex(useCodex);
            }
            AppSettings::SaveBackendSettings(useClaude, useGemini, useCodex, m_startupOptions.managerProvider);
        });
        m_input->SetManagerProviderCallback([this](const std::string& provider) {
            m_startupOptions.managerProvider = provider;
            std::wstring providerW = Utf8ToWide(provider.c_str(), (int)provider.size());
            SetEnvironmentVariableW(L"ORANGE_CODE_MANAGER_PROVIDER", providerW.c_str());
            bool useClaude = SendMessageW(m_hChkClaude, BM_GETCHECK, 0, 0) == BST_CHECKED;
            bool useGemini = SendMessageW(m_hChkGemini, BM_GETCHECK, 0, 0) == BST_CHECKED;
            bool useCodex = SendMessageW(m_hChkCodex, BM_GETCHECK, 0, 0) == BST_CHECKED;
            AppSettings::SaveBackendSettings(useClaude, useGemini, useCodex, provider);
        });
        DragAcceptFiles(hwnd, TRUE);

        m_hChkClaude = CreateWindowExW(0, L"BUTTON", L"Claude",
            WS_CHILD|WS_VISIBLE|BS_AUTOCHECKBOX|BS_PUSHLIKE|BS_FLAT,
            0, 0, 0, 0, hwnd, (HMENU)1010, m_hInst, 0);
        m_hChkGemini = CreateWindowExW(0, L"BUTTON", L"Gemini",
            WS_CHILD|WS_VISIBLE|BS_AUTOCHECKBOX|BS_PUSHLIKE|BS_FLAT,
            0, 0, 0, 0, hwnd, (HMENU)1011, m_hInst, 0);
        m_hChkCodex = CreateWindowExW(0, L"BUTTON", L"Codex",
            WS_CHILD|WS_VISIBLE|BS_AUTOCHECKBOX|BS_PUSHLIKE|BS_FLAT,
            0, 0, 0, 0, hwnd, (HMENU)1012, m_hInst, 0);
        m_hBtnBold = CreateWindowExW(0, L"BUTTON", L"B", WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON|BS_FLAT, 0,0,0,0, hwnd, (HMENU)1020, m_hInst, 0);
        m_hBtnItalic = CreateWindowExW(0, L"BUTTON", L"I", WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON|BS_FLAT, 0,0,0,0, hwnd, (HMENU)1021, m_hInst, 0);
        m_hBtnCode = CreateWindowExW(0, L"BUTTON", L"Code", WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON|BS_FLAT, 0,0,0,0, hwnd, (HMENU)1022, m_hInst, 0);
        m_hBtnTable = CreateWindowExW(0, L"BUTTON", L"Table", WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON|BS_FLAT, 0,0,0,0, hwnd, (HMENU)1023, m_hInst, 0);
        m_hBtnClear = CreateWindowExW(0, L"BUTTON", L"Clear", WS_CHILD|WS_VISIBLE|BS_PUSHBUTTON|BS_FLAT, 0,0,0,0, hwnd, (HMENU)1024, m_hInst, 0);

        m_hUiFont = CreateFontW(-14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
        HWND controls[] = {m_hChkClaude, m_hChkGemini, m_hChkCodex, m_hBtnBold, m_hBtnItalic, m_hBtnCode, m_hBtnTable, m_hBtnClear};
        for (HWND control : controls) SendMessageW(control, WM_SETFONT, (WPARAM)m_hUiFont, TRUE);

        {
            auto userSettings = AppSettings::LoadUserSettings();
            std::wstring labelText = userSettings.userName.empty() ? L"사용자" : userSettings.userName;
            m_hUserLabel = CreateWindowExW(0, L"STATIC", labelText.c_str(),
                WS_CHILD | WS_VISIBLE | SS_RIGHT,
                0, 0, 0, 0, hwnd, nullptr, m_hInst, nullptr);
            SendMessageW(m_hUserLabel, WM_SETFONT, (WPARAM)m_hUiFont, TRUE);
        }

        ShowWindow(m_hBtnBold, SW_HIDE);
        ShowWindow(m_hBtnItalic, SW_HIDE);
        ShowWindow(m_hBtnCode, SW_HIDE);
        ShowWindow(m_hBtnTable, SW_HIDE);
        ShowWindow(m_hBtnClear, SW_HIDE);
        ShowWindow(m_hChkClaude, SW_HIDE);
        ShowWindow(m_hChkGemini, SW_HIDE);
        ShowWindow(m_hChkCodex, SW_HIDE);
        SendMessageW(m_hChkClaude, BM_SETCHECK, m_startupOptions.useClaude ? BST_CHECKED : BST_UNCHECKED, 0);
        SendMessageW(m_hChkGemini, BM_SETCHECK, m_startupOptions.useGemini ? BST_CHECKED : BST_UNCHECKED, 0);
        SendMessageW(m_hChkCodex, BM_SETCHECK, m_startupOptions.useCodex ? BST_CHECKED : BST_UNCHECKED, 0);

        m_view = std::make_unique<OrangeView>();
        m_view->Create(hwnd, 0, 0, 0, 0, (HMENU)1003, m_hInst);
        {
            auto us = AppSettings::LoadUserSettings();
            m_view->SetUserName(us.userName.empty() ? L"나" : us.userName);
        }
        m_view->SetCancelCallback([this]() {
            if (m_backend) m_backend->Cancel(m_currentChatKey);
        });
        m_view->SetNearTopCallback([this]() {
            QueueLoadOlderHistory();
        });

        m_sidebar = std::make_unique<CSidebar>();
        m_sidebar->Create(hwnd, m_hInst, 1004);

        m_backend = std::make_unique<CBackendManager>();
        m_backend->Start([this](const std::string& chatKey, CBackendManager::OutputSource source, std::wstring c) {
            struct OutputPacket { std::string chatKey; CBackendManager::OutputSource source; std::wstring text; };
            auto* packet = new OutputPacket{chatKey, source, std::move(c)};
            PostMessageW(m_hwnd, WM_APP + 1, 0, (LPARAM)packet);
        }, [this](const std::string& chatKey) {
            auto* key = new std::string(chatKey);
            PostMessageW(m_hwnd, WM_APP + 2, 0, (LPARAM)key);
        }, [this](const std::string& chatKey, std::wstring errMsg) {
            struct RetryPacket { std::string chatKey; std::wstring errMsg; };
            auto* packet = new RetryPacket{chatKey, std::move(errMsg)};
            PostMessageW(m_hwnd, WM_APP + 7, 0, (LPARAM)packet);
        });
        m_backend->SetUseClaude(m_startupOptions.useClaude);
        m_backend->SetUseGemini(m_startupOptions.useGemini);
        m_backend->SetUseCodex(m_startupOptions.useCodex);

        // SQLite DB 초기화 — WAL 모드, 다중 프로세스 안전
        {
            wchar_t appData[MAX_PATH] = L"";
            if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, appData))) {
                std::wstring dbPath = std::wstring(appData) + L"\\OrangeCode\\orange_code.db";
                try { CCoordination::Initialize(dbPath); } catch (...) {}
            }
        }

        RefillSidebar();
        LayoutControls();
        // 초기 채팅 로딩은 창이 표시된 후에 처리 (WM_CREATE 동기 블로킹 방지)
        if (GetEnvironmentVariableW(L"ORANGE_CODE_SESSION_KEY", nullptr, 0) == 0) {
            m_deferredOpenChatKey = Persistence::LatestGlobalChatKey();
        } else {
            m_deferredOpenChatKey = WideToUtf8(CurrentSessionKey());
        }
        if (!m_deferredOpenChatKey.empty()) {
            PostMessageW(hwnd, kMsgDeferredOpenChat, 0, 0);
        } else {
            UpdateWindowTitle();
        }
        LayoutControls();
        if (!m_startupOptions.testPrompt.empty()) {
            SetTimer(hwnd, kTimerTestPrompt, 300, nullptr);
        } else if (!m_startupOptions.useMockBackend && !m_deferredOpenChatKey.empty()) {
            SetTimer(hwnd, kTimerStartupBackendSync, 900, nullptr);
        }
        if (m_startupOptions.testPasteClipboardImage) {
            SetTimer(hwnd, kTimerTestPasteClipboardImage, 600, nullptr);
        }
        if (!m_startupOptions.testAttachmentPath.empty()) {
            SetTimer(hwnd, kTimerTestAttachment, 600, nullptr);
        }
        if (!m_startupOptions.testCapturePath.empty()) {
            SetTimer(hwnd, kTimerTestCapture, m_startupOptions.testCaptureDelayMs, nullptr);
        }
        if (m_startupOptions.testShowRetry) {
            SetTimer(hwnd, kTimerTestShowRetry, 600, nullptr);
        }
        if (m_startupOptions.testExitMs > 0 &&
            (!m_startupOptions.testPrompt.empty() ||
             m_startupOptions.testPasteClipboardImage ||
             !m_startupOptions.testAttachmentPath.empty() ||
             !m_startupOptions.testCapturePath.empty() ||
             m_startupOptions.testShowRetry)) {
            SetTimer(hwnd, kTimerTestExit, m_startupOptions.testExitMs, nullptr);
        }
        return 0;
    }
    case WM_SIZE:
        LayoutControls();
        return 0;
    case WM_TIMER:
        if (wParam == kTimerTestPrompt) {
            KillTimer(hwnd, kTimerTestPrompt);
            DispatchPrompt(m_startupOptions.testPrompt);
            return 0;
        }
        if (wParam == kTimerStartupBackendSync) {
            KillTimer(hwnd, kTimerStartupBackendSync);
            RunAutoResumePrompt();
            return 0;
        }
        if (wParam == kTimerTestCapture) {
            KillTimer(hwnd, kTimerTestCapture);
            RunTestCapture();
            return 0;
        }
        if (wParam == kTimerTestExit) {
            KillTimer(hwnd, kTimerTestExit);
            PostMessageW(hwnd, WM_CLOSE, 0, 0);
            return 0;
        }
        if (wParam == kTimerMockOutput) {
            KillTimer(hwnd, kTimerMockOutput);
            std::wstring response = m_startupOptions.testResponse;
            if (response.empty()) {
                response = L"Mock response ready.\r\n\r\n- Build and capture can run without Gemini or Claude quota.\r\n- Use real backends only for backend integration checks.";
            }
            AppendBackendOutput(m_currentChatKey, CBackendManager::OutputSource::Mock, response);
            HandleBackendDone(m_currentChatKey);
            return 0;
        }
        if (wParam == kTimerTestPasteClipboardImage) {
            KillTimer(hwnd, kTimerTestPasteClipboardImage);
            PasteClipboardImage();
            return 0;
        }
        if (wParam == kTimerTestAttachment) {
            KillTimer(hwnd, kTimerTestAttachment);
            EnsureCurrentGlobalChat();
            AttachmentRecord rec;
            std::wstring sessionKey = Utf8ToWide(m_currentChatKey.c_str(), (int)m_currentChatKey.size());
            if (AttachmentStore::AddFile(sessionKey, m_startupOptions.testAttachmentPath, &rec)) {
                Persistence::SaveAttachmentRecord(m_currentChatKey, rec);
                AppendAttachmentCard({rec});
            }
            return 0;
        }
        if (wParam == kTimerTestShowRetry) {
            KillTimer(hwnd, kTimerTestShowRetry);
            // Inject fake rate-limit to show retry card for UI smoke capture
            struct RetryPacket { std::string chatKey; std::wstring errMsg; };
            auto* pkt = new RetryPacket{ m_currentChatKey, L"Server is temporarily limiting requests (smoke test)" };
            PostMessageW(hwnd, WM_APP + 7, 0, reinterpret_cast<LPARAM>(pkt));
            return 0;
        }
        if (wParam == kTimerRetryCountdown) {
            if (m_retryRemaining > 0) m_retryRemaining--;
            if (m_view && m_retryBlockIdx != (size_t)-1 &&
                m_retryBlockIdx < m_view->BlockCount()) {
                wchar_t buf[256];
                if (m_retryRemaining > 0) {
                    swprintf_s(buf, L"Rate limit\n\n%d초 후 재시도 (%d차). ESC로 취소.",
                               m_retryRemaining, m_retryAttempt);
                } else {
                    swprintf_s(buf, L"재시도 중 (%d차)...", m_retryAttempt);
                }
                m_view->SetBlockText(m_retryBlockIdx, buf);
            }
            if (m_retryRemaining <= 0) {
                KillTimer(hwnd, kTimerRetryCountdown);
                m_retryBlockIdx = (size_t)-1;
                if (!m_retryPrompt.empty()) {
                    std::wstring p = m_retryPrompt;
                    m_retryPrompt.clear();
                    DispatchPrompt(p);
                }
            }
            return 0;
        }
        if (wParam == kTimerAutoLoop) {
            TickAutoLoop();
            return 0;
        }
        if (wParam == kTimerProviderRelayTimeout) {
            HandleProviderRelayTimeout();
            return 0;
        }
        break;
    case WM_COMMAND: {
        const int id = LOWORD(wParam);
        if (id == 1010 && m_backend) {
            bool useClaude = SendMessageW(m_hChkClaude, BM_GETCHECK, 0, 0) == BST_CHECKED;
            bool useGemini = SendMessageW(m_hChkGemini, BM_GETCHECK, 0, 0) == BST_CHECKED;
            bool useCodex = SendMessageW(m_hChkCodex, BM_GETCHECK, 0, 0) == BST_CHECKED;
            m_backend->SetUseClaude(useClaude);
            if (m_input) m_input->SetProviderState(useClaude, useGemini, useCodex);
            AppSettings::SaveBackendSettings(useClaude, useGemini, useCodex, m_startupOptions.managerProvider);
            return 0;
        }
        if (id == 1011 && m_backend) {
            bool useClaude = SendMessageW(m_hChkClaude, BM_GETCHECK, 0, 0) == BST_CHECKED;
            bool useGemini = SendMessageW(m_hChkGemini, BM_GETCHECK, 0, 0) == BST_CHECKED;
            bool useCodex = SendMessageW(m_hChkCodex, BM_GETCHECK, 0, 0) == BST_CHECKED;
            m_backend->SetUseGemini(useGemini);
            if (m_input) m_input->SetProviderState(useClaude, useGemini, useCodex);
            AppSettings::SaveBackendSettings(useClaude, useGemini, useCodex, m_startupOptions.managerProvider);
            return 0;
        }
        if (id == 1012 && m_backend) {
            bool useClaude = SendMessageW(m_hChkClaude, BM_GETCHECK, 0, 0) == BST_CHECKED;
            bool useGemini = SendMessageW(m_hChkGemini, BM_GETCHECK, 0, 0) == BST_CHECKED;
            bool useCodex = SendMessageW(m_hChkCodex, BM_GETCHECK, 0, 0) == BST_CHECKED;
            m_backend->SetUseCodex(useCodex);
            if (m_input) m_input->SetProviderState(useClaude, useGemini, useCodex);
            AppSettings::SaveBackendSettings(useClaude, useGemini, useCodex, m_startupOptions.managerProvider);
            return 0;
        }
        if (id == 1024) {
            ClearCurrentChat();
            return 0;
        }
        if (id == 1004 && m_sidebar && HIWORD(wParam) == LBN_SELCHANGE) {
            const CSidebar::Row* row = m_sidebar->SelectedRow();
            if (!row) return 0;
            if (row->kind == CSidebar::RowKind::Chat ||
                row->kind == CSidebar::RowKind::NewChat) {
                QueueSidebarAction(*row);
            }
            return 0;
        }
        if (id == 1004 && m_sidebar && HIWORD(wParam) == LBN_DBLCLK) {
            const CSidebar::Row* row = m_sidebar->SelectedRow();
            if (!row) return 0;
            if (row->kind == CSidebar::RowKind::Goal ||
                row->kind == CSidebar::RowKind::Project) {
                RenameSidebarMeta(*row);
                return 0;
            }
            QueueSidebarAction(*row);
            return 0;
        }
        break;
    }
    case WM_CONTEXTMENU:
        if (m_sidebar && reinterpret_cast<HWND>(wParam) == m_sidebar->Hwnd()) {
            int x = GET_X_LPARAM(lParam);
            int y = GET_Y_LPARAM(lParam);
            ShowSidebarContextMenu(x, y);
            return 0;
        }
        break;
    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE && m_retryRemaining > 0) {
            KillTimer(hwnd, kTimerRetryCountdown);
            m_retryRemaining = 0;
            if (m_view && m_retryBlockIdx != (size_t)-1 &&
                m_retryBlockIdx < m_view->BlockCount()) {
                m_view->SetBlockText(m_retryBlockIdx, L"재시도 취소됨.");
            }
            m_retryBlockIdx = (size_t)-1;
            m_retryPrompt.clear();
            return 0;
        }
        if (wParam == L'V' && (GetKeyState(VK_CONTROL) & 0x8000)) {
            if (PasteClipboardImage()) return 0;
        }
        break;
    case WM_PASTE:
        if (PasteClipboardImage()) return 0;
        break;
    case WM_CTLCOLORSTATIC: {
        HDC hdc = (HDC)wParam;
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, GetSysColor(COLOR_WINDOWTEXT));
        return (LRESULT)GetSysColorBrush(COLOR_WINDOW);
    }
    case WM_DROPFILES:
        HandleDroppedFiles(reinterpret_cast<HDROP>(wParam));
        return 0;
    case WM_APP + 1: {
        struct OutputPacket { std::string chatKey; CBackendManager::OutputSource source; std::wstring text; };
        OutputPacket* packet = reinterpret_cast<OutputPacket*>(lParam);
        if (packet) {
            AppendBackendOutput(packet->chatKey, packet->source, packet->text);
            delete packet;
        }
        return 0;
    }
    case WM_APP + 2: {
        std::unique_ptr<std::string> key(reinterpret_cast<std::string*>(lParam));
        HandleBackendDone(key ? *key : std::string());
        return 0;
    }
    case WM_APP + 3: {
        std::unique_ptr<BuildResultPacket> packet(reinterpret_cast<BuildResultPacket*>(lParam));
        if (packet) AppendBuildResultCard(packet->exitCode, packet->elapsedMs, packet->output);
        return 0;
    }
    case kMsgAdminToolDone: {
        std::unique_ptr<AdminToolResultPacket> packet(reinterpret_cast<AdminToolResultPacket*>(lParam));
        if (packet && m_view) {
            std::wstring card;
            card += L"관리자 도구 결과\n\n";
            card += L"- 도구: orange.exec_admin\n";
            card += L"- 요청자: " + packet->provider + L"\n";
            card += L"- 실행: " + std::wstring(packet->launched ? L"성공" : L"실패") + L"\n";
            card += L"- 종료 코드: " + std::to_wstring(packet->exitCode) + L"\n";
            card += L"- 시간: " + std::to_wstring(packet->elapsedMs / 1000) + L"." +
                    std::to_wstring((packet->elapsedMs % 1000) / 100) + L"s\n";
            card += L"- 명령: `" + packet->command + L"`";
            if (packet->chatKey == m_currentChatKey) {
                m_view->NewBlock(packet->launched && packet->exitCode == 0 ? L"tool" : L"error");
                m_view->AppendText(card);
                m_view->ScrollLatestIntoView();
            }
            ChatSession session = Persistence::Load(packet->chatKey);
            ChatBlock block;
            block.role = packet->launched && packet->exitCode == 0 ? L"tool" : L"error";
            block.text = card;
            session.blocks.push_back(std::move(block));
            Persistence::Save(session, packet->chatKey);
            RefillSidebar();
        }
        return 0;
    }
    case WM_APP + 7: {
        // rate limit 재시도 요청 — 카운트다운 블록 생성 후 타이머 시작
        struct RetryPacket { std::string chatKey; std::wstring errMsg; };
        std::unique_ptr<RetryPacket> packet(reinterpret_cast<RetryPacket*>(lParam));
        if (packet && packet->chatKey == m_currentChatKey && m_view) {
            KillTimer(hwnd, kTimerRetryCountdown);
            m_retryPrompt = m_lastPrompt;
            static const int kRetryDelays[5] = {60, 120, 300, 600, 600};
            int idx = m_retryAttempt > 4 ? 4 : m_retryAttempt;
            int delay = kRetryDelays[idx];
            m_retryRemaining = delay;
            m_retryAttempt++;

            wchar_t buf[256];
            swprintf_s(buf, L"Rate limit\n\n%d초 후 재시도 (%d차). ESC로 취소.",
                       delay, m_retryAttempt);
            m_view->NewBlock(L"retry");
            m_view->AppendText(buf);
            m_retryBlockIdx = m_view->BlockCount() - 1;
            m_view->ScrollLatestIntoView();
            SaveChatBlock(L"retry", buf);
            SetTimer(hwnd, kTimerRetryCountdown, 1000, nullptr);
        }
        return 0;
    }
    case kMsgDeferredOpenChat:
        if (!m_deferredOpenChatKey.empty()) {
            OpenChatSession(m_deferredOpenChatKey);
            m_deferredOpenChatKey.clear();
        }
        return 0;
    case kMsgRunSidebarAction:
        RunSidebarAction();
        return 0;
    case kMsgLoadOlderHistory:
        LoadOlderHistory();
        return 0;
    case WM_DESTROY:
        orange::Persistence::SaveWindowState(hwnd);
        DragAcceptFiles(hwnd, FALSE);
        if (m_backend) m_backend->CancelAll();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, uMsg, wParam, lParam);
}

void CMainWindow::LayoutControls() {
    RECT rc; GetClientRect(m_hwnd, &rc);
    int w = rc.right - rc.left;
    int h = rc.bottom - rc.top;
    constexpr int margin = 8;
    constexpr int headerH = 28;  // top strip for user title label
    int sidebarW = 268;
    if (m_sidebar) m_sidebar->Layout(0, 0, sidebarW, h);
    int leftEdge = sidebarW + margin;
    int contentW = w - leftEdge - margin;
    if (contentW < 240 || h < 180) return;

    if (m_hUserLabel)
        MoveWindow(m_hUserLabel, leftEdge, 4, contentW, headerH - 4, TRUE);

    int inputH = 52;
    if (m_input) {
        inputH = (int)m_input->GetIdealHeight((float)contentW);
        if (inputH < 40) inputH = 40;
        if (inputH > 184) inputH = 184;
    }

    int inputY = h - margin - inputH;
    int viewH = inputY - headerH - margin;
    if (viewH < 40) viewH = 40;

    if (m_view) MoveWindow(m_view->Hwnd(), leftEdge, headerH, contentW, viewH, TRUE);
    if (m_input) m_input->Layout(leftEdge, inputY, contentW, inputH);
}

void CMainWindow::RefillSidebar() {
    std::vector<CSidebar::Row> rows;
    int selectedIndex = -1;
    auto addHeader = [&](const std::wstring& title) {
        CSidebar::Row header;
        header.kind = CSidebar::RowKind::Empty;
        header.title = title;
        rows.push_back(std::move(header));
    };
    std::string currentKey = m_currentChatKey;
    if (currentKey.empty()) currentKey = WideToUtf8(CurrentSessionKey());
    auto chatTitle = [](const Persistence::SessionMeta& s, size_t maxLen) {
        std::wstring title;
        if (!s.title.empty()) {
            title = Utf8ToWide(s.title.c_str(), (int)s.title.size());
        } else if (!s.previewLabel.empty()) {
            title = s.previewLabel;
            size_t sep = title.rfind(L" · ");
            if (sep != std::wstring::npos) title.resize(sep);
        } else {
            title = Utf8ToWide(s.key.c_str(), (int)s.key.size());
        }
        if (title.empty()) title = L"기본 대화";
        if (!s.providerLabel.empty()) {
            title = L"[" + s.providerLabel + L"] " + title;
        }
        if (title.size() > maxLen) title = title.substr(0, maxLen - 3) + L"…";
        return title;
    };

    addHeader(L"정팀장");
    {
        CSidebar::Row r;
        r.kind = CSidebar::RowKind::NewChat;
        r.level = 0;
        r.title = L"새 대화";
        rows.push_back(std::move(r));
    }
    auto globalChats = Persistence::ListSessions();
    for (const auto& s : globalChats) {
        CSidebar::Row r;
        r.kind = CSidebar::RowKind::Chat;
        r.level = 0;
        r.title = chatTitle(s, 30);
        r.progress = s.progress;
        r.trailing = Persistence::FormatMtimeShort(s.mtime);
        r.isCurrent = (s.key == currentKey);
        r.lastActiveIso = s.lastActiveIso;
        r.chatKey = s.key;
        if (r.isCurrent) selectedIndex = (int)rows.size();
        rows.push_back(std::move(r));
    }

    auto goals = Persistence::ListAllGoals();
    bool anyConfiguredGoal = false;
    for (const auto& g : goals) {
        const bool isPlaceholderGoal =
            g.id == CGoal::kDefaultId &&
            (g.title.empty() || g.title == CGoal::kDefaultTitle) &&
            g.purpose.empty() &&
            g.criteria.empty() &&
            g.progress == 0;
        if (isPlaceholderGoal) continue;
        if (!anyConfiguredGoal) addHeader(L"목표");
        anyConfiguredGoal = true;
        CSidebar::Row gr;
        gr.kind = CSidebar::RowKind::Goal;
        gr.level = 0;
        gr.title = g.title.empty()
            ? Utf8ToWide(g.id.c_str(), (int)g.id.size())
            : Utf8ToWide(g.title.c_str(), (int)g.title.size());
        gr.status = g.status;
        gr.verdict = g.verdict;
        gr.progress = g.progress;
        gr.goalId = g.id;
        rows.push_back(std::move(gr));

        auto projects = Persistence::ListProjects(g.id);
        for (const auto& p : projects) {
            CSidebar::Row pr;
            pr.kind = CSidebar::RowKind::Project;
            pr.level = 1;
            pr.title = p.title.empty()
                ? Utf8ToWide(p.id.c_str(), (int)p.id.size())
                : Utf8ToWide(p.title.c_str(), (int)p.title.size());
            pr.status = p.status;
            pr.verdict = p.verdict;
            pr.progress = p.progress;
            pr.goalId = g.id;
            pr.projectId = p.id;
            rows.push_back(std::move(pr));

            auto chats = Persistence::ListChatsIn(g.id, p.id);
            for (const auto& s : chats) {
                CSidebar::Row r;
                r.kind = CSidebar::RowKind::Chat;
                r.level = 2;
                r.title = chatTitle(s, 24);
                r.progress = s.progress;
                r.trailing = Persistence::FormatMtimeShort(s.mtime);
                r.isCurrent = (s.key == currentKey);
                r.lastActiveIso = s.lastActiveIso;
                r.chatKey = s.key;
                r.goalId = g.id;
                r.projectId = p.id;
                if (r.isCurrent) selectedIndex = (int)rows.size();
                rows.push_back(std::move(r));
            }
            CSidebar::Row nr;
            nr.kind = CSidebar::RowKind::NewChat;
            nr.level = 2;
            nr.title = L"새 대화";
            nr.goalId = g.id;
            nr.projectId = p.id;
            rows.push_back(std::move(nr));
        }
    }
    m_sidebar->Refill(std::move(rows));
    if (m_sidebar) m_sidebar->SetCurrentChat(currentKey);
    if (m_sidebar) m_sidebar->SetSelectedIdx(selectedIndex);
}

void CMainWindow::UpdateWindowTitle() {
    if (!m_hwnd) return;

    std::wstring label;
    if (!m_currentChatSession.title.empty()) {
        label = Utf8ToWide(m_currentChatSession.title.c_str(), (int)m_currentChatSession.title.size());
    }
    if (label.empty() && !m_currentChatKey.empty()) {
        label = Utf8ToWide(m_currentChatKey.c_str(), (int)m_currentChatKey.size());
    }

    label = TrimWide(label);
    if (label.empty()) {
        SetWindowTextW(m_hwnd, kAppTitleBase);
        return;
    }
    if (label.size() > 64) label = label.substr(0, 61) + L"...";

    std::wstring title = std::wstring(kAppTitleBase) + L" - " + label;
    SetWindowTextW(m_hwnd, title.c_str());
}

void CMainWindow::CreateGlobalChat() {
    CreateChatDraft();
}

void CMainWindow::RenameChatSession(const CSidebar::Row& row) {
    if (row.kind != CSidebar::RowKind::Chat || row.chatKey.empty()) return;

    std::wstring newTitle = PromptForName(m_hwnd, L"채팅 이름 변경", L"새 이름", row.title.c_str());
    newTitle = TrimWide(newTitle);
    if (newTitle.empty()) return;

    std::string next = WideToUtf8(newTitle);
    Json::Value result = Persistence::ChatApiRename(row.chatKey, next);
    if (result.isMember("error")) {
        std::wstring error = JsonStringWide(result, "error");
        if (error.empty()) error = L"chat rename failed";
        MessageBoxW(m_hwnd, error.c_str(), L"채팅 이름 변경 실패", MB_OK | MB_ICONWARNING);
        return;
    }

    if (row.chatKey == m_currentChatKey) {
        m_currentChatSession.title = next;
        UpdateWindowTitle();
    }
    RefillSidebar();

    UINT msg = ChatSessionsChangedMessage();
    if (msg) PostMessageW(HWND_BROADCAST, msg, 0, 0);
}

void CMainWindow::RenameSidebarMeta(const CSidebar::Row& row) {
    if (row.kind == CSidebar::RowKind::Goal && !row.goalId.empty()) {
        CGoal g = CGoal::Load(row.goalId);
        std::wstring oldTitle = g.title.empty()
            ? Utf8ToWide(row.goalId.c_str(), (int)row.goalId.size())
            : Utf8ToWide(g.title.c_str(), (int)g.title.size());
        std::wstring newTitle = PromptForName(m_hwnd, L"목표 이름 변경", L"목표 이름", oldTitle.c_str());
        newTitle = TrimWide(newTitle);
        if (newTitle.empty()) return;
        std::string next = WideToUtf8(newTitle);
        if (next == g.title) return;
        if (g.id.empty()) g.id = row.goalId;
        g.title = next;
        CGoal::Save(g);
        RefillSidebar();
        return;
    }

    if (row.kind == CSidebar::RowKind::Project &&
        !row.goalId.empty() && !row.projectId.empty()) {
        CProject p = CProject::Load(row.goalId, row.projectId);
        std::wstring oldTitle = p.title.empty()
            ? Utf8ToWide(row.projectId.c_str(), (int)row.projectId.size())
            : Utf8ToWide(p.title.c_str(), (int)p.title.size());
        std::wstring newTitle = PromptForName(m_hwnd, L"프로젝트 이름 변경", L"프로젝트 이름", oldTitle.c_str());
        newTitle = TrimWide(newTitle);
        if (newTitle.empty()) return;
        std::string next = WideToUtf8(newTitle);
        if (next == p.title) return;
        if (p.id.empty()) p.id = row.projectId;
        if (p.goalId.empty()) p.goalId = row.goalId;
        p.title = next;
        CProject::Save(p);
        RefillSidebar();
    }
}

void CMainWindow::QueueSidebarAction(const CSidebar::Row& row) {
    m_pendingSidebarAction = SidebarAction{};
    m_pendingSidebarAction.kind = row.kind;
    m_pendingSidebarAction.chatKey = row.chatKey;
    m_pendingSidebarAction.goalId = row.goalId;
    m_pendingSidebarAction.projectId = row.projectId;
    PostMessageW(m_hwnd, kMsgRunSidebarAction, 0, 0);
}

void CMainWindow::RunSidebarAction() {
    SidebarAction action = m_pendingSidebarAction;
    m_pendingSidebarAction = SidebarAction{};

    if (action.kind == CSidebar::RowKind::NewChat) {
        CreateChatDraft(action.goalId, action.projectId);
        return;
    }
    if (action.kind == CSidebar::RowKind::Chat && !action.chatKey.empty()) {
        OpenChatSession(action.chatKey, action.goalId, action.projectId);
        return;
    }
    if (action.kind == CSidebar::RowKind::Project && !action.goalId.empty()) {
        CreateChatDraft(action.goalId, action.projectId);
        return;
    }
}

void CMainWindow::ShowSidebarContextMenu(int screenX, int screenY) {
    if (!m_sidebar) return;
    const CSidebar::Row* row = m_sidebar->SelectedRow();
    if (!row) return;

    HMENU menu = CreatePopupMenu();
    if (!menu) return;

    const bool canOpen = row->kind == CSidebar::RowKind::Chat && !row->chatKey.empty();
    const bool canRenameMeta =
        row->kind == CSidebar::RowKind::Goal ||
        row->kind == CSidebar::RowKind::Project;
    const bool canNewChat =
        row->kind == CSidebar::RowKind::NewChat ||
        row->kind == CSidebar::RowKind::Project;

    if (canOpen) {
        AppendMenuW(menu, MF_STRING, kSidebarMenuOpen, L"열기");
        AppendMenuW(menu, MF_STRING, kSidebarMenuRename, L"이름 바꾸기");
    }
    if (canRenameMeta) {
        AppendMenuW(menu, MF_STRING, kSidebarMenuRename, L"이름 바꾸기");
    }
    if (canNewChat) {
        AppendMenuW(menu, MF_STRING, kSidebarMenuNewChat, L"새 대화");
    }
    if (!canOpen && !canRenameMeta && !canNewChat) {
        AppendMenuW(menu, MF_GRAYED, 0, L"사용 가능한 동작 없음");
    }

    SetForegroundWindow(m_hwnd);
    UINT cmd = TrackPopupMenu(menu,
                              TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY,
                              screenX, screenY, 0, m_hwnd, nullptr);
    DestroyMenu(menu);

    if (cmd == kSidebarMenuOpen && canOpen) {
        OpenChatSession(row->chatKey, row->goalId, row->projectId);
    } else if (cmd == kSidebarMenuNewChat && canNewChat) {
        CreateChatDraft(row->goalId, row->projectId);
    } else if (cmd == kSidebarMenuRename && canRenameMeta) {
        RenameSidebarMeta(*row);
    } else if (cmd == kSidebarMenuRename && canOpen) {
        RenameChatSession(*row);
    }
}

bool CMainWindow::HasConfiguredGoal() const {
    auto goals = Persistence::ListAllGoals();
    for (const auto& g : goals) {
        const bool isPlaceholderGoal =
            g.id == CGoal::kDefaultId &&
            (g.title.empty() || g.title == CGoal::kDefaultTitle) &&
            g.purpose.empty() &&
            g.criteria.empty() &&
            g.progress == 0;
        if (!isPlaceholderGoal) return true;
    }
    return false;
}

void CMainWindow::ActivateGoal(const std::string& goalId) {
    if (goalId.empty()) return;
    m_activeGoalId = CGoal::SanitizeId(goalId);
    m_activeProjectId = CProject::kDefaultId;
    SetEnvironmentVariableA("ORANGE_CODE_GOAL_ID", m_activeGoalId.c_str());
    SetEnvironmentVariableA("ORANGE_CODE_PROJECT_ID", m_activeProjectId.c_str());
}

void CMainWindow::EnsureGoalChat() {
    if (m_activeGoalId.empty()) {
        auto goals = Persistence::ListAllGoals();
        for (const auto& g : goals) {
            const bool isPlaceholderGoal =
                g.id == CGoal::kDefaultId &&
                (g.title.empty() || g.title == CGoal::kDefaultTitle) &&
                g.purpose.empty() &&
                g.criteria.empty() &&
                g.progress == 0;
            if (!isPlaceholderGoal) {
                ActivateGoal(g.id);
                break;
            }
        }
    }
    if (m_activeGoalId.empty()) return;

    if (m_currentChatKey.empty() || m_currentChatKey == "default") {
        m_currentChatKey = NewChatKey();
        SetEnvironmentVariableA("ORANGE_CODE_SESSION_KEY", m_currentChatKey.c_str());
        m_currentChatSession = ChatSession{};
        m_currentChatSession.sessionId.clear();
    }
    CProject::EnsureExists(m_activeGoalId, m_activeProjectId, CProject::kDefaultTitle);
}

bool CMainWindow::CreateGoalFromTitle(const std::wstring& title) {
    std::wstring cleanTitle = TrimWide(title);
    if (cleanTitle.empty()) return false;

    CGoal g;
    g.id = SlugFromTitle(cleanTitle);
    g.title = WideToUtf8(cleanTitle);
    g.createdAtIso = CCoordination::TimestampNow();
    g.lastActiveIso = g.createdAtIso;
    g.purpose = WideToUtf8(cleanTitle);
    g.criteria = "The manager can create projects, run build/capture verification, and report progress against this goal.";
    g.status = CWorkStatus::InProgress;
    g.progress = 1;
    if (!CGoal::Save(g)) return false;

    if (m_input) m_input->SetPlaceholder(L"메시지 입력 — Enter 전송, Shift+Enter 줄바꿈");
    RefillSidebar();
    if (m_view) {
        m_view->NewBlock(L"task");
        m_view->AppendText(L"목표 생성 완료\n\n- 목표: " + cleanTitle + L"\n- 다음: `/project 프로젝트명` 으로 하위 프로젝트를 만들 수 있습니다.");
        m_view->ScrollLatestIntoView();
    }
    return true;
}

bool CMainWindow::CreateProjectFromTitle(const std::wstring& title) {
    std::wstring cleanTitle = TrimWide(title);
    if (cleanTitle.empty()) return false;

    auto goals = Persistence::ListAllGoals();
    std::string goalId;
    for (const auto& g : goals) {
        const bool isPlaceholderGoal =
            g.id == CGoal::kDefaultId &&
            (g.title.empty() || g.title == CGoal::kDefaultTitle) &&
            g.purpose.empty() &&
            g.criteria.empty() &&
            g.progress == 0;
        if (!isPlaceholderGoal) {
            goalId = g.id;
            break;
        }
    }
    if (goalId.empty()) return false;

    CProject p;
    p.id = SlugFromTitle(cleanTitle);
    p.goalId = goalId;
    p.title = WideToUtf8(cleanTitle);
    p.createdAtIso = CCoordination::TimestampNow();
    p.lastActiveIso = p.createdAtIso;
    p.purpose = WideToUtf8(cleanTitle);
    p.criteria = "This project has concrete chats, file changes, verification, and a concise result.";
    p.status = CWorkStatus::InProgress;
    p.progress = 1;
    if (!CProject::Save(p)) return false;

    RefillSidebar();
    if (m_view) {
        m_view->NewBlock(L"task");
        m_view->AppendText(L"프로젝트 생성 완료\n\n- 프로젝트: " + cleanTitle);
        m_view->ScrollLatestIntoView();
    }
    return true;
}

void CMainWindow::SubmitFromInput() {
    std::wstring text = m_input->GetText();
    if (text.empty()) return;
    m_input->SetText(L"");
    DispatchPrompt(text);
}

void CMainWindow::RunStartupBackendSync() {
    if (m_startupBackendSyncDone) return;
    m_startupBackendSyncDone = true;
    if (!m_backend || m_currentChatKey.empty()) return;
    if (m_pendingBackendCount > 0 || m_buildRunning) return;

    bool useClaude = (SendMessageW(m_hChkClaude, BM_GETCHECK, 0, 0) == BST_CHECKED);
    bool useGemini = (SendMessageW(m_hChkGemini, BM_GETCHECK, 0, 0) == BST_CHECKED);
    bool useCodex = (SendMessageW(m_hChkCodex, BM_GETCHECK, 0, 0) == BST_CHECKED);
    if (!useClaude && !useGemini && !useCodex) return;

    m_backend->SetUseClaude(useClaude);
    m_backend->SetUseGemini(useGemini);
    m_backend->SetUseCodex(useCodex);

    ChatSession recent = Persistence::LoadRecent(m_currentChatKey, 20);
    std::wstring chatKey = Utf8ToWide(m_currentChatKey.c_str(), static_cast<int>(m_currentChatKey.size()));
    std::wstring prompt;
    prompt += L"OrangeCode app restarted after rebuild or launch.\n";
    prompt += L"Selected chat key: " + chatKey + L"\n";
    prompt += L"Start from the SQLite prompt table and DB job/job_detail briefing injected by OrangeCode.\n";
    prompt += L"Folder markdown files such as MANAGER_HANDOFF.md are supplemental only; do not execute their Next Action list unless the latest DB job or user request explicitly asks for it.\n";
    prompt += L"Review the recent chat blocks below only to regain context.\n";
    prompt += L"Pay special attention to user blocks. User messages are the primary source of intent, priority, corrections, and authority.\n";
    prompt += L"Assistant, tool, and error blocks are secondary context and may contain stale, failed, or noisy intermediate work.\n";
    prompt += L"한국어 1-3줄: DB briefing 기준으로 현재 의도와 다음 실행 판단만 짧게 보고.\n";
    prompt += L"The human user is the final decision maker. Do not address the user as any internal review role.\n\n";
    prompt += L"Recent chat blocks:\n";
    for (const auto& b : recent.blocks) {
        if (b.text.empty() || b.role == L"pending-response" || b.role == L"attachment") continue;
        std::wstring text = b.text;
        for (auto& ch : text) {
            if (ch == L'\r' || ch == L'\n' || ch == L'\t') ch = L' ';
        }
        if (text.size() > 500) text = text.substr(0, 500) + L"...";
        prompt += L"- [" + b.role + L"] " + text + L"\n";
    }

    m_lastBackendLabel = BackendLabel(useClaude, useGemini, useCodex);
    int sent = m_backend->SendMessage(m_currentChatKey, prompt);
    if (sent > 0) {
        m_pendingBackendCount = sent;
        if (m_view) m_view->SetBusy(true);
    }
}

void CMainWindow::RunAutoResumePrompt() {
    if (m_startupBackendSyncDone) return;
    m_startupBackendSyncDone = true;
    if (!m_backend || m_currentChatKey.empty()) return;
    if (m_pendingBackendCount > 0 || m_buildRunning) return;

    ChatSession recent = Persistence::LoadRecent(m_currentChatKey, 15);

    // Skip auto-resume if there's no prior user interaction
    bool hasUserBlock = false;
    for (const auto& b : recent.blocks) {
        if (b.role == L"user" && !b.text.empty()) { hasUserBlock = true; break; }
    }
    if (!hasUserBlock) return;

    // Skip if last meaningful block is a retry (rate-limit) or already an auto-resume.
    // Sending again immediately would just hit the same rate limit.
    for (int i = (int)recent.blocks.size() - 1; i >= 0; --i) {
        const auto& b = recent.blocks[i];
        if (b.text.empty()) continue;
        if (b.role == L"retry") return;
        if (b.role == L"user" && b.text.find(L"[앱 재시작]") != std::wstring::npos) return;
        break;
    }

    bool useClaude = (SendMessageW(m_hChkClaude, BM_GETCHECK, 0, 0) == BST_CHECKED);
    bool useGemini = (SendMessageW(m_hChkGemini, BM_GETCHECK, 0, 0) == BST_CHECKED);
    bool useCodex = (SendMessageW(m_hChkCodex, BM_GETCHECK, 0, 0) == BST_CHECKED);
    if (!useClaude && !useGemini && !useCodex) return;

    m_backend->SetUseClaude(useClaude);
    m_backend->SetUseGemini(useGemini);
    m_backend->SetUseCodex(useCodex);

    std::wstring prompt;
    prompt += L"OrangeCode internal restart context sync.\n";
    prompt += L"Use the SQLite prompt table and DB job/job_detail briefing as the highest-priority source.\n";
    prompt += L"This is not a new user instruction and must not create or resume markdown backlog work by itself.\n\n";
    prompt += L"Recent chat blocks:\n";
    for (const auto& b : recent.blocks) {
        if (b.text.empty() || b.role == L"pending-response" || b.role == L"attachment") continue;
        std::wstring text = b.text;
        for (auto& ch : text) {
            if (ch == L'\r' || ch == L'\n' || ch == L'\t') ch = L' ';
        }
        if (text.size() > 200) text = text.substr(0, 200) + L"…";
        prompt += L"- [" + b.role + L"] " + text + L"\n";
    }
    prompt += L"\n한국어 1-3줄: DB briefing 기준으로 현재 의도와 다음 실행 판단만 짧게 보고.";

    m_lastBackendLabel = BackendLabel(useClaude, useGemini, useCodex);
    int sent = m_backend->SendMessage(m_currentChatKey, prompt);
    if (sent > 0) {
        m_pendingBackendCount = sent;
        if (m_view) m_view->SetBusy(true);
    }
}

void CMainWindow::DispatchPrompt(const std::wstring& prompt) {
    m_view->NewBlock(L"user");
    m_view->AppendText(prompt);
    SaveChatBlock(L"user", prompt);
    m_lastPrompt = prompt;
    m_waitingForFirstOutput = true;
    m_buildRequestedThisTurn = false;
    m_relaunchPending = false;
    m_forceRelaunchAfterBuild = false;
    KillTimer(m_hwnd, kTimerProviderRelayTimeout);
    m_providerRelayKeys.clear();
    m_providerRelayActive = false;
    m_providerRelayTarget = CBackendManager::OutputSource::System;
    m_providerRelayFallback = CBackendManager::OutputSource::System;
    m_providerRelayTargetLabel.clear();
    m_providerRelayFallbackLabel.clear();
    m_providerRelayExcerpt.clear();
    ResetMeetingState();
    const bool supervisorCall = IsSupervisorCall(prompt);

    if (supervisorCall && TrimWide(prompt) == L"/supervisor") {
        m_waitingForFirstOutput = false;
        return;
    }

    if (prompt == L"/clear" || prompt == L"지우기") {
        m_waitingForFirstOutput = false;
        ClearCurrentChat();
        return;
    }

    if (prompt == L"/build" || prompt == L"빌드") {
        m_waitingForFirstOutput = false;
        m_captureAfterBuild = false;
        StartBuildCommand();
        return;
    }
    if (prompt == L"/capture" || prompt == L"캡처") {
        m_waitingForFirstOutput = false;
        RunCaptureCommand();
        return;
    }
    if (prompt == L"/verify" || prompt == L"검증") {
        m_waitingForFirstOutput = false;
        m_captureAfterBuild = true;
        if (m_view) {
            m_view->NewBlock(L"task");
            m_view->AppendText(L"검증 루프\n\n- 순서: build -> capture\n- 상태: 빌드 시작\n- 원칙: LLM 호출 없이 로컬 검증");
            m_view->ScrollLatestIntoView();
        }
        StartBuildCommand();
        return;
    }
    if (prompt == L"/settings") {
        m_waitingForFirstOutput = false;
        GeminiSettings gs = AppSettings::LoadGeminiSettings();
        BackendSettings bs = AppSettings::LoadBackendSettings();
        std::wstring settingsPath = AppSettings::SettingsPath();
        std::wstring card;
        card += L"설정\n\n";
        card += L"**Gemini**\n";
        card += L"- gemini.model: " + (gs.model.empty() ? L"(CLI 기본값)" : gs.model) + L"\n";
        card += L"  `/gemini.model <모델명>` 으로 변경\n";
        card += L"  `/gemini.model` (인수 없음) 으로 CLI 기본값 복원\n\n";
        card += L"**Backend**\n";
        card += L"- manager_provider: " + Utf8ToWide(bs.managerProvider.c_str(), (int)bs.managerProvider.size()) + L"\n\n";
        card += L"**파일**\n";
        card += L"- 경로: " + settingsPath;
        m_view->NewBlock(L"task");
        m_view->AppendText(card);
        m_view->ScrollLatestIntoView();
        SaveChatBlock(L"task", card);
        return;
    }
    {
        std::wstring trimmedCmd = TrimWide(prompt);
        if (trimmedCmd.rfind(L"/autoloop", 0) == 0) {
            m_waitingForFirstOutput = false;
            std::wstring loopArg;
            if (trimmedCmd.size() > 9) loopArg = TrimWide(trimmedCmd.substr(9));
            ToggleAutoLoop(loopArg);
            return;
        }
        if (trimmedCmd.rfind(L"/gemini.model", 0) == 0) {
            m_waitingForFirstOutput = false;
            std::wstring modelArg;
            if (trimmedCmd.size() > 13) modelArg = TrimWide(trimmedCmd.substr(13));
            bool ok = AppSettings::SaveGeminiModel(modelArg);
            std::wstring card;
            card += L"Gemini 모델 설정\n\n";
            if (ok) {
                card += L"- 설정: " + (modelArg.empty() ? L"(CLI 기본값으로 초기화)" : modelArg) + L"\n";
                card += L"- 적용: 다음 Gemini 호출부터 반영됩니다.";
            } else {
                card += L"- 실패: settings.json 저장에 실패했습니다.";
            }
            m_view->NewBlock(ok ? L"task" : L"error");
            m_view->AppendText(card);
            m_view->ScrollLatestIntoView();
            SaveChatBlock(ok ? L"task" : L"error", card);
            return;
        }
    }
    std::wstring arg;
    if (ConsumePrefix(prompt, L"/goal ", arg) || ConsumePrefix(prompt, L"목표 ", arg)) {
        if (!CreateGoalFromTitle(arg) && m_view) {
            m_view->NewBlock(L"error");
            m_view->AppendText(L"목표 생성 실패\n\n목표명을 다시 확인하세요.");
        }
        return;
    }
    if (ConsumePrefix(prompt, L"/project ", arg) || ConsumePrefix(prompt, L"프로젝트 ", arg)) {
        if (!CreateProjectFromTitle(arg) && m_view) {
            m_view->NewBlock(L"error");
            m_view->AppendText(L"프로젝트 생성 실패\n\n먼저 목표를 만들어야 합니다.");
        }
        return;
    }

    if (Persistence::EnsureChatDatabase()) {
        std::wstring title = TrimWide(prompt);
        if (title.size() > 120) title = title.substr(0, 120) + L"...";
        const bool ownerInstruction =
            prompt.find(L"대표님") != std::wstring::npos ||
            prompt.find(L"프롬 대표님") != std::wstring::npos ||
            prompt.find(L"from owner") != std::wstring::npos;
        Json::Value job;
        job["chat_key"] = m_currentChatKey;
        job["title"] = WideToUtf8(title);
        job["instruction_type"] = ownerInstruction ? "owner" : "normal";
        job["priority"] = ownerInstruction ? 1000 : 100;
        job["status"] = "in_progress";
        job["progress"] = 0;
        try {
            CDatabase::Instance().Run(
                "RecordUserPromptJob",
                "INSERT INTO job(chat_key, title, instruction_type, priority, status, progress, started_at, updated_at) "
                "VALUES(:chat_key, :title, :instruction_type, :priority, :status, :progress, CURRENT_TIMESTAMP, CURRENT_TIMESTAMP);",
                job);
        } catch (...) {}
    }

    CBackendManager::OutputSource pendingSource = CBackendManager::OutputSource::System;
    bool useClaude = (SendMessageW(m_hChkClaude, BM_GETCHECK, 0, 0) == BST_CHECKED);
    bool useGemini = (SendMessageW(m_hChkGemini, BM_GETCHECK, 0, 0) == BST_CHECKED);
    bool useCodex = (SendMessageW(m_hChkCodex, BM_GETCHECK, 0, 0) == BST_CHECKED);
    std::wstring backendPrompt = prompt;
    const bool routeClaude = (m_startupOptions.managerProvider == "claude");
    const bool routeGemini = (m_startupOptions.managerProvider == "gemini");
    const bool routeCodex = (m_startupOptions.managerProvider == "codex");
    if (routeClaude) useClaude = true;
    else if (routeGemini) useGemini = true;
    else if (routeCodex) useCodex = true;
    SendMessageW(m_hChkClaude, BM_SETCHECK, useClaude ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(m_hChkGemini, BM_SETCHECK, useGemini ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(m_hChkCodex, BM_SETCHECK, useCodex ? BST_CHECKED : BST_UNCHECKED, 0);
    if (m_input) {
        m_input->SetProviderState(useClaude, useGemini, useCodex);
        m_input->SetManagerProvider(m_startupOptions.managerProvider);
    }
    if (!m_startupOptions.backendFromFlag)
        AppSettings::SaveBackendSettings(useClaude, useGemini, useCodex, m_startupOptions.managerProvider);
    const bool councilRouting = SelectedBackendCount(useClaude, useGemini, useCodex) > 1;
    std::wstring councilRoster;
    std::wstring managerLabel = ManagerProviderLabel();
    std::wstring selectedRoster = CouncilRoster(useClaude, useGemini, useCodex);
    std::wstring workPlan;
    if (supervisorCall) {
        workPlan = managerLabel + L" 정팀장이 supervisor 호출에 응답하고, 선택된 백엔드 상태를 기준으로 다음 실행 방향을 정리";
        backendPrompt =
            L"Internal reviewer call.\n"
            L"정팀장은 manager 본체이고, supervisor는 내부 검토 역할입니다.\n"
            L"이번 호출은 UI에서 오렌지로 임명된 정팀장 provider가 응답합니다.\n"
            L"지정 정팀장 provider: " + managerLabel + L"\n"
            L"현재 선택된 백엔드: " + selectedRoster + L"\n"
            L"사용자가 최종 의사결정자입니다. 내부 role 별칭을 사용자 호칭으로 쓰지 마세요.\n\n"
            L"대표님 요청:\n" + prompt;
    } else if (councilRouting) {
        councilRoster = CouncilRoster(useClaude, useGemini, useCodex);
        workPlan = managerLabel + L" 정팀장이 회의를 주도하고, " + councilRoster +
                   L" 발언을 수집한 뒤 실행 권한과 검토 역할을 분리";
        backendPrompt = BuildCouncilRoutingPrompt(prompt, councilRoster, managerLabel);
        m_meetingActive = true;
        m_meetingIssue = prompt;
        m_meetingRoster = councilRoster;
        m_meetingManager = managerLabel;

    } else {
        workPlan = managerLabel + L" 단일 정팀장이 요청을 분석하고 필요한 경우 orange 도구로 build/capture/exec를 요청";
    }
    if (m_backend) {
        m_backend->SetUseClaude(useClaude);
        m_backend->SetUseGemini(useGemini);
        m_backend->SetUseCodex(useCodex);
    }
    backendPrompt =
        L"OrangeCode coordination policy:\n"
        L"- Selected backend providers: " + selectedRoster + L".\n"
        L"- The appointed manager provider for this chat is " + managerLabel + L".\n"
        L"- Authority order is: user instruction > appointed manager execution > reviewer advice.\n"
        L"- The appointed manager coordinates work and may delegate analysis/review to the other selected providers.\n"
        L"- After selected providers have given opinions, or when discussion repeats or stalls, the appointed manager must close with a decision and immediately take the next concrete action.\n"
        L"- Do not end with one-turn opinions only; reviewers then follow the manager decision as the working direction.\n"
        L"- If the latest user message explicitly asks to fix, edit, implement, add, build, verify, or update rules, the appointed manager must enter execution mode immediately after checking only the relevant evidence.\n"
        L"- In execution mode, do not keep discussing. Inspect the relevant files, make the smallest safe patch, then request build/capture when appropriate.\n"
        L"- Non-manager providers should act as advisors/reviewers unless OrangeCode grants explicit implementation tools.\n"
        L"- To delegate real work, the manager may output standalone JSON: "
        L"{\"tool\":\"orange.delegate\",\"args\":{\"providers\":[\"gemini\",\"codex\"],\"prompt\":\"specific subtask\"}}.\n"
        L"- Do not delegate backlog/task-file work unless the user's latest message explicitly requested that task.\n"
        L"- Never start auto_retry_rate_limit or other historical task names just because they exist in repository context.\n"
        L"- Providers may repeat, for example [\"claude\",\"claude\"], when independent parallel Claude workers are useful.\n"
        L"- Keep delegated subtasks concrete and bounded; delegate only when review or implementation would reduce risk or speed up the current explicit task.\n"
        L"- Provider relay is command-based, not plain text mention-based. Use `/relay Codex: ...`, `/ask Gemini: ...`, `/전달 Claude: ...`, `/호출 Codex: ...`, or `@Claude ...`; if the target stays silent, the asking provider continues as fallback.\n"
        L"- As each step proceeds, if a repeatable operating method is derived, treat it as a rule candidate and record the concise rule in the relevant durable file before relying on it in later sessions.\n"
        L"- Rule updates must stay scoped to the current evidence and must not create unrelated backlog work.\n\n" +
        backendPrompt;
    m_lastBackendLabel = BackendLabel(useClaude, useGemini, useCodex);
    if (useGemini && !useClaude && !useCodex) pendingSource = CBackendManager::OutputSource::Gemini;
    else if (useCodex && !useClaude && !useGemini) pendingSource = CBackendManager::OutputSource::Codex;
    else if (useClaude && !useGemini && !useCodex) pendingSource = CBackendManager::OutputSource::Claude;
    else if (useGemini) pendingSource = CBackendManager::OutputSource::Gemini;
    else if (useCodex) pendingSource = CBackendManager::OutputSource::Codex;

    m_currentAssistantRole = SourceRole(pendingSource);
    MarkResponsePending(SourceLabel(pendingSource));
    m_view->NewBlock(m_currentAssistantRole);
    m_pendingAssistantBlock = m_view->BlockCount() - 1;
    m_view->AppendText(L"__orange_pending_indicator__");

    if (m_startupOptions.useMockBackend) {
        m_seqQueue.clear();
        m_pendingBackendCount = 1;
        if (m_view) m_view->SetBusy(true);
        SetTimer(m_hwnd, kTimerMockOutput, m_startupOptions.testResponseDelayMs, nullptr);
        return;
    }
    if (councilRouting && m_backend) {
        // Sequential routing: manager responds first, reviewers receive manager's response as context
        m_seqManagerResponse.clear();
        m_seqBasePrompt = backendPrompt;
        m_seqQueue.clear();
        m_seqFinalManagerRequested = false;
        if (m_startupOptions.managerProvider == "claude") {
            m_seqManagerSource = CBackendManager::OutputSource::Claude;
            if (useGemini) m_seqQueue.push_back(CBackendManager::OutputSource::Gemini);
            if (useCodex)  m_seqQueue.push_back(CBackendManager::OutputSource::Codex);
        } else if (m_startupOptions.managerProvider == "gemini") {
            m_seqManagerSource = CBackendManager::OutputSource::Gemini;
            if (useClaude) m_seqQueue.push_back(CBackendManager::OutputSource::Claude);
            if (useCodex)  m_seqQueue.push_back(CBackendManager::OutputSource::Codex);
        } else {
            m_seqManagerSource = CBackendManager::OutputSource::Codex;
            if (useClaude) m_seqQueue.push_back(CBackendManager::OutputSource::Claude);
            if (useGemini) m_seqQueue.push_back(CBackendManager::OutputSource::Gemini);
        }
        m_pendingBackendCount = 1 + static_cast<int>(m_seqQueue.size());
        if (m_view) m_view->SetBusy(true);
        int sent = m_backend->SendToProviders(m_currentChatKey, backendPrompt, {m_seqManagerSource});
        if (sent == 0) {
            m_pendingBackendCount = 0;
            m_seqQueue.clear();
            m_waitingForFirstOutput = false;
            ClearResponsePendingMarkers();
            if (m_view) m_view->SetBusy(false);
        }
    } else {
        m_seqQueue.clear();
        m_pendingBackendCount = (useClaude ? 1 : 0) + (useGemini ? 1 : 0) + (useCodex ? 1 : 0);
        if (m_view) m_view->SetBusy(m_pendingBackendCount > 0);
        int sentCount = m_backend ? m_backend->SendMessage(m_currentChatKey, backendPrompt) : 0;
        if (sentCount != m_pendingBackendCount) {
            m_pendingBackendCount = sentCount;
            if (sentCount <= 0) {
                m_waitingForFirstOutput = false;
                ClearResponsePendingMarkers();
            }
            if (m_view) m_view->SetBusy(m_pendingBackendCount > 0);
        }
    }
}

unsigned __stdcall RunAdminToolThreadProc(void* raw) {
    std::unique_ptr<AdminToolThreadPacket> ctx(static_cast<AdminToolThreadPacket*>(raw));
    DWORD started = GetTickCount();
    auto* result = new AdminToolResultPacket();
    result->chatKey = ctx->chatKey;
    result->provider = ctx->provider;
    result->command = ctx->command;

    std::wstring params = L"/c \"" + ctx->command + L"\"";
    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    sei.hwnd = ctx->hwnd;
    sei.lpVerb = L"runas";
    sei.lpFile = L"cmd.exe";
    sei.lpParameters = params.c_str();
    sei.lpDirectory = ctx->cwd.empty() ? nullptr : ctx->cwd.c_str();
    sei.nShow = SW_SHOWNORMAL;

    if (ShellExecuteExW(&sei)) {
        result->launched = true;
        if (sei.hProcess) {
            WaitForSingleObject(sei.hProcess, INFINITE);
            GetExitCodeProcess(sei.hProcess, &result->exitCode);
            CloseHandle(sei.hProcess);
        }
    } else {
        result->launched = false;
        result->exitCode = GetLastError();
    }
    result->elapsedMs = GetTickCount() - started;
    PostMessageW(ctx->hwnd, kMsgAdminToolDone, 0, reinterpret_cast<LPARAM>(result));
    return 0;
}

bool CMainWindow::TryHandleToolRequest(const std::string& chatKey,
                                       CBackendManager::OutputSource source,
                                       const std::wstring& text) {
    Json::Value root;
    if (!ParseToolRequest(text, root)) return false;
    std::string tool = root.get("tool", "").asString();
    const Json::Value& args = root["args"].isObject() ? root["args"] : root;
    std::wstring provider = SourceLabel(source);
    if ((chatKey.empty() || chatKey == m_currentChatKey) && m_waitingForFirstOutput) {
        if (m_pendingAssistantBlock != (size_t)-1 && m_view &&
            m_pendingAssistantBlock < m_view->BlockCount()) {
            m_view->SetBlockText(m_pendingAssistantBlock, L"");
        }
        m_waitingForFirstOutput = false;
    }

    if (tool == "orange.build") {
        const auto& mp = m_startupOptions.managerProvider;
        bool isManager = (mp == "claude" && source == CBackendManager::OutputSource::Claude) ||
                         (mp == "gemini" && source == CBackendManager::OutputSource::Gemini) ||
                         (mp == "codex"  && source == CBackendManager::OutputSource::Codex);
        if (!isManager) {
            OutputDebugStringW((L"[guard] orange.build 거부: 실행자 아님 (provider: " + provider + L")\n").c_str());
            return true;
        }
        if (m_buildRequestedThisTurn) {
            OutputDebugStringW(L"[guard] orange.build 거부: 이미 요청된 턴\n");
            return true;
        }
        m_buildRequestedThisTurn = true;
        m_forceRelaunchAfterBuild = true;
        std::wstring card = L"도구 요청\n\n- 도구: orange.build\n- 요청자: " + provider + L"\n- 상태: 정팀장이 빌드 대행";
        if (chatKey == m_currentChatKey && m_view) {
            m_view->NewBlock(L"tool");
            m_view->AppendText(card);
            m_view->ScrollLatestIntoView();
            StartBuildCommand();
        }
        if (chatKey == m_currentChatKey) SaveChatBlock(L"tool", card);
        else SaveBlockToChatKey(chatKey, L"tool", card);
        return true;
    }

    if (tool == "orange.capture") {
        const auto& mp = m_startupOptions.managerProvider;
        bool isManager = (mp == "claude" && source == CBackendManager::OutputSource::Claude) ||
                         (mp == "gemini" && source == CBackendManager::OutputSource::Gemini) ||
                         (mp == "codex"  && source == CBackendManager::OutputSource::Codex);
        if (!isManager) {
            OutputDebugStringW((L"[guard] orange.capture 거부: 실행자 아님 (provider: " + provider + L")\n").c_str());
            return true;
        }
        std::wstring card = L"도구 요청\n\n- 도구: orange.capture\n- 요청자: " + provider + L"\n- 상태: 정팀장이 화면 캡처 대행";
        if (chatKey == m_currentChatKey && m_view) {
            m_view->NewBlock(L"tool");
            m_view->AppendText(card);
            m_view->ScrollLatestIntoView();
            RunCaptureCommand();
        }
        if (chatKey == m_currentChatKey) SaveChatBlock(L"tool", card);
        else SaveBlockToChatKey(chatKey, L"tool", card);
        return true;
    }

    if (tool == "orange.delegate") {
        return HandleDelegateTool(chatKey, source, args);
    }

    if (tool == "orange.exec_admin") {
        std::wstring command = JsonStringWide(args, "command");
        std::wstring cwd = JsonStringWide(args, "cwd");
        std::wstring reason = JsonStringWide(args, "reason");
        HandleExecAdminTool(chatKey, source, command, cwd, reason);
        return true;
    }

    std::wstring card = L"도구 요청 차단\n\n- 도구: " + Utf8ToWide(tool.c_str(), (int)tool.size()) +
                        L"\n- 요청자: " + provider +
                        L"\n- 이유: 아직 구현되지 않은 orange 도구";
    if (chatKey == m_currentChatKey && m_view) {
        m_view->NewBlock(L"error");
        m_view->AppendText(card);
        m_view->ScrollLatestIntoView();
    }
    if (chatKey == m_currentChatKey) SaveChatBlock(L"error", card);
    else SaveBlockToChatKey(chatKey, L"error", card);
    return true;
}

void CMainWindow::HandleExecAdminTool(const std::string& chatKey,
                                      CBackendManager::OutputSource source,
                                      const std::wstring& command,
                                      const std::wstring& cwd,
                                      const std::wstring& reason) {
    std::wstring provider = SourceLabel(source);
    std::wstring card;
    card += L"관리자 권한 도구 요청\n\n";
    card += L"- 도구: orange.exec_admin\n";
    card += L"- 요청자: " + provider + L"\n";
    card += L"- 명령: `" + (command.empty() ? L"(empty)" : command) + L"`\n";
    card += L"- 작업 폴더: " + (cwd.empty() ? FindProjectRoot() : cwd) + L"\n";
    card += L"- 이유: " + (reason.empty() ? L"(미기재)" : reason) + L"\n";
    card += L"- 정책: 정팀장이 UAC 승격 실행을 대행";

    if (chatKey == m_currentChatKey && m_view) {
        m_view->NewBlock(L"tool");
        m_view->AppendText(card);
        m_view->ScrollLatestIntoView();
    }
    if (chatKey == m_currentChatKey) SaveChatBlock(L"tool", card);
    else SaveBlockToChatKey(chatKey, L"tool", card);

    if (command.empty()) {
        std::wstring err = L"관리자 도구 차단\n\n- 이유: command가 비어 있습니다.";
        if (chatKey == m_currentChatKey && m_view) {
            m_view->NewBlock(L"error");
            m_view->AppendText(err);
        }
        if (chatKey == m_currentChatKey) SaveChatBlock(L"error", err);
        else SaveBlockToChatKey(chatKey, L"error", err);
        return;
    }

    auto* packet = new AdminToolThreadPacket();
    packet->hwnd = m_hwnd;
    packet->chatKey = chatKey.empty() ? m_currentChatKey : chatKey;
    packet->provider = provider;
    packet->command = command;
    packet->cwd = cwd.empty() ? FindProjectRoot() : cwd;
    uintptr_t h = _beginthreadex(nullptr, 0, RunAdminToolThreadProc, packet, 0, nullptr);
    if (h) {
        CloseHandle(reinterpret_cast<HANDLE>(h));
    } else {
        delete packet;
        std::wstring err = L"관리자 도구 실행 실패\n\n- 이유: 실행 스레드를 시작하지 못했습니다.";
        if (chatKey == m_currentChatKey && m_view) {
            m_view->NewBlock(L"error");
            m_view->AppendText(err);
        }
        if (chatKey == m_currentChatKey) SaveChatBlock(L"error", err);
        else SaveBlockToChatKey(chatKey, L"error", err);
    }
}

bool CMainWindow::IsProviderSelected(CBackendManager::OutputSource source) const {
    if (source == CBackendManager::OutputSource::Claude)
        return SendMessageW(m_hChkClaude, BM_GETCHECK, 0, 0) == BST_CHECKED;
    if (source == CBackendManager::OutputSource::Gemini)
        return SendMessageW(m_hChkGemini, BM_GETCHECK, 0, 0) == BST_CHECKED;
    if (source == CBackendManager::OutputSource::Codex)
        return SendMessageW(m_hChkCodex, BM_GETCHECK, 0, 0) == BST_CHECKED;
    return false;
}

void CMainWindow::TryRelayProviderMention(CBackendManager::OutputSource source, const std::wstring& text) {
    if (!m_backend || source == CBackendManager::OutputSource::System || source == CBackendManager::OutputSource::Mock)
        return;

    const CBackendManager::OutputSource candidates[] = {
        CBackendManager::OutputSource::Claude,
        CBackendManager::OutputSource::Gemini,
        CBackendManager::OutputSource::Codex
    };

    std::string requestedProvider;
    if (!CCoordination::TryParseProviderRelayCommand(text, requestedProvider)) return;
    CBackendManager::OutputSource requestedTarget = CBackendManager::OutputSource::System;
    if (requestedProvider == "claude") requestedTarget = CBackendManager::OutputSource::Claude;
    else if (requestedProvider == "gemini") requestedTarget = CBackendManager::OutputSource::Gemini;
    else if (requestedProvider == "codex") requestedTarget = CBackendManager::OutputSource::Codex;

    for (CBackendManager::OutputSource target : candidates) {
        if (target != requestedTarget || target == source || !IsProviderSelected(target))
            continue;

        std::wstring key = SourceLabel(source) + L"->" + SourceLabel(target);
        if (std::find(m_providerRelayKeys.begin(), m_providerRelayKeys.end(), key) != m_providerRelayKeys.end())
            continue;
        m_providerRelayKeys.push_back(key);

        std::wstring excerpt = text;
        while (!excerpt.empty() && iswspace(excerpt.front())) excerpt.erase(excerpt.begin());
        while (!excerpt.empty() && iswspace(excerpt.back())) excerpt.pop_back();
        if (excerpt.size() > 1800) excerpt = excerpt.substr(0, 1800) + L"...";

        std::wstring prompt;
        prompt += L"OrangeCode provider relay.\n";
        prompt += L"- " + SourceLabel(source) + L" explicitly called " + SourceLabel(target) + L" with an OrangeCode relay command in the current council discussion.\n";
        prompt += L"- Respond only to this handoff. If you are not the appointed manager, stay in reviewer/advisor mode and do not request orange tools.\n";
        prompt += L"- If this changes the working direction, state the concrete correction briefly.\n\n";
        prompt += L"Source message from " + SourceLabel(source) + L":\n";
        prompt += excerpt;

        int sent = m_backend->SendToProviders(m_currentChatKey, prompt, {target});
        if (sent > 0) {
            m_pendingBackendCount += sent;
            if (m_view) m_view->SetBusy(true);

            if (!m_providerRelayActive) {
                m_providerRelayActive = true;
                m_providerRelayTarget = target;
                m_providerRelayFallback = source;
                m_providerRelayTargetLabel = SourceLabel(target);
                m_providerRelayFallbackLabel = SourceLabel(source);
                m_providerRelayExcerpt = excerpt;
                SetTimer(m_hwnd, kTimerProviderRelayTimeout, kProviderRelayTimeoutMs, nullptr);
            }

            std::wstring card = L"provider relay\n\n";
            card += L"- from: " + SourceLabel(source) + L"\n";
            card += L"- to: " + SourceLabel(target) + L"\n";
            card += L"- command: /relay, /ask, /전달, /호출, or @provider\n";
            card += L"- fallback: no response within 90 seconds -> " + SourceLabel(source) + L" continues";
            if (m_view) {
                m_view->NewBlock(L"task");
                m_view->AppendText(card);
                m_view->ScrollLatestIntoView();
            }
            SaveChatBlock(L"task", card);
            m_currentAssistantRole.clear();
        }
        return;
    }
}

void CMainWindow::HandleProviderRelayTimeout() {
    KillTimer(m_hwnd, kTimerProviderRelayTimeout);
    if (!m_providerRelayActive || !m_backend) return;

    std::wstring prompt;
    prompt += L"OrangeCode provider relay fallback.\n";
    prompt += L"- " + m_providerRelayTargetLabel + L" was explicitly called, but did not respond within 90 seconds.\n";
    prompt += L"- You are the provider that raised the relay command: " + m_providerRelayFallbackLabel + L".\n";
    prompt += L"- Continue with the best available judgment so the work does not stall. Do not wait for the silent provider.\n\n";
    prompt += L"Original relayed message:\n";
    prompt += m_providerRelayExcerpt;

    if (m_pendingBackendCount > 0) --m_pendingBackendCount;
    int sent = m_backend->SendToProviders(m_currentChatKey, prompt, {m_providerRelayFallback});
    if (sent > 0) {
        m_pendingBackendCount += sent;
        if (m_view) m_view->SetBusy(true);
    }

    std::wstring card = L"provider relay timeout\n\n";
    card += L"- target: " + m_providerRelayTargetLabel + L"\n";
    card += L"- fallback: " + m_providerRelayFallbackLabel + L"\n";
    card += L"- status: silent target bypassed";
    if (m_view) {
        m_view->NewBlock(L"task");
        m_view->AppendText(card);
        m_view->ScrollLatestIntoView();
    }
    SaveChatBlock(L"task", card);
    m_currentAssistantRole.clear();

    m_providerRelayActive = false;
    m_providerRelayTarget = CBackendManager::OutputSource::System;
    m_providerRelayFallback = CBackendManager::OutputSource::System;
    m_providerRelayTargetLabel.clear();
    m_providerRelayFallbackLabel.clear();
    m_providerRelayExcerpt.clear();
}

void CMainWindow::AppendBackendOutput(const std::string& chatKey, CBackendManager::OutputSource source, const std::wstring& text) {
    std::wstring role = SourceRole(source);
    std::wstring clean = text;

    if (m_providerRelayActive && source == m_providerRelayTarget && (chatKey.empty() || chatKey == m_currentChatKey)) {
        KillTimer(m_hwnd, kTimerProviderRelayTimeout);
        m_providerRelayActive = false;
        m_providerRelayTarget = CBackendManager::OutputSource::System;
        m_providerRelayFallback = CBackendManager::OutputSource::System;
        m_providerRelayTargetLabel.clear();
        m_providerRelayFallbackLabel.clear();
        m_providerRelayExcerpt.clear();
    }

    if (TryHandleToolRequest(chatKey, source, clean)) {
        // 도구 카드가 삽입됐으므로 다음 어시스턴트 텍스트는 새 블록으로 시작
        if (chatKey.empty() || chatKey == m_currentChatKey) m_currentAssistantRole.clear();
        return;
    }

    if (m_meetingActive && (chatKey.empty() || chatKey == m_currentChatKey)) {
        if (source == CBackendManager::OutputSource::Claude) m_meetingClaude += clean;
        else if (source == CBackendManager::OutputSource::Gemini) m_meetingGemini += clean;
        else if (source == CBackendManager::OutputSource::Codex) m_meetingCodex += clean;
    }
    if (!m_seqQueue.empty() && source == m_seqManagerSource && (chatKey.empty() || chatKey == m_currentChatKey)) {
        m_seqManagerResponse += clean;
    }

    if (!chatKey.empty() && chatKey != m_currentChatKey) {
        ChatSession session = Persistence::Load(chatKey);
        ChatBlock block;
        block.role = role;
        block.text = clean;
        if (!session.blocks.empty() && session.blocks.back().role == role && role != L"user" && role != L"attachment") {
            session.blocks.back().text += clean;
        } else {
            session.blocks.push_back(std::move(block));
        }
        Persistence::Save(session, chatKey);
        RefillSidebar();
        return;
    }

    if (m_waitingForFirstOutput) {
        std::wstring trimmed = clean;
        while (!trimmed.empty() && (trimmed.front() == L'\r' || trimmed.front() == L'\n' || trimmed.front() == L' ')) trimmed.erase(trimmed.begin());
        while (!trimmed.empty() && (trimmed.back() == L'\r' || trimmed.back() == L'\n' || trimmed.back() == L' ')) trimmed.pop_back();
        if (trimmed == m_lastPrompt || trimmed == (m_lastPrompt + L"\n") || trimmed == (m_lastPrompt + L"\r\n")) return;

        if (m_pendingAssistantBlock != (size_t)-1 && m_pendingAssistantBlock < m_view->BlockCount()) {
            m_view->SetBlockText(m_pendingAssistantBlock, L"");
        }
        ClearResponsePendingMarkers();
        m_waitingForFirstOutput = false;
    }

    if (m_currentAssistantRole != role) {
        m_view->NewBlock(role);
        m_currentAssistantRole = role;
    }
    m_view->AppendText(clean);
    SaveChatBlock(role, clean);
    TryRelayProviderMention(source, clean);
}

void CMainWindow::HandleBackendDone(const std::string& chatKey) {
    if (!chatKey.empty() && chatKey != m_currentChatKey) {
        RefillSidebar();
        return;
    }
    if (m_pendingBackendCount > 0) --m_pendingBackendCount;

    // Sequential chain: if manager finished and reviewers are queued, launch the next one
    if (m_pendingBackendCount > 0 && !m_seqQueue.empty()) {
        CBackendManager::OutputSource nextSrc = m_seqQueue.front();
        m_seqQueue.erase(m_seqQueue.begin());

        std::wstring reviewerPrompt = m_seqBasePrompt;
        reviewerPrompt += L"\n\n[정팀장 " + SourceLabel(m_seqManagerSource) + L" 응답]\n\n";
        reviewerPrompt += m_seqManagerResponse;
        reviewerPrompt += L"\n\n당신은 검토자/조언자 역할입니다. 위 정팀장의 응답을 보고 의견·근거·위험·제안을 한국어로 간결하게 작성하세요.\n";
        reviewerPrompt += L"orange.build 등 실행 도구는 직접 요청하지 마세요.";

        std::wstring nextRole = SourceRole(nextSrc);
        m_currentAssistantRole = nextRole;
        m_waitingForFirstOutput = true;
        MarkResponsePending(SourceLabel(nextSrc));
        if (m_view) {
            m_view->NewBlock(nextRole);
            m_pendingAssistantBlock = m_view->BlockCount() - 1;
            m_view->AppendText(L"__orange_pending_indicator__");
        }
        if (m_backend) m_backend->SendToProviders(m_currentChatKey, reviewerPrompt, {nextSrc});
        return;
    }

    if (m_pendingBackendCount <= 0) {
        if (m_meetingActive && !m_seqFinalManagerRequested &&
            m_seqManagerSource != CBackendManager::OutputSource::System) {
            m_seqFinalManagerRequested = true;

            std::wstring finalPrompt = m_seqBasePrompt;
            finalPrompt += L"\n\n[Council responses collected by OrangeCode]\n";
            if (!m_meetingClaude.empty()) finalPrompt += L"\n[Claude]\n" + m_meetingClaude + L"\n";
            if (!m_meetingGemini.empty()) finalPrompt += L"\n[Gemini]\n" + m_meetingGemini + L"\n";
            if (!m_meetingCodex.empty()) finalPrompt += L"\n[Codex]\n" + m_meetingCodex + L"\n";
            finalPrompt +=
                L"\nYou are the appointed manager provider. Close the council now: state the decision, "
                L"then immediately take the next concrete action. If the action requires an OrangeCode tool, "
                L"output the standalone JSON tool request instead of another opinion turn.";

            m_pendingBackendCount = 1;
            m_waitingForFirstOutput = true;
            MarkResponsePending(SourceLabel(m_seqManagerSource));
            if (m_view) {
                m_view->NewBlock(SourceRole(m_seqManagerSource));
                m_pendingAssistantBlock = m_view->BlockCount() - 1;
                m_view->AppendText(L"__orange_pending_indicator__");
                m_view->SetBusy(true);
            }

            int sent = m_backend ? m_backend->SendToProviders(m_currentChatKey, finalPrompt, {m_seqManagerSource}) : 0;
            if (sent > 0) return;

            m_pendingBackendCount = 0;
            m_waitingForFirstOutput = false;
            ClearResponsePendingMarkers();
        }

        m_pendingBackendCount = 0;
        m_waitingForFirstOutput = false;
        ClearResponsePendingMarkers();
        // 정상 완료 — retry 카운터 초기화 (다음 rate limit은 1회차부터)
        if (m_retryPrompt.empty()) m_retryAttempt = 0;
        if (m_view) m_view->SetBusy(false);
        if (m_meetingActive) AppendMeetingResultCard();
        RefillSidebar();
        if (m_view) m_view->ScrollLatestIntoView();
        if (m_relaunchPending) {
            m_relaunchPending = false;
            RelaunchAfterSuccessfulBuild();
            return;
        }
    }
}

std::wstring CMainWindow::SourceRole(CBackendManager::OutputSource source) const {
    if (source == CBackendManager::OutputSource::Claude) return L"assistant-claude";
    if (source == CBackendManager::OutputSource::Gemini) return L"assistant-gemini";
    if (source == CBackendManager::OutputSource::Codex) return L"assistant-codex";
    if (source == CBackendManager::OutputSource::Mock) return L"assistant-mock";
    if (source == CBackendManager::OutputSource::System) return L"error";
    return L"assistant";
}

std::wstring CMainWindow::SourceLabel(CBackendManager::OutputSource source) const {
    if (source == CBackendManager::OutputSource::Claude) return L"Claude";
    if (source == CBackendManager::OutputSource::Gemini) return L"Gemini";
    if (source == CBackendManager::OutputSource::Codex) return L"Codex";
    if (source == CBackendManager::OutputSource::Mock) return L"Mock";
    return L"System";
}

void CMainWindow::ToggleAutoLoop(const std::wstring& arg) {
    bool turnOn = !m_autoLoopEnabled;
    if (arg == L"on")  turnOn = true;
    if (arg == L"off") turnOn = false;
    if (!arg.empty() && arg != L"on" && arg != L"off") {
        int secs = _wtoi(arg.c_str());
        if (secs > 0) { m_autoLoopIntervalSec = secs; turnOn = true; }
    }
    m_autoLoopEnabled = turnOn;
    KillTimer(m_hwnd, kTimerAutoLoop);
    if (m_autoLoopEnabled)
        SetTimer(m_hwnd, kTimerAutoLoop, (UINT)(m_autoLoopIntervalSec * 1000), nullptr);
    std::wstring card = L"자동 루프\n\n";
    card += L"- 상태: ";
    card += m_autoLoopEnabled ? L"켜짐 ▶" : L"꺼짐 ■";
    if (m_autoLoopEnabled)
        card += L"\n- 간격: " + std::to_wstring(m_autoLoopIntervalSec) + L"초";
    card += L"\n- 사용법: /autoloop on | off | <초>";
    if (m_input) m_input->SetAutoLoopActive(m_autoLoopEnabled);
    if (m_view) {
        m_view->NewBlock(L"task");
        m_view->AppendText(card);
        m_view->ScrollLatestIntoView();
    }
    SaveChatBlock(L"task", card);
}

void CMainWindow::TickAutoLoop() {
    if (!m_autoLoopEnabled) return;
    if (m_pendingBackendCount > 0 || m_buildRunning) return;
    if (m_currentChatKey.empty()) return;
    DispatchPrompt(L"계속 진행해 주세요.");
}

} // namespace orange
