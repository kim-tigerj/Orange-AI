#pragma once

// OrangeView — Direct2D + DirectWrite 자식 윈도우, *블록 기반 가상 스크롤*.
//
// 핵심: 채팅이 1만 줄로 자라도 가볍게 스크롤. 콘텐츠를 *블록* 으로 쪼개 각자 layout 캐시,
// 보이는 블록만 layout 생성·그림. append 시 마지막 블록만 갱신.
//
// 블록 = 보통 한 메시지 단위 (사용자/Claude 1턴). 호출자가 NewBlock() 으로 경계 지정.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>   // GET_X_LPARAM / GET_Y_LPARAM
#include <shellapi.h>   // ShellExecuteW (링크 클릭)
#include <d2d1.h>
#include <dwrite.h>
#include <dwrite_1.h>  // IDWriteTextFormat1::SetLineSpacing 사용 (OrangeView.h:366 부근)
#include <wincodec.h>
#include <functional>
#include <map>
#include <string>
#include <fstream>
#include <ctime>
#include <cwctype>

#include "MarkdownParser.h"
#include "StyledText.h"
#include "Persistence.h"

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "windowscodecs.lib")

namespace orange {

namespace detail {

// 화면에 출력되는 *모든* 변경을 디스크에 영구 기록 — `%APPDATA%\OrangeCode\screen_log.jsonl`.
// 사용자 박음 — *"언제나 화면에 뭔가 출력되는건 항상 기록"*.
// 짧은 응답이 다음 turn 의 placeholder 로 덮여 사라져도 디스크엔 남는다.
inline void LogScreenChange(const char* action, const std::wstring& role,
                            const std::wstring& text)
{
    wchar_t appdata[MAX_PATH] = L"";
    if (GetEnvironmentVariableW(L"APPDATA", appdata, MAX_PATH) == 0) return;
    std::wstring dir = std::wstring(appdata) + L"\\OrangeCode";
    CreateDirectoryW(dir.c_str(), nullptr);
    std::wstring wpath = dir + L"\\screen_log.jsonl";

    SYSTEMTIME st; GetSystemTime(&st);
    char tsBuf[40];
    sprintf_s(tsBuf, "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ",
              st.wYear, st.wMonth, st.wDay,
              st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);

    auto escapeJson = [](const std::string& s) -> std::string {
        std::string out;
        out.reserve(s.size() + 8);
        for (char c : s) {
            switch (c) {
                case '"':  out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\n': out += "\\n";  break;
                case '\r': out += "\\r";  break;
                case '\t': out += "\\t";  break;
                default:
                    if ((unsigned char)c < 0x20) {
                        char esc[8];
                        sprintf_s(esc, "\\u%04x", (unsigned char)c);
                        out += esc;
                    } else {
                        out += c;
                    }
                    break;
            }
        }
        return out;
    };

    std::string line;
    line.reserve(256);
    line += "{\"ts\":\"";
    line += tsBuf;
    line += "\",\"action\":\"";
    line += action;
    line += "\"";

    if (!role.empty()) {
        std::string r = WideToUtf8(role);
        line += ",\"role\":\"" + escapeJson(r) + "\"";
    }
    if (!text.empty()) {
        std::string t = WideToUtf8(text);
        // 너무 길면 잘라서 — entry 비대 회피 (디스크 폭증 방지).
        if (t.size() > 2000) t = t.substr(0, 2000) + "…[truncated]";
        line += ",\"text\":\"" + escapeJson(t) + "\"";
    }
    line += "}\n";

    // append (atomic 하진 않지만 jsonl 한 줄 단위라 손실 영향 작음).
    std::ofstream ofs(WideToUtf8(wpath), std::ios::binary | std::ios::app);
    if (ofs.is_open()) ofs.write(line.data(), line.size());
}

template <class T>
inline void SafeRelease(T*& p) {
    if (p) { p->Release(); p = nullptr; }
}

inline ID2D1Factory* D2DFactory() {
    static ID2D1Factory* s = nullptr;
    if (!s) D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &s);
    return s;
}

inline IDWriteFactory* DWFactory() {
    static IDWriteFactory* s = nullptr;
    if (!s) {
        DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,
                            __uuidof(IDWriteFactory),
                            reinterpret_cast<IUnknown**>(&s));
    }
    return s;
}

}  // namespace detail

class OrangeView {
public:
    static constexpr wchar_t kClassName[] = L"OrangeViewClass";

    OrangeView() = default;
    ~OrangeView() { Cleanup(); }

    OrangeView(const OrangeView&)            = delete;
    OrangeView& operator=(const OrangeView&) = delete;

    static bool EnsureClassRegistered(HINSTANCE hInst) {
        WNDCLASSEXW probe{};
        probe.cbSize = sizeof(probe);
        if (GetClassInfoExW(hInst, kClassName, &probe)) return true;

        WNDCLASSEXW wc{};
        wc.cbSize        = sizeof(wc);
        wc.style         = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;  // 더블클릭 메시지 활성
        wc.lpfnWndProc   = WndProcStatic;
        wc.cbWndExtra    = sizeof(LONG_PTR);
        wc.hInstance     = hInst;
        wc.hCursor       = LoadCursor(nullptr, IDC_IBEAM);
        wc.hbrBackground = nullptr;
        wc.lpszClassName = kClassName;
        return RegisterClassExW(&wc) != 0;
    }

    HWND Create(HWND parent, int x, int y, int w, int h,
                HMENU id, HINSTANCE hInst)
    {
        if (!EnsureClassRegistered(hInst)) return nullptr;
        m_hwnd = CreateWindowExW(
            0, kClassName, L"",
            WS_CHILD | WS_VISIBLE | WS_VSCROLL,
            x, y, w, h, parent, id, hInst, this);
        return m_hwnd;
    }

    HWND Hwnd() const { return m_hwnd; }

    struct AttachmentViewItem {
        std::wstring kind;
        std::wstring name;
        std::wstring sizeLabel;
        std::wstring originalPath;
        std::wstring storedPath;
        std::wstring thumbnailUrl;
        std::wstring fileUrl;
        std::wstring folderUrl;
    };

    struct AttachmentViewBlock {
        std::wstring manifestPath;
        std::wstring manifestFileUrl;
        std::wstring manifestFolderUrl;
        std::vector<AttachmentViewItem> items;
    };

    // 마지막 블록에 텍스트 누적. 블록 없으면 자동 생성.
    void AppendText(const std::wstring& text) {
        if (text.empty()) return;
        if (!m_suppressScreenLog) {
            detail::LogScreenChange("append", m_blocks.empty() ? L"" : m_blocks.back().role, text);
        }
        
        if (m_blocks.empty()) m_blocks.emplace_back();
        auto& last = m_blocks.back();
        last.text += text;
        last.dirty = true;

        bool wasNearBottom = IsNearBottom();
        InvalidatePositionsFrom(m_blocks.size() - 1);
        if (!m_bulkUpdate) {
            if (wasNearBottom) ScrollToBottom();
            InvalidateRect(m_hwnd, nullptr, FALSE);
        }
    }

    void AddAttachmentBlock(const AttachmentViewBlock& attachment) {
        if (!m_suppressScreenLog) {
            detail::LogScreenChange("attachment_block", L"attachment", attachment.manifestPath);
        }
        Block b;
        b.role = L"attachment";
        b.timeLabel = CurrentTimeLabel();
        b.attachment = attachment;
        b.dirty = true;
        m_blocks.push_back(std::move(b));
        InvalidatePositionsFrom(m_blocks.size() - 1);
        if (!m_bulkUpdate) {
            ScrollToBottom();
            InvalidateRect(m_hwnd, nullptr, FALSE);
        }
    }

    // 새 블록 시작. 이후 AppendText 는 새 블록으로.
    void NewBlock(const std::wstring& role = L"") {
        if (!m_suppressScreenLog) {
            detail::LogScreenChange("new_block", role, L"");
        }
        if (m_blocks.empty() || !m_blocks.back().text.empty() || m_blocks.back().role != role) {
            m_blocks.emplace_back();
            m_blocks.back().timeLabel = CurrentTimeLabel();
        }
        if (!m_blocks.empty()) m_blocks.back().role = role;
    }

    // 저장용 스냅샷
    size_t BlockCount() const { return m_blocks.size(); }
    const std::wstring& BlockRole(size_t i) const { return m_blocks[i].role; }
    const std::wstring& BlockText(size_t i) const { return m_blocks[i].text; }

    // 마지막 블록 텍스트 통째로 교체 (●●● placeholder → 첫 토큰 등의 흐름)
    void SetLastBlockText(const std::wstring& text) {
        if (!m_suppressScreenLog) {
            detail::LogScreenChange("set_last", L"", text);
        }
        if (m_blocks.empty()) { AppendText(text); return; }
        auto& last = m_blocks.back();
        last.text = text;
        last.dirty = true;
        bool wasNearBottom = IsNearBottom();
        InvalidatePositionsFrom(m_blocks.size() - 1);
        if (wasNearBottom) ScrollToBottom();
        InvalidateRect(m_hwnd, nullptr, FALSE);
    }

    // 임의 인덱스 블록 텍스트 in-place 교체 — retry 카드의 카운트다운 갱신 등.
    void SetBlockText(size_t idx, const std::wstring& text) {
        if (idx >= m_blocks.size()) return;
        auto& b = m_blocks[idx];
        b.text = text;
        b.dirty = true;
        InvalidatePositionsFrom(idx);
        InvalidateRect(m_hwnd, nullptr, FALSE);
    }

    void Clear() {
        for (auto& b : m_blocks) detail::SafeRelease(b.layout);
        m_blocks.clear();
        m_scrollY = 0.0f;
        if (!m_bulkUpdate) {
            UpdateScrollBar();
            InvalidateRect(m_hwnd, nullptr, FALSE);
        }
    }

    void SetScreenLogSuppressed(bool suppressed) {
        m_suppressScreenLog = suppressed;
    }

    void SetBulkUpdate(bool bulk) {
        m_bulkUpdate = bulk;
        if (!m_bulkUpdate) {
            InvalidatePositionsFrom(0);
            UpdateScrollBar();
            InvalidateRect(m_hwnd, nullptr, FALSE);
        }
    }

    float ScrollY() const { return m_scrollY; }
    float TotalHeight() const { return m_totalHeight; }
    void SetScrollY(float y) {
        m_scrollY = y;
        ClampScroll();
        UpdateScrollBar();
        InvalidateRect(m_hwnd, nullptr, FALSE);
        MaybeNotifyNearTop();
    }

    // 응답 취소 콜백 — 펄스 도트 클릭 시 호출. main.cpp 가 g_backend->Cancel() 로 연결.
    using CancelCallback = std::function<void()>;
    void SetCancelCallback(CancelCallback cb) { m_cancelCb = std::move(cb); }

    using NearTopCallback = std::function<void()>;
    void SetNearTopCallback(NearTopCallback cb) { m_nearTopCb = std::move(cb); }

    void SetUserName(const std::wstring& name) { m_userName = name; }

    // 응답 진행 상태. true 일 때 현재 assistant 블록 내부 진행 표시 + 주기 리페인트.
    void SetBusy(bool busy) {
        if (m_busy == busy) return;
        bool wasNearBottom = IsNearBottom();
        m_busy = busy;
        if (m_busy) {
            // ~120ms 마다 리페인트 — 부드러운 펄스
            SetTimer(m_hwnd, kBusyTimerId, 120, nullptr);
        } else {
            KillTimer(m_hwnd, kBusyTimerId);
        }
        for (auto& b : m_blocks) {
            if (IsAssistantRole(b.role)) b.dirty = true;
        }
        InvalidatePositionsFrom(0);
        if (wasNearBottom) ScrollToBottom();
        InvalidateRect(m_hwnd, nullptr, FALSE);
    }

    void ScrollLatestIntoView() {
        InvalidatePositionsFrom(0);
        ScrollToBottom();
        InvalidateRect(m_hwnd, nullptr, FALSE);
    }

    void ScrollToTop() {
        InvalidatePositionsFrom(0);
        m_scrollY = 0.0f;
        UpdateScrollBar();
        InvalidateRect(m_hwnd, nullptr, FALSE);
    }

private:
    // ---- 색·여백 ----
    static constexpr float kPaddingX        = 36.0f;
    static constexpr float kPaddingY        = 24.0f;
    static constexpr float kBlockGap        = 30.0f;
    static constexpr float kToolBlockGap    = 16.0f;
    static constexpr float kBlockHaloX      = 16.0f;
    static constexpr float kBlockHaloY      = 13.0f;
    static constexpr float kAssistantStripeW = 3.0f;
    static constexpr float kErrorStripeW     = 4.0f;
    static constexpr float kCardRadius       = 14.0f;
    static constexpr float kToolFontSize    = 12.0f;
    static constexpr float kAvatarSize      = 0.0f;
    static constexpr float kAvatarMargin    = 0.0f;

    // 디자인 원칙: 최고 수준 코딩 워크벤치. 깊이감 있는 중립 베이스 + 선명한 카드 분리 + 브랜드 오렌지 액센트.
    static D2D1::ColorF BgColor()            { return D2D1::ColorF(0xF2F4F7, 1.0f); }  // 본문 — 깊이감 있는 쿨-뉴트럴
    static D2D1::ColorF TextColor()          { return D2D1::ColorF(0x111827, 1.0f); }  // 본문 텍스트 — Tailwind gray-900
    static D2D1::ColorF DimTextColor()       { return D2D1::ColorF(0x6B7280, 1.0f); }  // 보조 텍스트 — Tailwind gray-500
    static D2D1::ColorF UserBgColor()        { return D2D1::ColorF(0xFEF2E7, 1.0f); }  // 사용자 카드 — 브랜드 오렌지 틴트
    static D2D1::ColorF AssistantBgColor()   { return D2D1::ColorF(0xFFFFFF, 1.0f); }  // assistant 카드 — 순수 흰색, 최대 대비
    static D2D1::ColorF AssistantAccentColor() { return D2D1::ColorF(0xF07020, 1.0f); }  // 브랜드 오렌지 — 선명, 전체 불투명도
    static D2D1::ColorF CodeBlockBgColor()   { return D2D1::ColorF(0xEBEFF5, 1.0f); }  // 코드 박스 — 배경보다 명확히 어둡게
    static D2D1::ColorF SelectionColor()     { return D2D1::ColorF(0xFF7A00, 0.28f); }
    static D2D1::ColorF SelectionFillColor() { return D2D1::ColorF(0xFF7A00, 0.12f); }
    static D2D1::ColorF LinkColor()          { return D2D1::ColorF(0x2563EB, 1.0f); }  // Tailwind blue-600
    static D2D1::ColorF TaskBgColor()        { return D2D1::ColorF(0xF0F3F7, 1.0f); }  // 태스크 카드 — 배경보다 살짝 밝게
    static D2D1::ColorF TaskBorderColor()    { return D2D1::ColorF(0xD8E1EC, 1.0f); }  // 경계선 — 부드럽지만 인식 가능
    static D2D1::ColorF UserBorderColor()    { return D2D1::ColorF(0xF0C8A0, 1.0f); }  // 유저 카드 전용 경계선 — 오렌지 틴트
    static D2D1::ColorF ErrorBgColor()       { return D2D1::ColorF(0xFFF1EE, 1.0f); }
    static D2D1::ColorF ErrorAccentColor()   { return D2D1::ColorF(0xDC2626, 1.0f); }  // Tailwind red-600

    // Syntax Highlighting — 차가운 코드 배경에 어울리는 VS Code Light 결
    static D2D1::ColorF CodeKeywordColor()    { return D2D1::ColorF(0x0000FF, 1.0f); } // Blue (keyword)
    static D2D1::ColorF CodeCommentColor()    { return D2D1::ColorF(0x6A9955, 1.0f); } // Green (comment)
    static D2D1::ColorF CodeStringColor()     { return D2D1::ColorF(0xA31515, 1.0f); } // Red (string)
    static D2D1::ColorF CodeNumberColor()     { return D2D1::ColorF(0x098658, 1.0f); } // Teal (number)
    static D2D1::ColorF CodeTypeColor()       { return D2D1::ColorF(0x267F99, 1.0f); } // Cyan (type)
    static D2D1::ColorF CodeFunctionColor()   { return D2D1::ColorF(0x795E26, 1.0f); } // Brown (function)
    static D2D1::ColorF CodePreprocColor()    { return D2D1::ColorF(0x8F3030, 1.0f); } // Dark red (#include)
    static D2D1::ColorF TableHeaderBgColor()  { return D2D1::ColorF(0xD5E0EE, 1.0f); }
    static D2D1::ColorF TableBorderColor()    { return D2D1::ColorF(0xC4D1E3, 1.0f); }
    static D2D1::ColorF TableRowAltBgColor()  { return D2D1::ColorF(0xEEF3FA, 0.90f); }
    static D2D1::ColorF QuoteBorderColor()   { return D2D1::ColorF(0x4B7CC8, 1.0f); }  // 더 선명한 파랑
    static D2D1::ColorF QuoteBgColor()       { return D2D1::ColorF(0xEAF1FF, 0.70f); }
    static D2D1::ColorF CodeHeaderBgColor()  { return D2D1::ColorF(0xC8D6E8, 1.0f); }  // 코드 헤더 — 더 선명한 슬레이트 블루
    static D2D1::ColorF HeadingSepColor()    { return D2D1::ColorF(0xC2CDD9, 1.0f); }

    static constexpr float kCodeBlockPadX   = 10.0f;
    static constexpr float kCodeBlockPadY   = 5.0f;
    static constexpr float kCodeBlockRadius = 6.0f;
    static constexpr float kSelectionStrokeW = 1.5f;

    // 응답 중 인디케이터(펄스 도트)
    static constexpr UINT_PTR kBusyTimerId       = 1;
    static constexpr UINT_PTR kAutoScrollTimerId = 2;
    static constexpr float    kBusyDotRadius     = 4.5f;
    static constexpr float    kBusyDotMargin     = 14.0f;  // 우하단 모서리 기준 안쪽 여백
    static constexpr int      kAutoScrollMargin   = 22;    // viewport 위/아래 임계
    static constexpr float    kAutoScrollSpeedMin = 8.0f;  // 한 틱 최소 픽셀
    static constexpr float kAutoScrollSpeedMax = 60.0f; // 한 틱 최대 픽셀

    static constexpr float kUserNameHeight  = 18.0f; // 사용자 이름 높이
    static constexpr float kUserNamePadding = 4.0f; // 사용자 이름 아래 여백

    // ---- 블록 ----
    struct Block : public orange::ChatBlock {
        std::wstring        timeLabel;  // HH:MM
        StyledText          styled;     // 파싱 결과 (캐시)
        AttachmentViewBlock attachment;
        IDWriteTextLayout*  layout       = nullptr;
        float               height       = 0.0f;
        float               yTop         = 0.0f;
        float               layoutWidth  = 0.0f;
        bool                dirty        = true;
    };

    std::vector<Block>     m_blocks;
    float                  m_totalHeight = 0.0f;

    static std::wstring CurrentTimeLabel() {
        SYSTEMTIME st{};
        GetLocalTime(&st);
        wchar_t buf[16];
        swprintf_s(buf, L"%02d:%02d", st.wHour, st.wMinute);
        return buf;
    }

    static bool IsAssistantRole(const std::wstring& role) {
        return role == L"assistant" ||
               role == L"assistant-claude" ||
               role == L"assistant-gemini" ||
               role == L"assistant-codex" ||
               role == L"assistant-mock";
    }

    static bool IsChatBubbleRole(const std::wstring& role) {
        return role == L"user" || role == L"task" || role == L"attachment" || IsAssistantRole(role);
    }

    static bool UsesWorkCard(const Block& b) {
        if (b.role == L"task") return true;
        if (!IsAssistantRole(b.role)) return false;
        return b.text.size() > 220 ||
               b.text.find(L'\n') != std::wstring::npos ||
               b.text.find(L"```") != std::wstring::npos;
    }

    static bool IsPendingIndicator(const Block& b) {
        return IsAssistantRole(b.role) && b.text == L"__orange_pending_indicator__";
    }

    bool IsActiveProgressBlock(const Block& b) const {
        if (!IsAssistantRole(b.role)) return false;
        if (IsPendingIndicator(b)) return true;
        if (!m_busy || m_blocks.empty()) return false;
        for (auto it = m_blocks.rbegin(); it != m_blocks.rend(); ++it) {
            if (IsAssistantRole(it->role)) return &b == &(*it);
        }
        return false;
    }

    static bool IsActionLink(const TextSpan& sp) {
        return sp.style == TextSpan::Style::Link &&
               sp.url.rfind(L"file:///", 0) == 0;
    }

    static float BubbleMaxWidth(const Block& b, float width) {
        if (b.role == L"user") return width * 0.58f;       // 우측 버블, 약간 확대
        if (b.role == L"task") return width * 0.78f;       // 태스크 카드 확대
        if (UsesWorkCard(b)) return width * 0.92f;         // 코드/긴 응답 — 문서 스타일
        if (IsAssistantRole(b.role)) return width * 0.90f; // 짧은 응답도 여유 확보
        return width;
    }

    // ---- 디바이스 자원 ----
    void EnsureDeviceResources() {
        if (m_renderTarget) return;
        if (!m_hwnd) return;

        RECT rc;
        GetClientRect(m_hwnd, &rc);
        D2D1_SIZE_U size = D2D1::SizeU(rc.right - rc.left, rc.bottom - rc.top);

        D2D1_RENDER_TARGET_PROPERTIES props =
            D2D1::RenderTargetProperties(
                D2D1_RENDER_TARGET_TYPE_DEFAULT,
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE));

        D2D1_HWND_RENDER_TARGET_PROPERTIES hwndProps =
            D2D1::HwndRenderTargetProperties(m_hwnd, size);

        if (FAILED(detail::D2DFactory()->CreateHwndRenderTarget(
            props, hwndProps, &m_renderTarget))) return;
        m_renderTarget->CreateSolidColorBrush(TextColor(),             &m_textBrush);
        m_renderTarget->CreateSolidColorBrush(DimTextColor(),          &m_dimTextBrush);
        m_renderTarget->CreateSolidColorBrush(UserBgColor(),           &m_userBgBrush);
        m_renderTarget->CreateSolidColorBrush(AssistantBgColor(),      &m_assistantBgBrush);
        m_renderTarget->CreateSolidColorBrush(AssistantAccentColor(),  &m_assistantAccentBrush);
        m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(0x1A73E8, 1.0f), &m_geminiAccentBrush);  // Google Blue
        m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(0x10A37F, 1.0f), &m_codexAccentBrush);   // OpenAI Emerald
        m_renderTarget->CreateSolidColorBrush(CodeBlockBgColor(),      &m_codeBlockBgBrush);
        m_renderTarget->CreateSolidColorBrush(SelectionColor(),        &m_selectionBrush);
        m_renderTarget->CreateSolidColorBrush(SelectionFillColor(),    &m_selectionFillBrush);
        m_renderTarget->CreateSolidColorBrush(LinkColor(),             &m_linkBrush);
        m_renderTarget->CreateSolidColorBrush(TaskBgColor(),           &m_taskBgBrush);
        m_renderTarget->CreateSolidColorBrush(TaskBorderColor(),       &m_taskBorderBrush);
        m_renderTarget->CreateSolidColorBrush(UserBorderColor(),       &m_userBorderBrush);
        m_renderTarget->CreateSolidColorBrush(ErrorBgColor(),          &m_errorBgBrush);
        m_renderTarget->CreateSolidColorBrush(ErrorAccentColor(),      &m_errorAccentBrush);
        m_renderTarget->CreateSolidColorBrush(TableHeaderBgColor(),    &m_tableHeaderBgBrush);
        m_renderTarget->CreateSolidColorBrush(TableBorderColor(),      &m_tableBorderBrush);
        m_renderTarget->CreateSolidColorBrush(TableRowAltBgColor(),    &m_tableRowAltBgBrush);
        m_renderTarget->CreateSolidColorBrush(QuoteBorderColor(),      &m_quoteBorderBrush);
        m_renderTarget->CreateSolidColorBrush(QuoteBgColor(),          &m_quoteBgBrush);
        m_renderTarget->CreateSolidColorBrush(CodeHeaderBgColor(),     &m_codeHeaderBgBrush);
        m_renderTarget->CreateSolidColorBrush(HeadingSepColor(),       &m_headingSepBrush);

        m_renderTarget->CreateSolidColorBrush(CodeKeywordColor(),      &m_codeKeywordBrush);
        m_renderTarget->CreateSolidColorBrush(CodeCommentColor(),      &m_codeCommentBrush);
        m_renderTarget->CreateSolidColorBrush(CodeStringColor(),       &m_codeStringBrush);
        m_renderTarget->CreateSolidColorBrush(CodeNumberColor(),       &m_codeNumberBrush);
        m_renderTarget->CreateSolidColorBrush(CodeTypeColor(),         &m_codeTypeBrush);
        m_renderTarget->CreateSolidColorBrush(CodeFunctionColor(),     &m_codeFunctionBrush);
        m_renderTarget->CreateSolidColorBrush(CodePreprocColor(),      &m_codePreprocBrush);
    }

    void DiscardDeviceResources() {
        for (auto& item : m_imageCache) {
            detail::SafeRelease(item.second);
        }
        m_imageCache.clear();
        detail::SafeRelease(m_fileActionIcon);
        detail::SafeRelease(m_folderActionIcon);
        detail::SafeRelease(m_fileFluentIcon);
        detail::SafeRelease(m_folderFluentIcon);

        detail::SafeRelease(m_textBrush);
        detail::SafeRelease(m_dimTextBrush);
        detail::SafeRelease(m_userBgBrush);
        detail::SafeRelease(m_assistantBgBrush);
        detail::SafeRelease(m_assistantAccentBrush);
        detail::SafeRelease(m_geminiAccentBrush);
        detail::SafeRelease(m_codexAccentBrush);
        detail::SafeRelease(m_codeBlockBgBrush);
        detail::SafeRelease(m_selectionBrush);
        detail::SafeRelease(m_selectionFillBrush);
        detail::SafeRelease(m_linkBrush);
        detail::SafeRelease(m_taskBgBrush);
        detail::SafeRelease(m_taskBorderBrush);
        detail::SafeRelease(m_errorBgBrush);
        detail::SafeRelease(m_errorAccentBrush);
        detail::SafeRelease(m_tableHeaderBgBrush);
        detail::SafeRelease(m_tableBorderBrush);
        detail::SafeRelease(m_tableRowAltBgBrush);
        detail::SafeRelease(m_userBorderBrush);
        detail::SafeRelease(m_quoteBorderBrush);
        detail::SafeRelease(m_quoteBgBrush);
        detail::SafeRelease(m_codeHeaderBgBrush);
        detail::SafeRelease(m_headingSepBrush);

        detail::SafeRelease(m_codeKeywordBrush);
        detail::SafeRelease(m_codeCommentBrush);
        detail::SafeRelease(m_codeStringBrush);
        detail::SafeRelease(m_codeNumberBrush);
        detail::SafeRelease(m_codeTypeBrush);
        detail::SafeRelease(m_codeFunctionBrush);
        detail::SafeRelease(m_codePreprocBrush);

        detail::SafeRelease(m_renderTarget);
    }

    static int HexValue(wchar_t ch) {
        if (ch >= L'0' && ch <= L'9') return ch - L'0';
        if (ch >= L'a' && ch <= L'f') return ch - L'a' + 10;
        if (ch >= L'A' && ch <= L'F') return ch - L'A' + 10;
        return -1;
    }

    static std::wstring DecodeFileUrl(const std::wstring& url) {
        std::wstring path = url;
        const std::wstring prefix = L"file:///";
        const std::wstring uncPrefix = L"file://";
        if (path.rfind(prefix, 0) == 0) {
            path = path.substr(prefix.size());
        } else if (path.rfind(uncPrefix, 0) == 0) {
            path = path.substr(uncPrefix.size());
        }

        std::wstring decoded;
        decoded.reserve(path.size());
        for (size_t i = 0; i < path.size(); ++i) {
            if (path[i] == L'%' && i + 2 < path.size()) {
                int hi = HexValue(path[i + 1]);
                int lo = HexValue(path[i + 2]);
                if (hi >= 0 && lo >= 0) {
                    decoded.push_back((wchar_t)((hi << 4) | lo));
                    i += 2;
                    continue;
                }
            }
            decoded.push_back(path[i] == L'/' ? L'\\' : path[i]);
        }
        return decoded;
    }

    ID2D1Bitmap* LoadImageBitmap(const std::wstring& url) {
        if (!m_renderTarget || url.empty()) return nullptr;
        auto it = m_imageCache.find(url);
        if (it != m_imageCache.end()) return it->second;

        std::wstring path = DecodeFileUrl(url);
        HRESULT coHr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        bool needUninit = SUCCEEDED(coHr);

        IWICImagingFactory* factory = nullptr;
        IWICBitmapDecoder* decoder = nullptr;
        IWICBitmapFrameDecode* frame = nullptr;
        IWICFormatConverter* converter = nullptr;
        ID2D1Bitmap* bitmap = nullptr;

        HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                                      CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
        if (SUCCEEDED(hr)) {
            hr = factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                                                    WICDecodeMetadataCacheOnLoad, &decoder);
        }
        if (SUCCEEDED(hr)) hr = decoder->GetFrame(0, &frame);
        if (SUCCEEDED(hr)) hr = factory->CreateFormatConverter(&converter);
        if (SUCCEEDED(hr)) {
            hr = converter->Initialize(frame, GUID_WICPixelFormat32bppPBGRA,
                                       WICBitmapDitherTypeNone, nullptr, 0.0f,
                                       WICBitmapPaletteTypeMedianCut);
        }
        if (SUCCEEDED(hr)) {
            hr = m_renderTarget->CreateBitmapFromWicBitmap(converter, nullptr, &bitmap);
        }

        detail::SafeRelease(converter);
        detail::SafeRelease(frame);
        detail::SafeRelease(decoder);
        detail::SafeRelease(factory);
        if (needUninit) CoUninitialize();

        if (SUCCEEDED(hr) && bitmap) {
            m_imageCache[url] = bitmap;
            return bitmap;
        }
        detail::SafeRelease(bitmap);
        m_imageCache[url] = nullptr;
        return nullptr;
    }

    void DrawImagePlaceholders(IDWriteTextLayout* layout,
                               const std::vector<TextSpan>& spans,
                               float originX,
                               float originY)
    {
        if (!layout || !m_renderTarget) return;
        for (const auto& sp : spans) {
            if (sp.style != TextSpan::Style::ImagePlaceholder || sp.url.empty()) continue;

            DWRITE_HIT_TEST_METRICS hit{};
            FLOAT x = 0.0f;
            FLOAT y = 0.0f;
            if (FAILED(layout->HitTestTextPosition(sp.start, FALSE, &x, &y, &hit))) continue;

            ID2D1Bitmap* bitmap = LoadImageBitmap(sp.url);
            if (!bitmap) continue;

            D2D1_SIZE_F src = bitmap->GetSize();
            if (src.width <= 0.0f || src.height <= 0.0f) continue;

            const float maxW = 180.0f;
            const float maxH = 120.0f;
            float scale = maxW / src.width;
            if (src.height * scale > maxH) scale = maxH / src.height;
            if (scale > 1.0f) scale = 1.0f;

            float w = src.width * scale;
            float h = src.height * scale;
            D2D1_RECT_F dst = D2D1::RectF(
                originX + x,
                originY + y + hit.height + 4.0f,
                originX + x + w,
                originY + y + hit.height + 4.0f + h);

            m_renderTarget->DrawBitmap(bitmap, dst, 1.0f,
                                       D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
            if (m_taskBorderBrush) {
                m_renderTarget->DrawRectangle(dst, m_taskBorderBrush, 1.0f);
            }
        }
    }

    void DrawActionIcon(const D2D1_RECT_F& rect, bool folder) {
        if (!m_renderTarget || !m_dimTextBrush) return;
        ID2D1Bitmap* icon = LoadSystemActionIcon(folder);
        if (icon) {
            const float size = 22.0f;
            float cx = (rect.left + rect.right) * 0.5f;
            float cy = (rect.top + rect.bottom) * 0.5f;
            D2D1_RECT_F dst = D2D1::RectF(cx - size * 0.5f, cy - size * 0.5f,
                                          cx + size * 0.5f, cy + size * 0.5f);
            m_renderTarget->DrawBitmap(icon, dst, 1.0f,
                                       D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
            return;
        }
        float cx = (rect.left + rect.right) * 0.5f;
        float cy = (rect.top + rect.bottom) * 0.5f;
        ID2D1SolidColorBrush* fill = nullptr;
        ID2D1SolidColorBrush* stroke = nullptr;
        ID2D1SolidColorBrush* fold = nullptr;
        m_renderTarget->CreateSolidColorBrush(
            folder ? D2D1::ColorF(0xE7B44C, 1.0f) : D2D1::ColorF(0x8FB5E8, 1.0f), &fill);
        m_renderTarget->CreateSolidColorBrush(
            folder ? D2D1::ColorF(0xB9811E, 1.0f) : D2D1::ColorF(0x4D78B8, 1.0f), &stroke);
        m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(0xFFFFFF, 0.86f), &fold);

        if (folder) {
            D2D1_ROUNDED_RECT tab = D2D1::RoundedRect(
                D2D1::RectF(cx - 7.5f, cy - 6.0f, cx - 0.5f, cy - 2.0f), 2.0f, 2.0f);
            D2D1_ROUNDED_RECT body = D2D1::RoundedRect(
                D2D1::RectF(cx - 8.0f, cy - 3.5f, cx + 8.0f, cy + 6.0f), 2.8f, 2.8f);
            m_renderTarget->FillRoundedRectangle(tab, fill);
            m_renderTarget->FillRoundedRectangle(body, fill);
            m_renderTarget->DrawRoundedRectangle(body, stroke, 1.0f);
            m_renderTarget->DrawLine(D2D1::Point2F(cx - 5.5f, cy - 0.5f),
                                     D2D1::Point2F(cx + 5.5f, cy - 0.5f),
                                     fold, 1.0f);
        } else {
            D2D1_ROUNDED_RECT page = D2D1::RoundedRect(
                D2D1::RectF(cx - 6.0f, cy - 7.0f, cx + 6.0f, cy + 7.0f), 2.2f, 2.2f);
            m_renderTarget->FillRoundedRectangle(page, fill);
            m_renderTarget->DrawRoundedRectangle(page, stroke, 1.0f);
            D2D1_POINT_2F tri[3] = {
                D2D1::Point2F(cx + 1.0f, cy - 7.0f),
                D2D1::Point2F(cx + 6.0f, cy - 2.0f),
                D2D1::Point2F(cx + 1.0f, cy - 2.0f)
            };
            ID2D1PathGeometry* geom = nullptr;
            ID2D1GeometrySink* sink = nullptr;
            if (SUCCEEDED(detail::D2DFactory()->CreatePathGeometry(&geom)) &&
                SUCCEEDED(geom->Open(&sink))) {
                sink->BeginFigure(tri[0], D2D1_FIGURE_BEGIN_FILLED);
                sink->AddLine(tri[1]);
                sink->AddLine(tri[2]);
                sink->EndFigure(D2D1_FIGURE_END_CLOSED);
                sink->Close();
                m_renderTarget->FillGeometry(geom, fold);
            }
            detail::SafeRelease(sink);
            detail::SafeRelease(geom);
            m_renderTarget->DrawLine(D2D1::Point2F(cx - 2.5f, cy + 0.5f),
                                     D2D1::Point2F(cx + 2.5f, cy + 0.5f),
                                     fold, 1.0f);
            m_renderTarget->DrawLine(D2D1::Point2F(cx - 2.5f, cy + 3.5f),
                                     D2D1::Point2F(cx + 2.0f, cy + 3.5f),
                                     fold, 1.0f);
        }
        detail::SafeRelease(fold);
        detail::SafeRelease(stroke);
        detail::SafeRelease(fill);
    }

    static void SkipSvgSeparators(const wchar_t*& p) {
        while (*p && (iswspace(*p) || *p == L',')) ++p;
    }

    static bool ReadSvgNumber(const wchar_t*& p, float& out) {
        SkipSvgSeparators(p);
        if (!*p) return false;
        wchar_t* end = nullptr;
        double value = wcstod(p, &end);
        if (end == p) return false;
        out = (float)value;
        p = end;
        return true;
    }

    ID2D1PathGeometry* CreateGeometryFromSvgPath(const wchar_t* path) {
        if (!path) return nullptr;
        ID2D1PathGeometry* geom = nullptr;
        ID2D1GeometrySink* sink = nullptr;
        if (FAILED(detail::D2DFactory()->CreatePathGeometry(&geom))) return nullptr;
        if (FAILED(geom->Open(&sink))) {
            detail::SafeRelease(geom);
            return nullptr;
        }

        const wchar_t* p = path;
        wchar_t cmd = 0;
        float x = 0.0f, y = 0.0f;
        bool figureOpen = false;

        auto closeFigure = [&]() {
            if (figureOpen) {
                sink->EndFigure(D2D1_FIGURE_END_CLOSED);
                figureOpen = false;
            }
        };

        while (*p) {
            SkipSvgSeparators(p);
            if (!*p) break;
            if (iswalpha(*p)) {
                cmd = *p++;
            }
            bool rel = (cmd >= L'a' && cmd <= L'z');
            wchar_t c = (wchar_t)towupper(cmd);

            if (c == L'M') {
                float nx = 0.0f, ny = 0.0f;
                if (!ReadSvgNumber(p, nx) || !ReadSvgNumber(p, ny)) break;
                if (rel) { nx += x; ny += y; }
                closeFigure();
                sink->BeginFigure(D2D1::Point2F(nx, ny), D2D1_FIGURE_BEGIN_FILLED);
                figureOpen = true;
                x = nx; y = ny;
                cmd = rel ? L'l' : L'L';
            } else if (c == L'L') {
                float nx = 0.0f, ny = 0.0f;
                if (!ReadSvgNumber(p, nx) || !ReadSvgNumber(p, ny)) break;
                if (rel) { nx += x; ny += y; }
                sink->AddLine(D2D1::Point2F(nx, ny));
                x = nx; y = ny;
            } else if (c == L'H') {
                float nx = 0.0f;
                if (!ReadSvgNumber(p, nx)) break;
                if (rel) nx += x;
                sink->AddLine(D2D1::Point2F(nx, y));
                x = nx;
            } else if (c == L'V') {
                float ny = 0.0f;
                if (!ReadSvgNumber(p, ny)) break;
                if (rel) ny += y;
                sink->AddLine(D2D1::Point2F(x, ny));
                y = ny;
            } else if (c == L'C') {
                float x1 = 0, y1 = 0, x2 = 0, y2 = 0, x3 = 0, y3 = 0;
                if (!ReadSvgNumber(p, x1) || !ReadSvgNumber(p, y1) ||
                    !ReadSvgNumber(p, x2) || !ReadSvgNumber(p, y2) ||
                    !ReadSvgNumber(p, x3) || !ReadSvgNumber(p, y3)) break;
                if (rel) {
                    x1 += x; y1 += y; x2 += x; y2 += y; x3 += x; y3 += y;
                }
                D2D1_BEZIER_SEGMENT seg = {
                    D2D1::Point2F(x1, y1),
                    D2D1::Point2F(x2, y2),
                    D2D1::Point2F(x3, y3)
                };
                sink->AddBezier(seg);
                x = x3; y = y3;
            } else if (c == L'Z') {
                closeFigure();
            } else {
                break;
            }
        }

        if (figureOpen) sink->EndFigure(D2D1_FIGURE_END_CLOSED);
        HRESULT hr = sink->Close();
        detail::SafeRelease(sink);
        if (FAILED(hr)) detail::SafeRelease(geom);
        return geom;
    }

    ID2D1PathGeometry* LoadFluentActionIcon(bool folder) {
        ID2D1PathGeometry*& cached = folder ? m_folderFluentIcon : m_fileFluentIcon;
        if (cached) return cached;
        static constexpr const wchar_t* kFluentDocument20Regular =
            L"M6 2C4.89543 2 4 2.89543 4 4V16C4 17.1046 4.89543 18 6 18H14C15.1046 18 16 17.1046 16 16V7.41421C16 7.01639 15.842 6.63486 15.5607 6.35355L11.6464 2.43934C11.3651 2.15804 10.9836 2 10.5858 2H6ZM5 4C5 3.44772 5.44772 3 6 3H10V6.5C10 7.32843 10.6716 8 11.5 8H15V16C15 16.5523 14.5523 17 14 17H6C5.44772 17 5 16.5523 5 16V4ZM14.7929 7H11.5C11.2239 7 11 6.77614 11 6.5V3.20711L14.7929 7Z";
        static constexpr const wchar_t* kFluentFolder20Regular =
            L"M4.5 3C3.11929 3 2 4.11929 2 5.5V14.5C2 15.8807 3.11929 17 4.5 17H15.5C16.8807 17 18 15.8807 18 14.5V7.5C18 6.11929 16.8807 5 15.5 5H9.70711L8.21967 3.51256C7.89148 3.18437 7.44636 3 6.98223 3H4.5ZM3 5.5C3 4.67157 3.67157 4 4.5 4H6.98223C7.18115 4 7.37191 4.07902 7.51256 4.21967L8.79289 5.5L7.43934 6.85355C7.34557 6.94732 7.21839 7 7.08579 7H3V5.5ZM3 8H7.08579C7.48361 8 7.86514 7.84196 8.14645 7.56066L9.70711 6H15.5C16.3284 6 17 6.67157 17 7.5V14.5C17 15.3284 16.3284 16 15.5 16H4.5C3.67157 16 3 15.3284 3 14.5V8Z";
        cached = CreateGeometryFromSvgPath(folder ? kFluentFolder20Regular : kFluentDocument20Regular);
        return cached;
    }

    ID2D1Bitmap* LoadSystemActionIcon(bool folder) {
        ID2D1Bitmap*& cached = folder ? m_folderActionIcon : m_fileActionIcon;
        if (cached || !m_renderTarget) return cached;

        HICON hIcon = nullptr;
        wchar_t dllPath[MAX_PATH] = L"";
        GetSystemDirectoryW(dllPath, MAX_PATH);
        wcscat_s(dllPath, L"\\shell32.dll");
        int iconIndex = folder ? 4 : 137;
        ExtractIconExW(dllPath, iconIndex, &hIcon, nullptr, 1);
        if (!hIcon) {
            SHFILEINFOW sfi{};
            DWORD attr = folder ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
            const wchar_t* path = folder ? L"C:\\" : L"sample.txt";
            DWORD_PTR ok = SHGetFileInfoW(path, attr, &sfi, sizeof(sfi),
                                          SHGFI_ICON | SHGFI_LARGEICON | SHGFI_USEFILEATTRIBUTES);
            if (!ok || !sfi.hIcon) return nullptr;
            hIcon = sfi.hIcon;
        }

        HRESULT coHr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        bool needUninit = SUCCEEDED(coHr);
        IWICImagingFactory* factory = nullptr;
        IWICBitmap* wicIcon = nullptr;
        IWICFormatConverter* converter = nullptr;

        HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                                      CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
        if (SUCCEEDED(hr)) hr = factory->CreateBitmapFromHICON(hIcon, &wicIcon);
        if (SUCCEEDED(hr)) hr = factory->CreateFormatConverter(&converter);
        if (SUCCEEDED(hr)) {
            hr = converter->Initialize(wicIcon, GUID_WICPixelFormat32bppPBGRA,
                                       WICBitmapDitherTypeNone, nullptr, 0.0f,
                                       WICBitmapPaletteTypeMedianCut);
        }
        if (SUCCEEDED(hr)) {
            hr = m_renderTarget->CreateBitmapFromWicBitmap(converter, nullptr, &cached);
        }

        detail::SafeRelease(converter);
        detail::SafeRelease(wicIcon);
        detail::SafeRelease(factory);
        DestroyIcon(hIcon);
        if (needUninit) CoUninitialize();
        return SUCCEEDED(hr) ? cached : nullptr;
    }

    void DrawActionLinkChips(IDWriteTextLayout* layout,
                             const std::vector<TextSpan>& spans,
                             const std::wstring& text,
                             float originX,
                             float originY)
    {
        if (!layout || !m_renderTarget || !m_taskBgBrush || !m_taskBorderBrush) return;
        for (const auto& sp : spans) {
            if (!IsActionLink(sp)) continue;
            DrawSpanBackgroundPerLine(layout, sp, originX, originY,
                                      /*padX=*/5.0f, /*padY=*/1.5f,
                                      /*radius=*/4.0f, m_taskBgBrush);

            DWRITE_HIT_TEST_METRICS metrics[4]{};
            UINT32 actual = 0;
            if (FAILED(layout->HitTestTextRange(sp.start, sp.length, originX, originY,
                                                metrics, 4, &actual))) {
                continue;
            }
            for (UINT32 i = 0; i < actual; ++i) {
                float centerX = metrics[i].left + metrics[i].width * 0.5f;
                float centerY = metrics[i].top + metrics[i].height * 0.5f;
                D2D1_RECT_F rect = D2D1::RectF(
                    centerX - 13.0f,
                    centerY - 10.5f,
                    centerX + 13.0f,
                    centerY + 10.5f);
                D2D1_ROUNDED_RECT rr = { rect, 4.0f, 4.0f };
                m_renderTarget->DrawRoundedRectangle(rr, m_taskBorderBrush, 1.0f);
                bool folder = sp.start + sp.length <= text.size() &&
                              text.substr(sp.start, sp.length) == L"folder";
                DrawActionIcon(rect, folder);
            }
        }
    }

    void EnsureTextFormat() {
        if (m_textFormat) return;
        // 1순위: Pretendard (현대적이고 깔끔한 한국어 폰트)
        // 2순위: Segoe UI (Windows 표준)
        const wchar_t* face = L"Pretendard";
        if (FAILED(detail::DWFactory()->CreateTextFormat(
                face, nullptr,
                DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                DWRITE_FONT_STRETCH_NORMAL,
                15.0f, L"ko-KR", &m_textFormat)))
        {
            detail::DWFactory()->CreateTextFormat(
                L"Segoe UI", nullptr,
                DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                DWRITE_FONT_STRETCH_NORMAL,
                15.0f, L"ko-KR", &m_textFormat);
        }
        if (m_textFormat) {
            m_textFormat->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
            // 15pt 기준 1.56× line height — 여유로운 독서 간격.
            m_textFormat->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM, 24.0f, 18.0f);
        }
    }

    float ContentWidth() const {
        RECT rc;
        GetClientRect(m_hwnd, &rc);
        float w = (float)(rc.right - rc.left) - kPaddingX * 2;
        return (w < 32.0f) ? 32.0f : w;
    }

    void DrawAvatar(const std::wstring& role, float x, float y) {
        (void)role;
        (void)x;
        (void)y;
        return;
        if (!m_renderTarget || !m_assistantAccentBrush) return;
        
        D2D1_ELLIPSE circle = D2D1::Ellipse(D2D1::Point2F(x + kAvatarSize * 0.5f, y + kAvatarSize * 0.5f), 
                                            kAvatarSize * 0.5f, kAvatarSize * 0.5f);
        
        ID2D1SolidColorBrush* bg = m_assistantAccentBrush;
        std::wstring label = L"A";
        if (role == L"user") {
            bg = m_userBgBrush;
            label = L"U";
        } else if (role == L"task" || role == L"member") {
            bg = m_taskBorderBrush;
            label = L"T";
        } else if (role == L"error") {
            bg = m_errorAccentBrush;
            label = L"E";
        }
        
        if (bg) m_renderTarget->FillEllipse(circle, bg);
        if (m_taskBorderBrush) m_renderTarget->DrawEllipse(circle, m_taskBorderBrush, 0.5f);
        
        DrawSmallText(label, x, y + 6.0f, kAvatarSize, kAvatarSize, m_textBrush, 14.0f, DWRITE_FONT_WEIGHT_BOLD);
    }

    void DrawBlockProgress(const D2D1_RECT_F& cardRect, bool pendingOnly) {
        if (!m_renderTarget || !m_assistantAccentBrush) return;

        float cx = cardRect.left + kBlockHaloX + 12.0f;
        float cy = (cardRect.top + cardRect.bottom) * 0.5f;
        DWORD tick = GetTickCount();
        D2D1::ColorF ac = AssistantAccentColor();
        for (int d = 0; d < 3; ++d) {
            float phase = ((tick + d * 200) % 900) / 900.0f;
            float wave = (phase < 0.5f) ? phase * 2.0f : (1.0f - phase) * 2.0f;
            ac.a = 0.25f + wave * 0.70f;
            m_assistantAccentBrush->SetColor(ac);
            m_renderTarget->FillEllipse(
                D2D1::Ellipse(D2D1::Point2F(cx + d * 11.0f, cy), 3.5f, 3.5f),
                m_assistantAccentBrush);
        }
        ac.a = 1.0f;
        m_assistantAccentBrush->SetColor(ac);
    }

    // 블록의 layout 을 (필요 시) 만들거나 폭 변경 시 재생성. height 갱신.
    void EnsureBlockLayout(Block& b, float width) {
        if (b.role == L"attachment") {
            detail::SafeRelease(b.layout);
            float h = 82.0f;
            for (const auto& item : b.attachment.items) {
                h += item.thumbnailUrl.empty() ? 82.0f : 124.0f;
            }
            h += 54.0f;
            b.height = h;
            b.layoutWidth = width;
            b.dirty = false;
            return;
        }

        float layoutWidth = width;
        if (IsChatBubbleRole(b.role)) {
            const float bubbleMaxW = BubbleMaxWidth(b, width);
            // 아바타 공간(kAvatarSize + kAvatarMargin) 확보
            layoutWidth = bubbleMaxW - (kBlockHaloX * 2.0f + 18.0f + kAvatarSize + kAvatarMargin);
            if (layoutWidth < 120.0f) layoutWidth = (width < 200.0f) ? width - 40.0f : 120.0f;
        }

        if (b.layout && !b.dirty && b.layoutWidth == layoutWidth) return;
        detail::SafeRelease(b.layout);

        EnsureTextFormat();
        if (!m_textFormat || b.text.empty()) {
            b.height = 0.0f;
            b.layoutWidth = layoutWidth;
            b.dirty = false;
            return;
        }

        // 마크다운 파싱 (dirty 일 때만)
        if (b.dirty) {
            b.styled = IsPendingIndicator(b)
                ? StyledText{L"...", {}, {}}
                : MarkdownParser::Parse(b.text);
        }

        const std::wstring& display = b.styled.text;
        if (display.empty()) {
            b.height = 0.0f;
            b.layoutWidth = layoutWidth;
            b.dirty = false;
            return;
        }

        detail::DWFactory()->CreateTextLayout(
            display.c_str(), (UINT32)display.size(),
            m_textFormat, layoutWidth, 1e7f, &b.layout);

        if (b.layout) {
            ApplySpans(b.layout, b.styled.spans, m_linkBrush, m_dimTextBrush);
            // tool 블록은 본문보다 작게 — 마크다운 span 적용 이후에 강제 적용
            if (b.role == L"tool") {
                DWRITE_TEXT_RANGE all = { 0, (UINT32)display.size() };
                b.layout->SetFontSize(kToolFontSize, all);
            }
            DWRITE_TEXT_METRICS m{};
            b.layout->GetMetrics(&m);
            b.height = m.height;
            if (IsActiveProgressBlock(b) && IsPendingIndicator(b)) {
                b.height += 18.0f;
            }
            // 사용자 이름 공간 확보
            if (b.role == L"user" && !b.userName.empty()) {
                b.height += kUserNameHeight + kUserNamePadding;
            }
        } else {
            b.height = 0.0f;
        }
        b.layoutWidth = layoutWidth;
        b.dirty       = false;
    }

    // 블록 i 다음에 들어갈 간격. tool 블록은 본문에 붙어 보이도록 좁게.
    float GapAfter(size_t i) const {
        if (i + 1 < m_blocks.size() && m_blocks[i + 1].role == L"tool") return kToolBlockGap;
        if (m_blocks[i].role == L"tool") return kToolBlockGap;
        if (HasTopLabel(m_blocks[i])) {
            // 레이블 블록 카드 하단 = yTop + labelPadY(22) + height + kBlockHaloY.
            // 다음 카드 상단 = yTop + height + gap - kBlockHaloY.
            // 겹침 방지: gap >= labelPadY + 2*kBlockHaloY = 22 + 26 = 48.
            return kBlockGap + 22.0f;  // 52 → 4px 여유
        }
        return kBlockGap;
    }

    // span 영역(여러 줄)을 단일 둥근 박스로 묶어 배경 그림. 코드블록 그루핑용.
    // 우측은 콘텐츠 영역 끝까지 채워 일정한 폭 — 라인별 ragged edge 회피.
    D2D1_RECT_F DrawSpanBackgroundMerged(IDWriteTextLayout* layout, const TextSpan& sp,
                                         float originX, float originY,
                                         float spanRight,
                                         ID2D1SolidColorBrush* brush)
    {
        if (!layout || !brush || sp.length == 0) return { 0, 0, 0, 0 };
        DWRITE_HIT_TEST_METRICS stack[16];
        DWRITE_HIT_TEST_METRICS* metrics = stack;
        UINT actual = 0;
        std::vector<DWRITE_HIT_TEST_METRICS> heap;
        HRESULT hr = layout->HitTestTextRange(
            sp.start, sp.length, 0, 0, stack, 16, &actual);
        if (hr == E_NOT_SUFFICIENT_BUFFER) {
            heap.resize(actual);
            metrics = heap.data();
            hr = layout->HitTestTextRange(
                sp.start, sp.length, 0, 0,
                heap.data(), (UINT)heap.size(), &actual);
        }
        if (FAILED(hr) || actual == 0) return { 0, 0, 0, 0 };

        float top    = metrics[0].top;
        float bottom = metrics[0].top + metrics[0].height;
        float left   = metrics[0].left;
        for (UINT i = 1; i < actual; ++i) {
            if (metrics[i].top < top) top = metrics[i].top;
            float b = metrics[i].top + metrics[i].height;
            if (b > bottom) bottom = b;
            if (metrics[i].left < left) left = metrics[i].left;
        }

        D2D1_RECT_F rect = D2D1::RectF(
            originX + left      - kCodeBlockPadX,
            originY + top       - kCodeBlockPadY,
            originX + spanRight + kCodeBlockPadX,
            originY + bottom    + kCodeBlockPadY
        );
        D2D1_ROUNDED_RECT rr = { rect, kCodeBlockRadius, kCodeBlockRadius };
        m_renderTarget->FillRoundedRectangle(rr, brush);
        return rect;
    }

    // span 의 줄마다 텍스트 폭에 딱 맞춰 작은 둥근 박스. 인라인 코드용.
    void DrawSpanBackgroundPerLine(IDWriteTextLayout* layout, const TextSpan& sp,
                                   float originX, float originY,
                                   float padX, float padY, float radius,
                                   ID2D1SolidColorBrush* brush)
    {
        if (!layout || !brush || sp.length == 0) return;
        DWRITE_HIT_TEST_METRICS stack[16];
        DWRITE_HIT_TEST_METRICS* metrics = stack;
        UINT actual = 0;
        std::vector<DWRITE_HIT_TEST_METRICS> heap;
        HRESULT hr = layout->HitTestTextRange(
            sp.start, sp.length, originX, originY, stack, 16, &actual);
        if (hr == E_NOT_SUFFICIENT_BUFFER) {
            heap.resize(actual);
            metrics = heap.data();
            hr = layout->HitTestTextRange(
                sp.start, sp.length, originX, originY,
                heap.data(), (UINT)heap.size(), &actual);
        }
        if (FAILED(hr)) return;
        for (UINT i = 0; i < actual; ++i) {
            const auto& m = metrics[i];
            // 폭 0(빈 wrap-only 줄) 은 그리지 않음
            if (m.width <= 0.5f) continue;
            D2D1_ROUNDED_RECT rr = {
                D2D1::RectF(
                    m.left              - padX,
                    m.top               - padY,
                    m.left + m.width    + padX,
                    m.top  + m.height   + padY),
                radius, radius
            };
            m_renderTarget->FillRoundedRectangle(rr, brush);
        }
    }

    void DrawCopyButton(const D2D1_RECT_F& codeRect, int blockIdx, size_t spanIdx) {
        bool isCopied = (m_copiedBlockIdx == blockIdx && m_copiedSpanIdx == spanIdx);
        
        // 버튼 영역 (우측 상단)
        D2D1_RECT_F btnRect = D2D1::RectF(
            codeRect.right - 60.0f,
            codeRect.top + 4.0f,
            codeRect.right - 4.0f,
            codeRect.top + 26.0f
        );

        // 배경 (살짝 어두운 투명 베이지)
        ID2D1SolidColorBrush* bgB = nullptr;
        m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(0x000000, 0.05f), &bgB);
        D2D1_ROUNDED_RECT rr = { btnRect, 4.0f, 4.0f };
        m_renderTarget->FillRoundedRectangle(rr, bgB);
        detail::SafeRelease(bgB);

        // 텍스트 ("Copy" or "Copied!")
        const wchar_t* label = isCopied ? L"Copied!" : L"Copy";
        IDWriteTextLayout* tl = nullptr;
        detail::DWFactory()->CreateTextLayout(
            label, (UINT32)wcslen(label), m_textFormat,
            btnRect.right - btnRect.left, btnRect.bottom - btnRect.top, &tl);
        if (tl) {
            tl->SetFontSize(11.0f, { 0, (UINT32)wcslen(label) });
            tl->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            tl->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            m_renderTarget->DrawTextLayout({ btnRect.left, btnRect.top }, tl, m_dimTextBrush);
            detail::SafeRelease(tl);
        }
    }

    // StyledText 의 span 을 IDWriteTextLayout 에 DWrite 포맷팅으로 적용.
    // linkBrush: 링크 색용. nullptr 이면 색 적용 스킵.
    void ApplySpans(IDWriteTextLayout* layout,
                    const std::vector<TextSpan>& spans,
                    ID2D1SolidColorBrush* linkBrush,
                    ID2D1SolidColorBrush* dimBrush)
    {
        for (const auto& sp : spans) {
            DWRITE_TEXT_RANGE r = { sp.start, sp.length };
            switch (sp.style) {
            case TextSpan::Style::Bold:
                layout->SetFontWeight(DWRITE_FONT_WEIGHT_BOLD, r);
                break;
            case TextSpan::Style::Italic:
                layout->SetFontStyle(DWRITE_FONT_STYLE_ITALIC, r);
                break;
            case TextSpan::Style::InlineCode:
            case TextSpan::Style::CodeBlock:
                layout->SetFontFamilyName(L"Consolas", r);
                break;
            case TextSpan::Style::CodeKeyword:
                if (m_codeKeywordBrush) layout->SetDrawingEffect(m_codeKeywordBrush, r);
                break;
            case TextSpan::Style::CodeComment:
                if (m_codeCommentBrush) layout->SetDrawingEffect(m_codeCommentBrush, r);
                break;
            case TextSpan::Style::CodeString:
                if (m_codeStringBrush) layout->SetDrawingEffect(m_codeStringBrush, r);
                break;
            case TextSpan::Style::CodeNumber:
                if (m_codeNumberBrush) layout->SetDrawingEffect(m_codeNumberBrush, r);
                break;
            case TextSpan::Style::CodeType:
                if (m_codeTypeBrush) layout->SetDrawingEffect(m_codeTypeBrush, r);
                break;
            case TextSpan::Style::CodeFunction:
                if (m_codeFunctionBrush) layout->SetDrawingEffect(m_codeFunctionBrush, r);
                break;
            case TextSpan::Style::CodePreprocessor:
                if (m_codePreprocBrush) layout->SetDrawingEffect(m_codePreprocBrush, r);
                break;
            case TextSpan::Style::Link:
                if (IsActionLink(sp)) {
                    layout->SetFontSize(12.0f, r);
                    layout->SetFontWeight(DWRITE_FONT_WEIGHT_SEMI_BOLD, r);
                    if (dimBrush) layout->SetDrawingEffect(dimBrush, r);
                } else {
                    layout->SetUnderline(TRUE, r);
                    if (linkBrush) layout->SetDrawingEffect(linkBrush, r);
                }
                break;
            case TextSpan::Style::Strikethrough:
                layout->SetStrikethrough(TRUE, r);
                break;
            case TextSpan::Style::CodeBlockLang:
                // ` ```cpp ` 의 cpp 라벨 — 코드블록 위 한 줄, dim 색 + 작은 폰트.
                layout->SetFontSize(11.0f, r);
                layout->SetFontFamilyName(L"Consolas", r);
                if (dimBrush) layout->SetDrawingEffect(dimBrush, r);
                break;
            case TextSpan::Style::ImagePlaceholder:
                // `[image: alt]` placeholder — 본문에 섞이는 dim 한 글자 묶음. 폰트 크기는
                // 본문 결 유지(이미지가 본문 흐름의 일부라 크기 변동 X), 색만 dim 으로 후퇴.
                if (dimBrush) layout->SetDrawingEffect(dimBrush, r);
                break;
            case TextSpan::Style::HorizontalRule:
                // ─ 라인 문자에 작은 폰트 + dim 색 (m_dimTextBrush) — 본문에서 *분리선* 으로
                // 자연스럽게 시각 후퇴. 본문(#2E2A26) 보다 옅은 회갈색(#8A8278).
                layout->SetFontSize(9.0f, r);
                if (dimBrush) layout->SetDrawingEffect(dimBrush, r);
                break;
            case TextSpan::Style::Heading1:
                layout->SetFontSize(24.0f, r);
                layout->SetFontWeight(DWRITE_FONT_WEIGHT_BOLD, r);
                break;
            case TextSpan::Style::Heading2:
                layout->SetFontSize(20.0f, r);
                layout->SetFontWeight(DWRITE_FONT_WEIGHT_BOLD, r);
                break;
            case TextSpan::Style::Heading3:
                layout->SetFontSize(17.0f, r);
                layout->SetFontWeight(DWRITE_FONT_WEIGHT_BOLD, r);
                break;
            case TextSpan::Style::Heading4:
                // H1(24) → H2(20) → H3(17) → H4(15) → H5(본문 14) → H6(본문 작게).
                // H5 는 본문과 같은 크기로 굵기만 — 본문 흐름 유지하며 강조. H6 는 본문보다 작게
                // dim — 보조 라벨 결.
                layout->SetFontSize(15.0f, r);
                layout->SetFontWeight(DWRITE_FONT_WEIGHT_BOLD, r);
                break;
            case TextSpan::Style::Heading5:
                // 본문 크기 그대로(14pt) + 굵기 — 인라인 강조 결의 헤더.
                layout->SetFontWeight(DWRITE_FONT_WEIGHT_BOLD, r);
                break;
            case TextSpan::Style::Heading6:
                // 본문보다 작게 + 굵기 + dim — 가장 약한 헤더 결, 보조 라벨 같은 자리.
                layout->SetFontSize(12.0f, r);
                layout->SetFontWeight(DWRITE_FONT_WEIGHT_BOLD, r);
                if (dimBrush) layout->SetDrawingEffect(dimBrush, r);
                break;
            case TextSpan::Style::TableCell:
            case TextSpan::Style::TableHeader:
                layout->SetFontSize(13.0f, r);
                if (sp.style == TextSpan::Style::TableHeader) {
                    layout->SetFontWeight(DWRITE_FONT_WEIGHT_SEMI_BOLD, r);
                }
                break;
            case TextSpan::Style::Quote:
                layout->SetFontStyle(DWRITE_FONT_STYLE_ITALIC, r);
                if (dimBrush) layout->SetDrawingEffect(dimBrush, r);
                break;
            }
        }
    }

    // 인덱스 from 부터 Y 위치 / 총 높이 다시 계산. layout dirty 도 처리.
    void InvalidatePositionsFrom(size_t from) {
        float width = ContentWidth();
        float y = 0.0f;
        if (from > 0) {
            y = m_blocks[from - 1].yTop + m_blocks[from - 1].height + GapAfter(from - 1);
        }
        for (size_t i = from; i < m_blocks.size(); ++i) {
            EnsureBlockLayout(m_blocks[i], width);
            m_blocks[i].yTop = y;
            y += m_blocks[i].height + GapAfter(i);
        }
        m_totalHeight = (m_blocks.empty())
            ? 0.0f
            : (y - GapAfter(m_blocks.size() - 1));
        UpdateScrollBar();
    }

    // 폭 변경 시 모든 블록 dirty + 위치 재계산
    void OnWidthChanged() {
        for (auto& b : m_blocks) b.dirty = true;
        InvalidatePositionsFrom(0);
    }

    // ---- 스크롤 ----
    bool IsNearBottom() const {
        if (!m_hwnd) return true;
        RECT rc;
        GetClientRect(m_hwnd, &rc);
        float viewH = (float)(rc.bottom - rc.top) - kPaddingY * 2;
        float maxScroll = m_totalHeight - viewH;
        if (maxScroll <= 0.0f) return true;
        return (maxScroll - m_scrollY) < 32.0f;
    }

    void ScrollToBottom() {
        RECT rc;
        GetClientRect(m_hwnd, &rc);
        float viewH = (float)(rc.bottom - rc.top) - kPaddingY * 2;
        float maxScroll = m_totalHeight - viewH;
        m_scrollY = (maxScroll > 0.0f) ? maxScroll : 0.0f;
        UpdateScrollBar();
    }

    void ClampScroll() {
        RECT rc;
        GetClientRect(m_hwnd, &rc);
        float viewH = (float)(rc.bottom - rc.top) - kPaddingY * 2;
        float maxScroll = m_totalHeight - viewH;
        if (maxScroll < 0.0f) maxScroll = 0.0f;
        if (m_scrollY < 0.0f) m_scrollY = 0.0f;
        if (m_scrollY > maxScroll) m_scrollY = maxScroll;
    }

    void UpdateScrollBar() {
        if (!m_hwnd) return;
        RECT rc;
        GetClientRect(m_hwnd, &rc);
        int viewH = rc.bottom - rc.top;
        int contentH = (int)(m_totalHeight + kPaddingY * 2);

        SCROLLINFO si{};
        si.cbSize = sizeof(si);
        si.fMask  = SIF_RANGE | SIF_PAGE | SIF_POS | SIF_DISABLENOSCROLL;
        si.nMin   = 0;
        si.nMax   = (contentH > 0) ? contentH - 1 : 0;
        si.nPage  = (UINT)viewH;
        si.nPos   = (int)m_scrollY;
        SetScrollInfo(m_hwnd, SB_VERT, &si, TRUE);
    }

    // ---- WndProc ----
    static LRESULT CALLBACK WndProcStatic(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
        OrangeView* self = nullptr;
        if (msg == WM_NCCREATE) {
            auto* cs = reinterpret_cast<CREATESTRUCT*>(lp);
            self = static_cast<OrangeView*>(cs->lpCreateParams);
            SetWindowLongPtrW(hwnd, 0, (LONG_PTR)self);
            self->m_hwnd = hwnd;
        } else {
            self = reinterpret_cast<OrangeView*>(GetWindowLongPtrW(hwnd, 0));
        }
        if (self) return self->WndProc(hwnd, msg, wp, lp);
        return DefWindowProcW(hwnd, msg, wp, lp);
    }

    LRESULT WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
        switch (msg) {
        case WM_PAINT:        OnPaint();                                     return 0;
        case WM_SIZE:         OnSize();                                      return 0;
        case WM_MOUSEWHEEL:   OnMouseWheel(GET_WHEEL_DELTA_WPARAM(wp));      return 0;
        case WM_VSCROLL:      OnVScroll(LOWORD(wp));                         return 0;
        case WM_ERASEBKGND:   return 1;
        case WM_TIMER:
            if (wp == kBusyTimerId && m_busy) {
                InvalidateRect(m_hwnd, nullptr, FALSE);
            }
            else if (wp == kAutoScrollTimerId && m_dragging && m_autoScrollDir != 0) {
                m_scrollY += (float)m_autoScrollDir * m_autoScrollSpeed;
                ClampScroll();
                UpdateScrollBar();
                // 현재 마우스 위치(viewport 좌표)로 끝점 갱신 — 마우스가 가만히 있어도 스크롤로 새 텍스트 위에 위치
                POINT pt;
                if (GetCursorPos(&pt) && ScreenToClient(m_hwnd, &pt)) {
                    HitInfo h = HitTextAtClient(pt.x, pt.y);
                    if (h.blockIdx >= 0) m_selEnd = { h.blockIdx, h.textIdx };
                }
                InvalidateRect(m_hwnd, nullptr, FALSE);
            }
            else if (wp == kCopyFeedbackTimerId) {
                m_copiedBlockIdx = -1;
                m_copiedSpanIdx = (size_t)-1;
                KillTimer(m_hwnd, kCopyFeedbackTimerId);
                InvalidateRect(m_hwnd, nullptr, FALSE);
            }
            return 0;
        case WM_CONTEXTMENU:  OnContextMenu(GET_X_LPARAM(lp), GET_Y_LPARAM(lp)); return 0;
        case WM_SETCURSOR:
            // 링크 / 펄스 도트(취소 가능) 위면 손 모양, 그 외엔 기본(IDC_IBEAM)
            if (LOWORD(lp) == HTCLIENT) {
                POINT pt;
                if (GetCursorPos(&pt) && ScreenToClient(m_hwnd, &pt)) {
                    // 1) 마크다운 링크
                    if (IsAttachmentActionAt(pt.x, pt.y)) {
                        SetCursor(LoadCursor(nullptr, IDC_HAND));
                        return TRUE;
                    }
                    HitInfo h = HitTextAtClient(pt.x, pt.y);
                    if (h.blockIdx >= 0 && IsLinkAt(h.blockIdx, h.textIdx)) {
                        SetCursor(LoadCursor(nullptr, IDC_HAND));
                        return TRUE;
                    }
                }
            }
            break;
        case WM_COMMAND:
            if (LOWORD(wp) == kCmdCopyAll)         CopyAllToClipboard();
            else if (LOWORD(wp) == kCmdCopyBlock)  CopySelection();
            return 0;
        case WM_LBUTTONDOWN: {
            SetFocus(m_hwnd);
            int cx = GET_X_LPARAM(lp);
            int cy = GET_Y_LPARAM(lp);
            if (OpenAttachmentActionAt(cx, cy)) return 0;

            // 1) 복사 버튼 클릭 검사
            int bIdx = HitBlockAtClientY(cy);
            if (bIdx >= 0) {
                const auto& b = m_blocks[(size_t)bIdx];
                const float textY = kPaddingY + b.yTop - m_scrollY + TextTopPadForHit(b);
                RECT rcCl2; GetClientRect(m_hwnd, &rcCl2);
                BlockTextOrigin org = ComputeBlockTextOrigin(b, (float)(rcCl2.right - rcCl2.left));
                const float originX = org.valid ? org.x : kPaddingX;
                for (size_t i = 0; i < b.styled.spans.size(); ++i) {
                    const auto& sp = b.styled.spans[i];
                    if (sp.style == TextSpan::Style::CodeBlock) {
                        // OnPaint DrawSpanBackgroundMerged 와 동일한 원점/폭으로 Rect 계산
                        DWRITE_HIT_TEST_METRICS stack[16];
                        UINT actual = 0;
                        if (SUCCEEDED(b.layout->HitTestTextRange(sp.start, sp.length, 0, 0, stack, 16, &actual)) && actual > 0) {
                            float top = stack[0].top, bottom = stack[0].top + stack[0].height;
                            for (UINT j = 1; j < actual; ++j) {
                                if (stack[j].top < top) top = stack[j].top;
                                if (stack[j].top + stack[j].height > bottom) bottom = stack[j].top + stack[j].height;
                            }
                            D2D1_RECT_F codeRect = D2D1::RectF(
                                originX - kCodeBlockPadX,
                                textY + top - kCodeBlockPadY,
                                originX + b.layoutWidth + kCodeBlockPadX,
                                textY + bottom + kCodeBlockPadY);
                            
                            D2D1_RECT_F btnRect = D2D1::RectF(
                                codeRect.right - 60.0f, codeRect.top + 4.0f,
                                codeRect.right - 4.0f, codeRect.top + 26.0f);

                            if (cx >= btnRect.left && cx <= btnRect.right &&
                                cy >= btnRect.top && cy <= btnRect.bottom) {
                                std::wstring code = b.styled.text.substr(sp.start, sp.length);
                                if (CopyTextToClipboard(code)) {
                                    m_copiedBlockIdx = bIdx;
                                    m_copiedSpanIdx = i;
                                    SetTimer(m_hwnd, kCopyFeedbackTimerId, 2000, nullptr);
                                    InvalidateRect(m_hwnd, nullptr, FALSE);
                                }
                                return 0;
                            }
                        }
                    }
                }
            }

            HitInfo h = HitTextAtClient(cx, cy);

            // 트리플클릭: 직전 더블클릭과 같은 블록 + DoubleClickTime 안 + 픽셀 거리 임계 안.
            // 거리 검증 없으면 더블클릭 후 다른 위치 클릭도 같은 블록이면 트리플로 잡힘.
            DWORD now = GetTickCount();
            int dxLimit = GetSystemMetrics(SM_CXDOUBLECLK);  // 보통 4
            int dyLimit = GetSystemMetrics(SM_CYDOUBLECLK);
            int dx = cx - m_lastDblClickX;
            int dy = cy - m_lastDblClickY;
            if (dx < 0) dx = -dx;
            if (dy < 0) dy = -dy;
            bool isTriple = (h.blockIdx >= 0 &&
                             h.blockIdx == m_lastDblClickBlock &&
                             (now - m_lastDblClickTime) <= GetDoubleClickTime() &&
                             dx <= dxLimit && dy <= dyLimit);
            if (isTriple) {
                SelectLineAt(h.blockIdx, h.textIdx);
                m_lastDblClickTime  = 0;
                m_lastDblClickBlock = -1;
                return 0;
            }

            // Shift+클릭: 기존 selStart 유지, selEnd 만 새 위치로 — 표준 텍스트 선택 확장.
            bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
            if (shift && h.blockIdx >= 0 && m_selStart.block >= 0) {
                m_selEnd   = { h.blockIdx, h.textIdx };
                m_dragging = true;
                SetCapture(m_hwnd);
                InvalidateRect(m_hwnd, nullptr, FALSE);
                return 0;
            }

            ClearSelection();
            if (h.blockIdx >= 0) {
                m_dragging = true;
                m_selStart = { h.blockIdx, h.textIdx };
                m_selEnd   = { h.blockIdx, h.textIdx };
                SetCapture(m_hwnd);
            }
            InvalidateRect(m_hwnd, nullptr, FALSE);
            return 0;
        }
        case WM_MOUSEMOVE: {
            int cx = GET_X_LPARAM(lp);
            int cy = GET_Y_LPARAM(lp);
            m_mousePos = D2D1::Point2F((float)cx, (float)cy);

            if (!m_dragging) {
                // 호버 상태에 따라 버튼 그리기를 위해 리페인트 (비효율적일 수 있으나 일단 구현)
                InvalidateRect(m_hwnd, nullptr, FALSE);
                return 0;
            }

            // 자동 스크롤 — viewport 위/아래 임계 안에서 마우스가 머물면 SetTimer 로 스크롤.
            // 속도는 임계 진입 깊이에 비례 — 더 깊이 들어갈수록 빠름.
            RECT rc; GetClientRect(m_hwnd, &rc);
            int newDir = 0;
            int depth = 0;
            if (cy < kAutoScrollMargin) {
                newDir = -1;
                depth  = kAutoScrollMargin - cy;
            } else if (cy > rc.bottom - kAutoScrollMargin) {
                newDir = 1;
                depth  = cy - (rc.bottom - kAutoScrollMargin);
            }
            if (newDir != 0) {
                float sp = (float)depth;
                if (sp < kAutoScrollSpeedMin) sp = kAutoScrollSpeedMin;
                if (sp > kAutoScrollSpeedMax) sp = kAutoScrollSpeedMax;
                m_autoScrollSpeed = sp;
            }
            if (newDir != m_autoScrollDir) {
                m_autoScrollDir = newDir;
                if (newDir != 0) SetTimer(m_hwnd, kAutoScrollTimerId, 50, nullptr);
                else             KillTimer(m_hwnd, kAutoScrollTimerId);
            }

            HitInfo h = HitTextAtClient(cx, cy);
            if (h.blockIdx >= 0) {
                SelectionEnd ne{ h.blockIdx, h.textIdx };
                if (ne.block != m_selEnd.block || ne.idx != m_selEnd.idx) {
                    m_selEnd = ne;
                    InvalidateRect(m_hwnd, nullptr, FALSE);
                }
            }
            return 0;
        }
        case WM_LBUTTONUP: {
            bool wasDragging = m_dragging;
            if (m_dragging) {
                m_dragging = false;
                ReleaseCapture();
            }
            if (m_autoScrollDir != 0) {
                m_autoScrollDir = 0;
                KillTimer(m_hwnd, kAutoScrollTimerId);
            }
            // 단순 클릭(드래그 거리 ≈ 0) 이고 링크 자리면 ShellExecute open
            if (wasDragging && m_selStart.block >= 0 &&
                m_selStart.block == m_selEnd.block &&
                m_selStart.idx   == m_selEnd.idx)
            {
                if (HandleLinkClick(m_selStart.block, m_selStart.idx)) {
                    ClearSelection();
                    InvalidateRect(m_hwnd, nullptr, FALSE);
                }
            }
            return 0;
        }
        case WM_LBUTTONDBLCLK: {
            SetFocus(m_hwnd);
            int cx = GET_X_LPARAM(lp);
            int cy = GET_Y_LPARAM(lp);
            HitInfo h = HitTextAtClient(cx, cy);
            if (h.blockIdx >= 0) {
                SelectWordAt(h.blockIdx, h.textIdx);
                m_lastDblClickTime  = GetTickCount();
                m_lastDblClickBlock = h.blockIdx;
                m_lastDblClickX     = cx;
                m_lastDblClickY     = cy;
            }
            return 0;
        }
        case WM_KEYDOWN:
            if (wp == 'C' && (GetKeyState(VK_CONTROL) & 0x8000)) {
                CopySelection();
                return 0;
            }
            if (wp == VK_ESCAPE && HasSelection()) {
                ClearSelection();
                InvalidateRect(m_hwnd, nullptr, FALSE);
                return 0;
            }
            // 키보드 스크롤 — 마우스 없이 대화 탐색
            switch (wp) {
            case VK_PRIOR: OnVScroll(SB_PAGEUP);   return 0;  // PageUp
            case VK_NEXT:  OnVScroll(SB_PAGEDOWN); return 0;  // PageDown
            case VK_HOME:  OnVScroll(SB_TOP);      return 0;
            case VK_END:   OnVScroll(SB_BOTTOM);   return 0;
            case VK_UP:    OnVScroll(SB_LINEUP);   return 0;
            case VK_DOWN:  OnVScroll(SB_LINEDOWN); return 0;
            }
            break;
        case WM_DESTROY:      Cleanup();                                     return 0;
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }

    // ---- 우클릭 메뉴 ----
    static constexpr UINT kCmdCopyAll   = 100;
    static constexpr UINT kCmdCopyBlock = 101;

    void OnContextMenu(int screenX, int screenY) {
        // WM_CONTEXTMENU 의 LPARAM 이 -1 이면 키보드 트리거 — 윈도우 가운데 폴백
        if (screenX == -1 && screenY == -1) {
            RECT rc; GetWindowRect(m_hwnd, &rc);
            screenX = (rc.left + rc.right)  / 2;
            screenY = (rc.top  + rc.bottom) / 2;
        }
        HMENU menu = CreatePopupMenu();
        if (!menu) return;
        UINT selFlags = HasSelection() ? MF_STRING : (MF_STRING | MF_GRAYED);
        AppendMenuW(menu, selFlags,    kCmdCopyBlock, L"선택 복사\tCtrl+C");
        AppendMenuW(menu, MF_STRING,   kCmdCopyAll,   L"전체 대화 복사");
        TrackPopupMenu(menu,
                       TPM_LEFTALIGN | TPM_TOPALIGN | TPM_RIGHTBUTTON,
                       screenX, screenY, 0, m_hwnd, nullptr);
        DestroyMenu(menu);
    }

    // 클라이언트 Y 좌표 → 그 자리에 있는 블록 인덱스(없으면 -1).
    int HitBlockAtClientY(int clientY) const {
        float docY = (float)clientY + m_scrollY - kPaddingY;
        for (size_t i = 0; i < m_blocks.size(); ++i) {
            const auto& b = m_blocks[i];
            float visualBottom = b.yTop + b.height + TextTopPadForHit(b) + kBlockHaloY;
            if (docY >= b.yTop && docY < visualBottom) {
                return (int)i;
            }
        }
        return -1;
    }

    struct HitInfo { int blockIdx; UINT textIdx; };

    // 선택 끝점 — 블록 인덱스 + 그 블록 안의 텍스트 인덱스. block == -1 이면 미정.
    struct SelectionEnd { int block; UINT idx; };

    static bool LessSel(const SelectionEnd& a, const SelectionEnd& b) {
        if (a.block != b.block) return a.block < b.block;
        return a.idx < b.idx;
    }

    struct BlockTextOrigin {
        float x = kPaddingX;
        float y = kPaddingY;
        bool valid = false;
    };

    bool HasBackendLabel(const Block& b) const {
        return b.role == L"assistant-claude" ||
               b.role == L"assistant-gemini" ||
               b.role == L"assistant-codex" ||
               b.role == L"assistant-mock";
    }

    bool HasTopLabel(const Block& b) const {
        return HasBackendLabel(b) || b.role == L"user";
    }

    float TextTopPadForHit(const Block& b) const {
        return HasTopLabel(b) ? 22.0f : 0.0f;
    }

    BlockTextOrigin ComputeBlockTextOrigin(const Block& b, float clientRight) const {
        if (!b.layout) return {};

        DWRITE_TEXT_METRICS textMetrics{};
        b.layout->GetMetrics(&textMetrics);

        const bool isUserBubble = (b.role == L"user");
        const bool usesWorkCard = UsesWorkCard(b);
        const float width = ContentWidth();
        const float bubbleMaxW = BubbleMaxWidth(b, width);
        float bubbleW = textMetrics.widthIncludingTrailingWhitespace + kBlockHaloX * 2.0f + 18.0f;
        if (bubbleW < 132.0f) bubbleW = 132.0f;
        if (IsActiveProgressBlock(b) && bubbleW < 220.0f) bubbleW = 220.0f;
        if (usesWorkCard && bubbleW < 360.0f) bubbleW = 360.0f;
        for (const auto& sp : b.styled.spans) {
            if (sp.style == TextSpan::Style::CodeBlock) {
                bubbleW = bubbleMaxW;
                break;
            }
        }
        if (bubbleW > bubbleMaxW) bubbleW = bubbleMaxW;

        float cardLeft = isUserBubble
            ? (clientRight - kPaddingX - kAvatarSize - kAvatarMargin - bubbleW)
            : (kPaddingX + kAvatarSize + kAvatarMargin - kBlockHaloX);
        if (cardLeft < kPaddingX + kAvatarSize + kAvatarMargin - kBlockHaloX) 
            cardLeft = kPaddingX + kAvatarSize + kAvatarMargin - kBlockHaloX;

        const float labelPadY = TextTopPadForHit(b);
        float textOriginY = kPaddingY + b.yTop - m_scrollY + labelPadY;
        if (isUserBubble && !b.userName.empty()) {
            textOriginY += kUserNameHeight + kUserNamePadding;
        }
        return {
            cardLeft + kBlockHaloX,
            textOriginY,
            true
        };
    }

    // 클라이언트 좌표 → 블록 인덱스 + 그 블록 layout 안의 텍스트 인덱스.
    HitInfo HitTextAtClient(int clientX, int clientY) const {
        HitInfo h{-1, 0};
        int bIdx = HitBlockAtClientY(clientY);
        if (bIdx < 0) return h;
        const auto& b = m_blocks[(size_t)bIdx];
        if (!b.layout) return h;
        RECT rc{};
        GetClientRect(m_hwnd, &rc);
        BlockTextOrigin origin = ComputeBlockTextOrigin(b, (float)(rc.right - rc.left));
        if (!origin.valid) return h;
        float layoutX = (float)clientX - origin.x;
        float layoutY = (float)clientY - origin.y;
        DWRITE_TEXT_METRICS textMetrics{};
        b.layout->GetMetrics(&textMetrics);
        if (layoutY < 0.0f) layoutY = 0.0f;
        if (layoutY > textMetrics.height) layoutY = textMetrics.height;
        BOOL isTrailing = FALSE, isInside = FALSE;
        DWRITE_HIT_TEST_METRICS m{};
        if (FAILED(b.layout->HitTestPoint(layoutX, layoutY,
                                          &isTrailing, &isInside, &m))) {
            return h;
        }
        h.blockIdx = bIdx;
        h.textIdx  = m.textPosition + (isTrailing ? m.length : 0);
        const auto& src = b.styled.text.empty() ? b.text : b.styled.text;
        if (layoutY >= textMetrics.height - 0.5f) h.textIdx = (UINT)src.size();
        else if (h.textIdx > src.size()) h.textIdx = (UINT)src.size();
        return h;
    }

    bool HasSelection() const {
        if (m_selStart.block < 0 || m_selEnd.block < 0) return false;
        return !(m_selStart.block == m_selEnd.block &&
                 m_selStart.idx   == m_selEnd.idx);
    }
    SelectionEnd SelLo() const {
        return LessSel(m_selStart, m_selEnd) ? m_selStart : m_selEnd;
    }
    SelectionEnd SelHi() const {
        return LessSel(m_selStart, m_selEnd) ? m_selEnd : m_selStart;
    }

    void ClearSelection() {
        m_dragging = false;
        m_selStart = {-1, 0};
        m_selEnd   = {-1, 0};
    }

    enum class CharClass { Other, AsciiWord, Hangul, CJK };

    static CharClass ClassifyChar(wchar_t c) {
        // ASCII 알파뉴메릭 + underscore
        if ((c >= L'a' && c <= L'z') ||
            (c >= L'A' && c <= L'Z') ||
            (c >= L'0' && c <= L'9') ||
            c == L'_') return CharClass::AsciiWord;
        // 한글 음절(Hangul Syllables) + 호환 자모
        if (c >= 0xAC00 && c <= 0xD7A3) return CharClass::Hangul;
        if (c >= 0x3130 && c <= 0x318F) return CharClass::Hangul;
        // CJK 통합 한자 + 히라가나 + 가타카나
        if ((c >= 0x4E00 && c <= 0x9FFF) ||
            (c >= 0x3040 && c <= 0x309F) ||
            (c >= 0x30A0 && c <= 0x30FF)) return CharClass::CJK;
        return CharClass::Other;
    }

    // 위치가 클릭 가능한(URL 비어있지 않은) Link span 자리인지.
    bool IsLinkAt(int blockIdx, UINT textIdx) const {
        if (blockIdx < 0 || (size_t)blockIdx >= m_blocks.size()) return false;
        const auto& b = m_blocks[(size_t)blockIdx];
        for (const auto& sp : b.styled.spans) {
            if (sp.style == TextSpan::Style::Link &&
                textIdx >= sp.start && textIdx < sp.start + sp.length &&
                !sp.url.empty()) {
                return true;
            }
        }
        return false;
    }

    // 단순 클릭이 link span 자리면 URL 을 ShellExecute 로 연다. true 반환 시 처리됨.
    bool HandleLinkClick(int blockIdx, UINT textIdx) {
        if (blockIdx < 0 || (size_t)blockIdx >= m_blocks.size()) return false;
        const auto& b = m_blocks[(size_t)blockIdx];
        for (const auto& sp : b.styled.spans) {
            if (sp.style != TextSpan::Style::Link) continue;
            if (textIdx >= sp.start && textIdx < sp.start + sp.length) {
                if (!sp.url.empty()) {
                    ShellExecuteW(nullptr, L"open", sp.url.c_str(),
                                  nullptr, nullptr, SW_SHOWNORMAL);
                }
                return true;
            }
        }
        return false;
    }

    // 트리플클릭 — 클릭한 줄(시각 줄, wrap 단위) 의 시작~끝(개행 제외) 선택.
    void SelectLineAt(int blockIdx, UINT textIdx) {
        if (blockIdx < 0 || (size_t)blockIdx >= m_blocks.size()) return;
        const auto& b = m_blocks[(size_t)blockIdx];
        if (!b.layout) return;

        DWRITE_LINE_METRICS stack[64];
        DWRITE_LINE_METRICS* metrics = stack;
        UINT actual = 0;
        std::vector<DWRITE_LINE_METRICS> heap;
        HRESULT hr = b.layout->GetLineMetrics(stack, 64, &actual);
        if (hr == E_NOT_SUFFICIENT_BUFFER) {
            heap.resize(actual);
            metrics = heap.data();
            hr = b.layout->GetLineMetrics(heap.data(),
                                          (UINT)heap.size(), &actual);
        }
        if (FAILED(hr) || actual == 0) return;

        UINT acc = 0;
        for (UINT i = 0; i < actual; ++i) {
            UINT lineEnd = acc + metrics[i].length;
            const bool isLastLine = (i + 1 == actual);
            if (textIdx >= acc && (textIdx < lineEnd || (isLastLine && textIdx == lineEnd))) {
                UINT visibleEnd = lineEnd - metrics[i].newlineLength;
                m_selStart = { blockIdx, acc };
                m_selEnd   = { blockIdx, visibleEnd };
                m_dragging = false;
                InvalidateRect(m_hwnd, nullptr, FALSE);
                return;
            }
            acc = lineEnd;
        }
    }

    // 단어 경계로 양쪽 확장. 같은 CharClass 끼리 묶고, Other 면 한 글자만.
    void SelectWordAt(int blockIdx, UINT textIdx) {
        if (blockIdx < 0 || (size_t)blockIdx >= m_blocks.size()) return;
        const auto& b = m_blocks[(size_t)blockIdx];
        const std::wstring& src = b.styled.text;
        if (src.empty()) return;
        if (textIdx >= src.size()) textIdx = (UINT)src.size() - 1;

        UINT lo = textIdx, hi = textIdx;
        CharClass cls = ClassifyChar(src[textIdx]);
        if (cls == CharClass::Other) {
            hi = textIdx + 1;  // 공백·구두점 등은 한 글자만
        } else {
            while (lo > 0 && ClassifyChar(src[lo - 1]) == cls) --lo;
            while (hi < src.size() && ClassifyChar(src[hi]) == cls) ++hi;
        }
        m_selStart = { blockIdx, lo };
        m_selEnd   = { blockIdx, hi };
        m_dragging = false;  // 더블클릭 결과는 즉시 확정
        InvalidateRect(m_hwnd, nullptr, FALSE);
    }

    // 임의의 텍스트를 클립보드에 (CF_UNICODETEXT). 빈 문자열은 false.
    bool CopyTextToClipboard(const std::wstring& text) {
        if (text.empty()) return false;
        if (!OpenClipboard(m_hwnd)) return false;
        EmptyClipboard();
        size_t bytes = (text.size() + 1) * sizeof(wchar_t);
        HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, bytes);
        bool ok = false;
        if (h) {
            void* p = GlobalLock(h);
            if (p) {
                memcpy(p, text.c_str(), bytes);
                GlobalUnlock(h);
                if (SetClipboardData(CF_UNICODETEXT, h)) ok = true;
                else GlobalFree(h);
            } else {
                GlobalFree(h);
            }
        }
        CloseClipboard();
        return ok;
    }

    // 선택 영역 복사. 다중 블록이면 블록 사이 빈 줄로 구분.
    // styled.text(마크다운 제거된 디스플레이 텍스트) 기준 — IDWriteTextLayout 인덱스와 일치.
    bool CopySelection() {
        if (!HasSelection()) return false;
        SelectionEnd lo = SelLo(), hi = SelHi();
        std::wstring out;
        out.reserve(256);
        for (int i = lo.block; i <= hi.block && (size_t)i < m_blocks.size(); ++i) {
            const auto& b = m_blocks[(size_t)i];
            const std::wstring& src = b.styled.text.empty() ? b.text : b.styled.text;
            UINT s = (i == lo.block) ? lo.idx : 0u;
            UINT e = (i == hi.block) ? hi.idx : (UINT)src.size();
            if (s > src.size()) s = (UINT)src.size();
            if (e > src.size()) e = (UINT)src.size();
            if (e > s) out += src.substr(s, e - s);
            if (i < hi.block) out += L"\r\n\r\n";
        }
        return CopyTextToClipboard(out);
    }

    // 모든 블록 텍스트를 모아 클립보드로. role 라벨 prefix 로 가독성.
    bool CopyAllToClipboard() {
        std::wstring out;
        out.reserve(1024);
        for (const auto& b : m_blocks) {
            if (b.text.empty()) continue;
            if (b.role == L"user") {
                out += L"[user]\r\n";
            } else if (b.role == L"assistant") {
                out += L"[assistant]\r\n";
            } else if (b.role == L"tool") {
                out += L"[tool] ";
            }
            out += b.text;
            out += L"\r\n\r\n";
        }
        return CopyTextToClipboard(out);
    }

    void DrawTable(IDWriteTextLayout* layout, const TableInfo& tbl,
                   const std::vector<TextSpan>& spans,
                   float originX, float originY)
    {
        if (!layout || !m_tableBorderBrush) return;
        if (tbl.rowCount == 0 || tbl.colCount == 0) return;

        struct CellRect {
            uint16_t row = 0;
            uint16_t col = 0;
            TextSpan::Style style = TextSpan::Style::TableCell;
            D2D1_RECT_F textRect{};
            bool valid = false;
        };

        std::vector<float> rowTop(tbl.rowCount, 1e9f);
        std::vector<float> rowBottom(tbl.rowCount, -1e9f);
        std::vector<float> colLeft(tbl.colCount, 1e9f);
        std::vector<float> colRight(tbl.colCount, -1e9f);
        std::vector<CellRect> cells;
        cells.reserve(tbl.cells.size());

        constexpr float kCellPadX = 8.0f;
        constexpr float kCellPadY = 4.0f;

        for (const auto& c : tbl.cells) {
            if (c.spanIdx >= spans.size() || c.row >= tbl.rowCount || c.col >= tbl.colCount) continue;
            const auto& sp = spans[c.spanIdx];
            DWRITE_HIT_TEST_METRICS m[8]; // 한 셀이 여러 줄일 수 있음
            UINT32 actual = 0;
            if (FAILED(layout->HitTestTextRange(sp.start, sp.length, 0, 0, m, 8, &actual)) || actual == 0) continue;

            float left = m[0].left, top = m[0].top, right = m[0].left + m[0].width, bottom = m[0].top + m[0].height;
            for (UINT32 i = 1; i < actual; ++i) {
                if (m[i].left < left) left = m[i].left;
                if (m[i].top < top) top = m[i].top;
                if (m[i].left + m[i].width > right) right = m[i].left + m[i].width;
                if (m[i].top + m[i].height > bottom) bottom = m[i].top + m[i].height;
            }

            CellRect cell;
            cell.row = c.row;
            cell.col = c.col;
            cell.style = sp.style;
            cell.textRect = D2D1::RectF(left, top, right, bottom);
            cell.valid = true;
            cells.push_back(cell);

            if (left - kCellPadX < colLeft[c.col]) colLeft[c.col] = left - kCellPadX;
            if (right + kCellPadX > colRight[c.col]) colRight[c.col] = right + kCellPadX;
            if (top - kCellPadY < rowTop[c.row]) rowTop[c.row] = top - kCellPadY;
            if (bottom + kCellPadY > rowBottom[c.row]) rowBottom[c.row] = bottom + kCellPadY;
        }

        if (cells.empty()) return;

        for (uint16_t row = 0; row < tbl.rowCount; ++row) {
            if (rowTop[row] > rowBottom[row]) continue;
            for (uint16_t col = 0; col < tbl.colCount; ++col) {
                if (colLeft[col] > colRight[col]) continue;

                D2D1_RECT_F cellRect = D2D1::RectF(
                    originX + colLeft[col],
                    originY + rowTop[row],
                    originX + colRight[col],
                    originY + rowBottom[row]);

                bool isHeader = false;
                for (const auto& cell : cells) {
                    if (cell.row == row && cell.col == col && cell.style == TextSpan::Style::TableHeader) {
                        isHeader = true;
                        break;
                    }
                }

                if (isHeader) {
                    m_renderTarget->FillRectangle(cellRect, m_tableHeaderBgBrush);
                } else if (row % 2 == 1) {
                    m_renderTarget->FillRectangle(cellRect, m_tableRowAltBgBrush);
                }
                m_renderTarget->DrawRectangle(cellRect, m_tableBorderBrush, 0.5f);
            }
        }
    }

    void DrawSmallText(const std::wstring& text, float x, float y, float w, float h,
                       ID2D1SolidColorBrush* brush, float size = 12.0f,
                       DWRITE_FONT_WEIGHT weight = DWRITE_FONT_WEIGHT_NORMAL) {
        if (!m_textFormat || !brush || text.empty()) return;
        IDWriteTextLayout* layout = nullptr;
        if (SUCCEEDED(detail::DWFactory()->CreateTextLayout(
                text.c_str(), (UINT32)text.size(), m_textFormat, w, h, &layout)) && layout) {
            DWRITE_TEXT_RANGE all{0, (UINT32)text.size()};
            layout->SetFontSize(size, all);
            layout->SetFontWeight(weight, all);
            m_renderTarget->DrawTextLayout(D2D1::Point2F(x, y), layout, brush);
            detail::SafeRelease(layout);
        }
    }

    D2D1_RECT_F DrawAttachmentAction(bool folder, float x, float y) {
        float w = 30.0f;
        float h = 28.0f;
        D2D1_RECT_F r = D2D1::RectF(x, y, x + w, y + h);
        D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(r, 6.0f, 6.0f);
        if (m_assistantBgBrush) m_renderTarget->FillRoundedRectangle(rr, m_assistantBgBrush);
        if (m_taskBorderBrush) m_renderTarget->DrawRoundedRectangle(rr, m_taskBorderBrush, 1.0f);
        DrawActionIcon(r, folder);
        return r;
    }

    static bool PtInRectF(float x, float y, const D2D1_RECT_F& r) {
        return x >= r.left && x <= r.right && y >= r.top && y <= r.bottom;
    }

    float AttachmentCardWidth(float availableWidth) const {
        float cardW = availableWidth * 0.78f;
        if (cardW < 500.0f) cardW = 500.0f;
        if (cardW > 720.0f) cardW = 720.0f;
        return cardW;
    }

    bool OpenAttachmentActionAt(int clientX, int clientY) {
        float px = (float)clientX;
        float py = (float)clientY;
        for (const auto& b : m_blocks) {
            if (b.role != L"attachment") continue;
            float blockTop = b.yTop - m_scrollY;
            if (py < blockTop || py > blockTop + b.height + kPaddingY * 2.0f) continue;

            const float cardLeft = kPaddingX - kBlockHaloX;
            const float cardTop = kPaddingY + b.yTop - m_scrollY;
            const float cardRight = cardLeft + AttachmentCardWidth(ContentWidth());
            const float x = cardLeft + 20.0f;
            float y = cardTop + 18.0f + 30.0f + 34.0f;

            for (const auto& item : b.attachment.items) {
                float itemTop = y;
                D2D1_RECT_F fileR = D2D1::RectF(cardRight - 96.0f, itemTop + 20.0f,
                                                cardRight - 66.0f, itemTop + 48.0f);
                D2D1_RECT_F folderR = D2D1::RectF(cardRight - 60.0f, itemTop + 20.0f,
                                                  cardRight - 30.0f, itemTop + 48.0f);
                if (PtInRectF(px, py, fileR)) {
                    ShellExecuteW(nullptr, L"open", item.storedPath.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                    return true;
                }
                if (PtInRectF(px, py, folderR)) {
                    ShellExecuteW(nullptr, L"open", item.folderUrl.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                    return true;
                }
                y += item.thumbnailUrl.empty() ? 82.0f : 124.0f;
            }

            D2D1_RECT_F manifestFile = D2D1::RectF(cardRight - 96.0f, y + 16.0f,
                                                   cardRight - 66.0f, y + 44.0f);
            D2D1_RECT_F manifestFolder = D2D1::RectF(cardRight - 60.0f, y + 16.0f,
                                                     cardRight - 30.0f, y + 44.0f);
            if (PtInRectF(px, py, manifestFile)) {
                ShellExecuteW(nullptr, L"open", b.attachment.manifestPath.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                return true;
            }
            if (PtInRectF(px, py, manifestFolder)) {
                ShellExecuteW(nullptr, L"open", b.attachment.manifestFolderUrl.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                return true;
            }
        }
        return false;
    }

    bool IsAttachmentActionAt(int clientX, int clientY) {
        float px = (float)clientX;
        float py = (float)clientY;
        for (const auto& b : m_blocks) {
            if (b.role != L"attachment") continue;
            const float cardLeft = kPaddingX - kBlockHaloX;
            const float cardTop = kPaddingY + b.yTop - m_scrollY;
            const float cardRight = cardLeft + AttachmentCardWidth(ContentWidth());
            const float x = cardLeft + 20.0f;
            float y = cardTop + 18.0f + 30.0f + 34.0f;
            for (const auto& item : b.attachment.items) {
                if (PtInRectF(px, py, D2D1::RectF(cardRight - 96.0f, y + 20.0f,
                                                  cardRight - 66.0f, y + 48.0f)) ||
                    PtInRectF(px, py, D2D1::RectF(cardRight - 60.0f, y + 20.0f,
                                                  cardRight - 30.0f, y + 48.0f))) {
                    return true;
                }
                y += item.thumbnailUrl.empty() ? 82.0f : 124.0f;
            }
            if (PtInRectF(px, py, D2D1::RectF(cardRight - 96.0f, y + 16.0f,
                                              cardRight - 66.0f, y + 44.0f)) ||
                PtInRectF(px, py, D2D1::RectF(cardRight - 60.0f, y + 16.0f,
                                              cardRight - 30.0f, y + 44.0f))) {
                return true;
            }
        }
        return false;
    }

    void DrawAttachmentBlock(Block& b, float availableWidth) {
        EnsureTextFormat();
        float cardW = AttachmentCardWidth(availableWidth);
        const float cardLeft = kPaddingX - kBlockHaloX;
        const float cardTop = kPaddingY + b.yTop - m_scrollY;
        const float cardRight = cardLeft + cardW;
        D2D1_RECT_F cardRect = D2D1::RectF(cardLeft, cardTop, cardRight, cardTop + b.height);
        D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(cardRect, kCardRadius, kCardRadius);
        if (m_taskBgBrush) m_renderTarget->FillRoundedRectangle(rr, m_taskBgBrush);
        if (m_taskBorderBrush) m_renderTarget->DrawRoundedRectangle(rr, m_taskBorderBrush, 1.0f);

        float x = cardLeft + 20.0f;
        float y = cardTop + 18.0f;
        DrawSmallText(L"첨부", x, y, 120.0f, 22.0f, m_textBrush, 14.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD);
        y += 30.0f;
        DrawSmallText(L"원본 파일은 manifest의 stored_path로 전달됩니다.", x, y, cardW - 40.0f, 20.0f,
                      m_dimTextBrush, 12.0f);
        y += 30.0f;

        for (const auto& item : b.attachment.items) {
            float itemTop = y;
            float thumbW = 132.0f;
            float thumbH = item.thumbnailUrl.empty() ? 0.0f : 92.0f;
            if (!item.thumbnailUrl.empty()) {
                D2D1_RECT_F thumbRect = D2D1::RectF(x, y, x + thumbW, y + thumbH);
                D2D1_ROUNDED_RECT trr = D2D1::RoundedRect(thumbRect, 4.0f, 4.0f);
                if (m_assistantBgBrush) m_renderTarget->FillRoundedRectangle(trr, m_assistantBgBrush);
                if (m_taskBorderBrush) m_renderTarget->DrawRoundedRectangle(trr, m_taskBorderBrush, 1.0f);
                if (ID2D1Bitmap* bitmap = LoadImageBitmap(item.thumbnailUrl)) {
                    D2D1_SIZE_F src = bitmap->GetSize();
                    if (src.width > 0 && src.height > 0) {
                        float scale = thumbW / src.width;
                        if (src.height * scale > thumbH) scale = thumbH / src.height;
                        float bw = src.width * scale;
                        float bh = src.height * scale;
                        D2D1_RECT_F dst = D2D1::RectF(
                            thumbRect.left + (thumbW - bw) * 0.5f,
                            thumbRect.top + (thumbH - bh) * 0.5f,
                            thumbRect.left + (thumbW - bw) * 0.5f + bw,
                            thumbRect.top + (thumbH - bh) * 0.5f + bh);
                        m_renderTarget->DrawBitmap(bitmap, dst, 1.0f,
                                                   D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
                    }
                }
            }

            float infoX = item.thumbnailUrl.empty() ? x : x + thumbW + 16.0f;
            DrawSmallText(item.kind == L"image" ? L"이미지" : L"파일", infoX, itemTop,
                          90.0f, 18.0f, m_dimTextBrush, 11.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD);
            DrawSmallText(item.name, infoX, itemTop + 22.0f, cardRight - infoX - 100.0f, 26.0f,
                          m_textBrush, 13.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD);
            DrawAttachmentAction(false, cardRight - 96.0f, itemTop + 20.0f);
            DrawAttachmentAction(true, cardRight - 60.0f, itemTop + 20.0f);
            DrawSmallText(item.sizeLabel, infoX, itemTop + 54.0f, 120.0f, 18.0f,
                          m_dimTextBrush, 12.0f);

            y += item.thumbnailUrl.empty() ? 82.0f : 124.0f;
        }

        DrawSmallText(L"Manifest", x, y, 90.0f, 18.0f, m_dimTextBrush, 11.0f,
                      DWRITE_FONT_WEIGHT_SEMI_BOLD);
        DrawSmallText(b.attachment.manifestPath, x, y + 20.0f, cardW - 120.0f, 20.0f,
                      m_textBrush, 12.0f);
        DrawAttachmentAction(false, cardRight - 96.0f, y + 16.0f);
        DrawAttachmentAction(true, cardRight - 60.0f, y + 16.0f);

        if (!b.timeLabel.empty()) {
            DrawSmallText(b.timeLabel, cardRight + 8.0f, cardRect.bottom - 17.0f,
                          64.0f, 18.0f, m_dimTextBrush, 11.0f);
        }
    }

    void OnPaint() {
        EnsureDeviceResources();
        if (!m_renderTarget) {
            ValidateRect(m_hwnd, nullptr);
            return;
        }

        RECT rc;
        GetClientRect(m_hwnd, &rc);
        float viewH = (float)(rc.bottom - rc.top) - kPaddingY * 2;
        float width = ContentWidth();

        m_renderTarget->BeginDraw();
        m_renderTarget->Clear(BgColor());

        const float clientRight = (float)(rc.right - rc.left);

        // 보이는 블록만 layout 보장 + 그리기
        for (auto& b : m_blocks) {
            float blockTop    = b.yTop;
            float blockBottom = blockTop + b.height;
            float visTop      = m_scrollY;
            float visBottom   = m_scrollY + viewH;

            // viewport 위쪽
            if (blockBottom < visTop - 200.0f) continue;
            // viewport 아래쪽 — 정렬되어 있으니 끊으면 됨
            if (blockTop > visBottom + 200.0f) break;

            EnsureBlockLayout(b, width);
            if (b.role == L"attachment") {
                DrawAttachmentBlock(b, width);
                continue;
            }
            if (!b.layout) continue;

            int blockIdx = (int)(&b - m_blocks.data());
            const bool hasBackendLabel = HasBackendLabel(b);
            const bool hasTopLabel = HasTopLabel(b);
            const bool usesWorkCard = UsesWorkCard(b);
            const float labelPadY = hasTopLabel ? 22.0f : 0.0f;
            const float textY = kPaddingY + blockTop - m_scrollY + labelPadY;
            DWRITE_TEXT_METRICS textMetrics{};
            b.layout->GetMetrics(&textMetrics);
            const bool isUserBubble = (b.role == L"user");
            const float bubbleMaxW = BubbleMaxWidth(b, width);
            bool hasCodeBlock = false;
            for (const auto& sp : b.styled.spans) {
                if (sp.style == TextSpan::Style::CodeBlock) {
                    hasCodeBlock = true;
                    break;
                }
            }
            float bubbleW = textMetrics.widthIncludingTrailingWhitespace + kBlockHaloX * 2.0f + 18.0f;
            if (bubbleW < 132.0f) bubbleW = 132.0f;
            if (IsActiveProgressBlock(b) && bubbleW < 220.0f) bubbleW = 220.0f;
            if (usesWorkCard && bubbleW < 360.0f) bubbleW = 360.0f;
            if (hasCodeBlock) bubbleW = bubbleMaxW;
            if (bubbleW > bubbleMaxW) bubbleW = bubbleMaxW;

            float cardLeft = isUserBubble
                ? (clientRight - kPaddingX - kAvatarSize - kAvatarMargin - bubbleW)
                : (kPaddingX + kAvatarSize + kAvatarMargin - kBlockHaloX);
            if (cardLeft < kPaddingX + kAvatarSize + kAvatarMargin - kBlockHaloX) 
                cardLeft = kPaddingX + kAvatarSize + kAvatarMargin - kBlockHaloX;

            float cardRight = cardLeft + bubbleW;
            BlockTextOrigin origin = ComputeBlockTextOrigin(b, clientRight);
            float textX = origin.valid ? origin.x : (cardLeft + kBlockHaloX);

            // 아바타 그리기
            float avatarX = isUserBubble ? (clientRight - kPaddingX - kAvatarSize) : kPaddingX;
            float avatarY = textY - labelPadY - 4.0f;
            DrawAvatar(b.role, avatarX, avatarY);

            // 역할별 시각 차별 — 모든 role 카드 결 통일 (kCardRadius 10px), 좌측 stripe / 테두리 / 색으로 분리.
            const D2D1_RECT_F cardRect = D2D1::RectF(
                cardLeft,
                textY      - kBlockHaloY - labelPadY,
                cardRight,
                textY      + b.height + kBlockHaloY);
            const D2D1_ROUNDED_RECT cardRR = { cardRect, kCardRadius, kCardRadius };

            if ((b.role == L"task" || b.role == L"comm" || b.role == L"member") &&
                m_taskBgBrush && m_taskBorderBrush) {
                m_renderTarget->FillRoundedRectangle(cardRR, m_taskBgBrush);
                m_renderTarget->DrawRoundedRectangle(cardRR, m_taskBorderBrush, 1.0f);
            } else if (b.role == L"tool" && m_renderTarget) {
                ID2D1SolidColorBrush* toolBg = nullptr;
                ID2D1SolidColorBrush* toolStroke = nullptr;
                ID2D1SolidColorBrush* toolStripe = nullptr;
                m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(0xEFF6FF, 1.0f), &toolBg);
                m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(0xBFDBFE, 1.0f), &toolStroke);
                m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(0x3B82F6, 1.0f), &toolStripe);
                if (toolBg)     { m_renderTarget->FillRoundedRectangle(cardRR, toolBg);            detail::SafeRelease(toolBg); }
                if (toolStroke) { m_renderTarget->DrawRoundedRectangle(cardRR, toolStroke, 1.0f); detail::SafeRelease(toolStroke); }
                if (toolStripe) {
                    D2D1_RECT_F tstripe = D2D1::RectF(cardRect.left, cardRect.top + kCardRadius,
                                                       cardRect.left + 3.0f, cardRect.bottom - kCardRadius);
                    m_renderTarget->FillRectangle(tstripe, toolStripe);
                    detail::SafeRelease(toolStripe);
                }
            } else if (b.role == L"user" && m_userBgBrush) {
                m_renderTarget->FillRoundedRectangle(cardRR, m_userBgBrush);
                if (m_userBorderBrush) m_renderTarget->DrawRoundedRectangle(cardRR, m_userBorderBrush, 1.0f);
                if (!b.userName.empty()) {
                    // Position the user name slightly above the message bubble.
                    DrawSmallText(b.userName, cardRect.left, cardRect.top - kUserNameHeight - kUserNamePadding + 2.0f,
                                  cardRect.right - cardRect.left, kUserNameHeight, m_textBrush, 12.0f, DWRITE_FONT_WEIGHT_BOLD);
                }
            } else if (b.role == L"assistant" ||
                       b.role == L"assistant-claude" ||
                       b.role == L"assistant-gemini" ||
                       b.role == L"assistant-codex" ||
                       b.role == L"assistant-mock") {
                // Provider별 고유 색상 정체성 — stripe + 카드 테두리 tint
                ID2D1SolidColorBrush* providerAccent = m_assistantAccentBrush;  // Claude: 오렌지
                if (b.role == L"assistant-gemini" && m_geminiAccentBrush) providerAccent = m_geminiAccentBrush;
                else if (b.role == L"assistant-codex" && m_codexAccentBrush) providerAccent = m_codexAccentBrush;

                // 카드 배경: provider별 미세 tint
                if (m_renderTarget) {
                    D2D1::ColorF cardBg = D2D1::ColorF(0xFFFFFF, 1.0f);  // Claude: 순백
                    if (b.role == L"assistant-gemini") cardBg = D2D1::ColorF(0xF8FBFF, 1.0f);  // Gemini: 극미세 파란 tint
                    else if (b.role == L"assistant-codex") cardBg = D2D1::ColorF(0xF7FFFC, 1.0f);  // Codex: 극미세 초록 tint
                    ID2D1SolidColorBrush* cardBgB = nullptr;
                    m_renderTarget->CreateSolidColorBrush(cardBg, &cardBgB);
                    if (cardBgB) {
                        m_renderTarget->FillRoundedRectangle(cardRR, cardBgB);
                        detail::SafeRelease(cardBgB);
                    }
                }
                // 카드 테두리: provider 색 10% opacity
                if (providerAccent && m_renderTarget) {
                    D2D1_COLOR_F origColor = providerAccent->GetColor();
                    ID2D1SolidColorBrush* borderTintB = nullptr;
                    m_renderTarget->CreateSolidColorBrush(
                        D2D1::ColorF(origColor.r, origColor.g, origColor.b, 0.18f), &borderTintB);
                    if (borderTintB) {
                        m_renderTarget->DrawRoundedRectangle(cardRR, borderTintB, 1.0f);
                        detail::SafeRelease(borderTintB);
                    }
                }
                if (providerAccent) {
                    D2D1_RECT_F stripe = D2D1::RectF(
                        cardRect.left,
                        cardRect.top + kCardRadius,
                        cardRect.left + kAssistantStripeW,
                        cardRect.bottom - kCardRadius);
                    m_renderTarget->FillRectangle(stripe, providerAccent);
                }
            } else if (b.role == L"debug") {
                // 디버그 블록 스타일: 연한 회색 배경
                if (m_dimTextBrush) {
                    m_renderTarget->FillRoundedRectangle(cardRR, m_codeBlockBgBrush);
                }
            } else if (b.role == L"error" && m_errorBgBrush && m_errorAccentBrush) {
                m_renderTarget->FillRoundedRectangle(cardRR, m_errorBgBrush);
                D2D1_RECT_F stripe = D2D1::RectF(
                    cardRect.left,
                    cardRect.top + kCardRadius,
                    cardRect.left + kErrorStripeW,
                    cardRect.bottom - kCardRadius);
                m_renderTarget->FillRectangle(stripe, m_errorAccentBrush);
            }

            // 코드 박스 배경 — 텍스트보다 먼저 그린다.
            //   CodeBlock(```)  : 한 블록을 단일 둥근 사각으로 합쳐 그림 (콘텐츠 폭 끝까지 균일)
            //   InlineCode(`)   : 토큰별 작은 둥근 사각 (per-line, 폭은 텍스트 폭 + 약간 패딩)
            if (m_codeBlockBgBrush) {
                const float codeRight = b.layoutWidth;
                for (size_t i = 0; i < b.styled.spans.size(); ++i) {
                    const auto& sp = b.styled.spans[i];
                    if (sp.style == TextSpan::Style::CodeBlock) {
                        D2D1_RECT_F codeRect = DrawSpanBackgroundMerged(b.layout, sp, textX, textY,
                                                 codeRight, m_codeBlockBgBrush);

                        bool hasLangLabel = false;
                        for (size_t j = 0; j < i; ++j) {
                            const auto& langSp = b.styled.spans[j];
                            if (langSp.style == TextSpan::Style::CodeBlockLang &&
                                langSp.start == sp.start && langSp.length > 0) {
                                DWRITE_HIT_TEST_METRICS htStack[4]{};
                                UINT htActual = 0;
                                if (SUCCEEDED(b.layout->HitTestTextRange(
                                        langSp.start, langSp.length, 0, 0,
                                        htStack, 4, &htActual)) && htActual > 0) {
                                    float sepY = textY + htStack[0].top + htStack[0].height - 0.5f;

                                    // Header background (slightly darker than code body)
                                    if (m_codeHeaderBgBrush && sepY > codeRect.top + kCodeBlockRadius) {
                                        D2D1_RECT_F hdrFlat = D2D1::RectF(
                                            codeRect.left, codeRect.top + kCodeBlockRadius,
                                            codeRect.right, sepY + 0.5f);
                                        m_renderTarget->FillRectangle(hdrFlat, m_codeHeaderBgBrush);
                                    }

                                    // Separator line
                                    if (m_taskBorderBrush && sepY > codeRect.top && sepY < codeRect.bottom) {
                                        m_renderTarget->DrawLine(
                                            D2D1::Point2F(codeRect.left,  sepY),
                                            D2D1::Point2F(codeRect.right, sepY),
                                            m_taskBorderBrush, 1.0f);
                                    }

                                    // Always-visible copy button in header area
                                    DrawCopyButton(D2D1::RectF(
                                        codeRect.left, codeRect.top,
                                        codeRect.right, sepY + 0.5f), blockIdx, i);
                                    hasLangLabel = true;
                                }
                                break;
                            }
                        }

                        // Hover copy button only when no lang label
                        if (!hasLangLabel &&
                            m_mousePos.x >= codeRect.left && m_mousePos.x <= codeRect.right &&
                            m_mousePos.y >= codeRect.top && m_mousePos.y <= codeRect.bottom) {
                            DrawCopyButton(codeRect, blockIdx, i);
                        }
                    } else if (sp.style == TextSpan::Style::InlineCode) {
                        DrawSpanBackgroundPerLine(b.layout, sp, textX, textY,
                                                  /*padX=*/2.5f, /*padY=*/0.0f,
                                                  /*radius=*/2.5f, m_codeBlockBgBrush);
                    }
                }
            }
            if (m_renderTarget) {
                for (const auto& tbl : b.styled.tables) {
                    DrawTable(b.layout, tbl, b.styled.spans, textX, textY);
                }
            }

            // Blockquote: tinted background + left border bar
            for (const auto& sp : b.styled.spans) {
                if (sp.style == TextSpan::Style::Quote && sp.length > 0) {
                    DWRITE_HIT_TEST_METRICS htStack[32]{};
                    UINT htActual = 0;
                    if (SUCCEEDED(b.layout->HitTestTextRange(
                            sp.start, sp.length, 0, 0,
                            htStack, 32, &htActual)) && htActual > 0) {
                        float qTop    = textY + htStack[0].top - 2.0f;
                        float qBottom = textY + htStack[htActual - 1].top
                                      + htStack[htActual - 1].height + 2.0f;
                        float barX = textX - 10.0f;
                        // Tinted background
                        if (m_quoteBgBrush) {
                            m_renderTarget->FillRectangle(
                                D2D1::RectF(barX, qTop, cardRect.right - kBlockHaloX, qBottom),
                                m_quoteBgBrush);
                        }
                        // Left bar
                        if (m_quoteBorderBrush) {
                            m_renderTarget->FillRectangle(
                                D2D1::RectF(barX, qTop, barX + 4.0f, qBottom),
                                m_quoteBorderBrush);
                        }
                    }
                }
            }

            // 다중 블록 cross 선택 fill — 텍스트보다 먼저 그려 글자를 덮지 않게 한다.
            if (m_selectionFillBrush && HasSelection()) {
                SelectionEnd lo = SelLo(), hi = SelHi();
                if (blockIdx >= lo.block && blockIdx <= hi.block) {
                    UINT spanLo = (blockIdx == lo.block) ? lo.idx : 0u;
                    UINT spanHi = (blockIdx == hi.block) ? hi.idx
                                                         : (UINT)b.styled.text.size();
                    if (spanHi > spanLo) {
                        TextSpan sp;
                        sp.start  = spanLo;
                        sp.length = spanHi - spanLo;
                        sp.style  = TextSpan::Style::Bold;  // 미사용 placeholder
                        DrawSpanBackgroundPerLine(b.layout, sp, textX, textY,
                                                  /*padX=*/0.0f, /*padY=*/0.0f,
                                                  /*radius=*/0.0f, m_selectionFillBrush);
                    }
                }
            }

            // tool / retry 블록은 흐린 회색, 나머지는 본문 색
            ID2D1SolidColorBrush* brush =
                ((b.role == L"tool" || b.role == L"retry") && m_dimTextBrush) ? m_dimTextBrush : m_textBrush;

            if (IsPendingIndicator(b) && m_assistantAccentBrush) {
                DrawBlockProgress(cardRect, true);
            } else {
                D2D1_POINT_2F p = D2D1::Point2F(textX, textY);
                m_renderTarget->DrawTextLayout(p, b.layout, brush,
                                               D2D1_DRAW_TEXT_OPTIONS_NONE);
                DrawImagePlaceholders(b.layout, b.styled.spans, textX, textY);
                if (IsActiveProgressBlock(b) && m_assistantAccentBrush && !b.styled.text.empty()) {
                    UINT32 textLen = (UINT32)b.styled.text.size();
                    float cx = 0.0f, cy = 0.0f;
                    DWRITE_HIT_TEST_METRICS htm{};
                    b.layout->HitTestTextPosition(textLen > 0 ? textLen - 1 : 0,
                                                  TRUE, &cx, &cy, &htm);
                    float dotCenterY = textY + cy + htm.height * 0.5f;
                    float dotX = textX + cx + 5.0f;
                    DWORD tick = GetTickCount();
                    D2D1::ColorF ac = AssistantAccentColor();
                    for (int d = 0; d < 3; ++d) {
                        float phase = ((tick + d * 200) % 900) / 900.0f;
                        float wave = (phase < 0.5f) ? phase * 2.0f : (1.0f - phase) * 2.0f;
                        ac.a = 0.25f + wave * 0.70f;
                        m_assistantAccentBrush->SetColor(ac);
                        D2D1_ELLIPSE dot = D2D1::Ellipse(
                            D2D1::Point2F(dotX + d * 9.0f, dotCenterY),
                            3.0f, 3.0f);
                        m_renderTarget->FillEllipse(dot, m_assistantAccentBrush);
                    }
                    ac.a = 1.0f;
                    m_assistantAccentBrush->SetColor(ac);
                }
            }

            // H1 / H2 separator lines drawn below each heading
            if (m_headingSepBrush && b.layout) {
                for (const auto& sp : b.styled.spans) {
                    if ((sp.style == TextSpan::Style::Heading1 ||
                         sp.style == TextSpan::Style::Heading2) && sp.length > 0) {
                        DWRITE_HIT_TEST_METRICS htStack[8]{};
                        UINT htActual = 0;
                        if (SUCCEEDED(b.layout->HitTestTextRange(
                                sp.start, sp.length, 0, 0,
                                htStack, 8, &htActual)) && htActual > 0) {
                            float sepY = textY + htStack[htActual - 1].top
                                       + htStack[htActual - 1].height + 3.0f;
                            float lineRight = cardRect.right - kBlockHaloX;
                            m_renderTarget->DrawLine(
                                D2D1::Point2F(textX, sepY),
                                D2D1::Point2F(lineRight, sepY),
                                m_headingSepBrush, 1.5f);
                        }
                    }
                }
            }

            if ((b.role == L"assistant-claude" ||
                 b.role == L"assistant-gemini" ||
                 b.role == L"assistant-codex" ||
                 b.role == L"assistant-mock") && m_renderTarget) {
                const wchar_t* label = L"Claude";
                D2D1::ColorF badgeColor = AssistantAccentColor();
                if (b.role == L"assistant-gemini") { label = L"Gemini"; badgeColor = D2D1::ColorF(0x2563EB, 1.0f); }
                if (b.role == L"assistant-codex")  { label = L"Codex";  badgeColor = D2D1::ColorF(0x059669, 1.0f); }
                if (b.role == L"assistant-mock")   { label = L"Mock";   badgeColor = D2D1::ColorF(0x9CA3AF, 1.0f); }
                IDWriteTextLayout* badgeLayout = nullptr;
                detail::DWFactory()->CreateTextLayout(
                    label, (UINT32)wcslen(label), m_textFormat, 100.0f, 20.0f, &badgeLayout);
                if (badgeLayout) {
                    DWRITE_TEXT_RANGE all = {0, (UINT32)wcslen(label)};
                    badgeLayout->SetFontSize(11.0f, all);
                    badgeLayout->SetFontWeight(DWRITE_FONT_WEIGHT_BOLD, all);
                    DWRITE_TEXT_METRICS lm{};
                    badgeLayout->GetMetrics(&lm);
                    const float bx = cardRect.left + kBlockHaloX;
                    const float by = cardRect.top  + 4.0f;
                    const float bw = lm.widthIncludingTrailingWhitespace + 12.0f;
                    const float bh = 17.0f;
                    const D2D1_ROUNDED_RECT badgeRR = { D2D1::RectF(bx, by, bx + bw, by + bh), 8.5f, 8.5f };
                    ID2D1SolidColorBrush* bgBrush = nullptr;
                    m_renderTarget->CreateSolidColorBrush(badgeColor, &bgBrush);
                    if (bgBrush) { m_renderTarget->FillRoundedRectangle(badgeRR, bgBrush); detail::SafeRelease(bgBrush); }
                    ID2D1SolidColorBrush* wBrush = nullptr;
                    m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), &wBrush);
                    if (wBrush) {
                        m_renderTarget->DrawTextLayout(D2D1::Point2F(bx + 6.0f, by + 2.0f),
                                                       badgeLayout, wBrush, D2D1_DRAW_TEXT_OPTIONS_NONE);
                        detail::SafeRelease(wBrush);
                    }
                    detail::SafeRelease(badgeLayout);
                }
            }

            if (b.role == L"user" && m_dimTextBrush && !m_userName.empty()) {
                const UINT32 uLen = (UINT32)m_userName.size();
                IDWriteTextLayout* uLayout = nullptr;
                detail::DWFactory()->CreateTextLayout(
                    m_userName.c_str(), uLen, m_textFormat, 200.0f, 18.0f, &uLayout);
                if (uLayout) {
                    DWRITE_TEXT_RANGE all = {0, uLen};
                    uLayout->SetFontSize(12.0f, all);
                    uLayout->SetFontWeight(DWRITE_FONT_WEIGHT_SEMI_BOLD, all);
                    DWRITE_TEXT_METRICS lm{};
                    uLayout->GetMetrics(&lm);
                    float lx = cardRect.right - kBlockHaloX - lm.widthIncludingTrailingWhitespace;
                    m_renderTarget->DrawTextLayout(
                        D2D1::Point2F(lx, cardRect.top - kUserNameHeight - kUserNamePadding + 2.0f), uLayout, m_dimTextBrush,
                        D2D1_DRAW_TEXT_OPTIONS_NONE);
                    detail::SafeRelease(uLayout);
                }
            }

            if (!b.timeLabel.empty() && m_dimTextBrush) {
                IDWriteTextLayout* timeLayout = nullptr;
                detail::DWFactory()->CreateTextLayout(
                    b.timeLabel.c_str(), (UINT32)b.timeLabel.size(), m_textFormat,
                    64.0f, 18.0f, &timeLayout);
                if (timeLayout) {
                    DWRITE_TEXT_RANGE all = {0, (UINT32)b.timeLabel.size()};
                    timeLayout->SetFontSize(11.0f, all);
                    D2D1_POINT_2F tp = isUserBubble
                        ? D2D1::Point2F(cardRect.left - 44.0f, cardRect.bottom - 17.0f)
                        : (hasBackendLabel
                            ? D2D1::Point2F(cardRect.right - 48.0f, cardRect.top + 5.0f)
                            : D2D1::Point2F(cardRect.right + 8.0f, cardRect.bottom - 17.0f));
                    m_renderTarget->DrawTextLayout(tp, timeLayout, m_dimTextBrush,
                                                   D2D1_DRAW_TEXT_OPTIONS_NONE);
                    detail::SafeRelease(timeLayout);
                }
            }
        }

        HRESULT hr = m_renderTarget->EndDraw();
        if (hr == D2DERR_RECREATE_TARGET) {
            DiscardDeviceResources();
        }
        ValidateRect(m_hwnd, nullptr);
    }

    void OnSize() {
        if (m_renderTarget) {
            RECT rc;
            GetClientRect(m_hwnd, &rc);
            m_renderTarget->Resize(D2D1::SizeU(rc.right - rc.left, rc.bottom - rc.top));
        }
        OnWidthChanged();
        ClampScroll();
    }

    void OnMouseWheel(int delta) {
        float step = 60.0f * ((float)delta / (float)WHEEL_DELTA);
        m_scrollY -= step;
        ClampScroll();
        UpdateScrollBar();
        InvalidateRect(m_hwnd, nullptr, FALSE);
    }

    void OnVScroll(WORD code) {
        SCROLLINFO si{};
        si.cbSize = sizeof(si);
        si.fMask  = SIF_ALL;
        GetScrollInfo(m_hwnd, SB_VERT, &si);
        int pos = si.nPos;
        switch (code) {
        case SB_LINEUP:        pos -= 20;                  break;
        case SB_LINEDOWN:      pos += 20;                  break;
        case SB_PAGEUP:        pos -= (int)si.nPage;       break;
        case SB_PAGEDOWN:      pos += (int)si.nPage;       break;
        case SB_THUMBTRACK:
        case SB_THUMBPOSITION: pos = si.nTrackPos;         break;
        case SB_TOP:           pos = 0;                    break;
        case SB_BOTTOM:        pos = si.nMax;              break;
        }
        m_scrollY = (float)pos;
        ClampScroll();
        UpdateScrollBar();
        InvalidateRect(m_hwnd, nullptr, FALSE);
        MaybeNotifyNearTop();
    }

    void MaybeNotifyNearTop() {
        if (m_nearTopCb && m_scrollY <= 48.0f) {
            m_nearTopCb();
        }
    }

    void Cleanup() {
        for (auto& b : m_blocks) detail::SafeRelease(b.layout);
        m_blocks.clear();
        detail::SafeRelease(m_textFormat);
        DiscardDeviceResources();
    }

    HWND                   m_hwnd                  = nullptr;
    ID2D1HwndRenderTarget* m_renderTarget          = nullptr;
    ID2D1SolidColorBrush*  m_textBrush             = nullptr;
    ID2D1SolidColorBrush*  m_dimTextBrush          = nullptr;
    ID2D1SolidColorBrush*  m_userBgBrush           = nullptr;
    ID2D1SolidColorBrush*  m_assistantBgBrush      = nullptr;
    ID2D1SolidColorBrush*  m_assistantAccentBrush  = nullptr;  // Claude — 브랜드 오렌지
    ID2D1SolidColorBrush*  m_geminiAccentBrush     = nullptr;  // Gemini — Google Blue
    ID2D1SolidColorBrush*  m_codexAccentBrush      = nullptr;  // Codex  — OpenAI Emerald
    ID2D1SolidColorBrush*  m_codeBlockBgBrush      = nullptr;
    ID2D1SolidColorBrush*  m_selectionBrush        = nullptr;
    ID2D1SolidColorBrush*  m_selectionFillBrush    = nullptr;
    ID2D1SolidColorBrush*  m_linkBrush             = nullptr;
    ID2D1SolidColorBrush*  m_taskBgBrush           = nullptr;
    ID2D1SolidColorBrush*  m_taskBorderBrush       = nullptr;
    ID2D1SolidColorBrush*  m_errorBgBrush          = nullptr;
    ID2D1SolidColorBrush*  m_errorAccentBrush      = nullptr;
    ID2D1SolidColorBrush*  m_tableHeaderBgBrush    = nullptr;
    ID2D1SolidColorBrush*  m_tableBorderBrush      = nullptr;
    ID2D1SolidColorBrush*  m_tableRowAltBgBrush    = nullptr;
    ID2D1SolidColorBrush*  m_userBorderBrush        = nullptr;
    ID2D1SolidColorBrush*  m_quoteBorderBrush      = nullptr;
    ID2D1SolidColorBrush*  m_quoteBgBrush          = nullptr;
    ID2D1SolidColorBrush*  m_codeHeaderBgBrush     = nullptr;
    ID2D1SolidColorBrush*  m_headingSepBrush       = nullptr;

    // Syntax Highlighting Brushes
    ID2D1SolidColorBrush*  m_codeKeywordBrush      = nullptr;
    ID2D1SolidColorBrush*  m_codeCommentBrush      = nullptr;
    ID2D1SolidColorBrush*  m_codeStringBrush       = nullptr;
    ID2D1SolidColorBrush*  m_codeNumberBrush       = nullptr;
    ID2D1SolidColorBrush*  m_codeTypeBrush         = nullptr;
    ID2D1SolidColorBrush*  m_codeFunctionBrush     = nullptr;
    ID2D1SolidColorBrush*  m_codePreprocBrush      = nullptr;

    IDWriteTextFormat*     m_textFormat            = nullptr;
    std::map<std::wstring, ID2D1Bitmap*> m_imageCache;
    ID2D1Bitmap*           m_fileActionIcon        = nullptr;
    ID2D1Bitmap*           m_folderActionIcon      = nullptr;
    ID2D1PathGeometry*     m_fileFluentIcon        = nullptr;
    ID2D1PathGeometry*     m_folderFluentIcon      = nullptr;
    float                  m_scrollY               = 0.0f;
    bool                   m_busy                  = false;
    std::wstring           m_userName;

    // 마우스 및 선택 상태
    D2D1_POINT_2F          m_mousePos              = { -1.0f, -1.0f };
    bool                   m_dragging              = false;
    SelectionEnd           m_selStart              = {-1, 0};
    SelectionEnd           m_selEnd                = {-1, 0};

    // 복사 버튼 피드백 (어느 블록의 어느 span 인지)
    int                    m_copiedBlockIdx        = -1;
    size_t                 m_copiedSpanIdx         = (size_t)-1;
    static constexpr UINT_PTR kCopyFeedbackTimerId = 3;
    // 트리플클릭(한 줄 선택) 검출용 — 직전 더블클릭 시각/블록/좌표 저장.
    DWORD                  m_lastDblClickTime      = 0;
    int                    m_lastDblClickBlock     = -1;
    int                    m_lastDblClickX         = 0;
    int                    m_lastDblClickY         = 0;
    // 자동 스크롤 — 드래그 중 viewport 위/아래로 마우스 나가면 깊이 비례 속도로 스크롤.
    int                    m_autoScrollDir         = 0;     // -1=위, 0=정지, 1=아래
    float                  m_autoScrollSpeed       = 0.0f;  // 한 틱당 픽셀 (깊이 기반)
    bool                   m_suppressScreenLog     = false;
    bool                   m_bulkUpdate            = false;
    CancelCallback         m_cancelCb;
    NearTopCallback        m_nearTopCb;
};

}  // namespace orange
