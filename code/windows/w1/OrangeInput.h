#pragma once

// COrangeInput — Direct2D 자체 입력 박스 (Win32 EDIT 폐기 토대).
//
// 사용자 04:24 *근본부터* 결: 채팅 입력 + 출력 제대로. 입력 영역의 근본
// = EDIT 컨트롤 폐기, 자체 텍스트 모델 + caret + Direct2D 그리기.
//
// 토대 1단계 (본 사이클 범위):
//   - 한 줄 텍스트 모델 (std::wstring + caret index)
//   - Direct2D + DirectWrite 그리기 (배경·둥근 모서리·placeholder·caret 깜빡임)
//   - 키 입력 (WM_CHAR 자모 / WM_KEYDOWN 방향키·Backspace·Delete·Home·End)
//   - 클릭 caret 이동 (HitTestPoint)
//   - Enter 전송 콜백
//   - placeholder (빈 상태일 때 dim 텍스트)
//
// 다음 단계 (별 사이클):
//   - 2단계: 다중 줄 (LF) + Shift+Enter 줄바꿈 + 자동 높이
//   - 3단계: 한글 IME (WM_IME_*)
//   - 4단계: 선택 (Shift+방향키·Shift+클릭·드래그) + 클립보드
//   - 5단계: 마크다운 입력 보조 (코드 fence·인용 prefix·리스트)
//
// 시그니처는 다음 단계에서 *추가만* — 외부 사용자(main.cpp) 영향 최소.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>
#include <imm.h>
#include <shellapi.h>
#include <d2d1.h>
#include <dwrite.h>
#include <functional>
#include <string>
#include <vector>

#include "Utils.h"

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "imm32.lib")

namespace orange {

class COrangeInput {
public:
    COrangeInput() = default;
    ~COrangeInput() {
        DiscardDeviceResources();
        if (m_tfText)        { m_tfText->Release();        m_tfText        = nullptr; }
        if (m_tfPlaceholder) { m_tfPlaceholder->Release(); m_tfPlaceholder = nullptr; }
        if (m_tfIcon)        { m_tfIcon->Release();        m_tfIcon        = nullptr; }
        if (m_dwFactory)     { m_dwFactory->Release();     m_dwFactory     = nullptr; }
        if (m_d2dFactory)    { m_d2dFactory->Release();    m_d2dFactory    = nullptr; }
    }

    using SubmitCallback = std::function<void(const std::wstring&)>;
    using PasteImageCallback = std::function<bool()>;
    using AttachFileCallback = std::function<void()>;
    using CaptureCallback = std::function<void()>;
    using LoopToggleCallback = std::function<void()>;
    using FilesDroppedCallback = std::function<void(const std::vector<std::wstring>&)>;
    using ProviderToggleCallback = std::function<void(bool, bool, bool)>;
    using ManagerProviderCallback = std::function<void(const std::string&)>;

    bool Create(HWND parent, HINSTANCE hInst);

    HWND Hwnd() const { return m_hwnd; }

    void Layout(int x, int y, int w, int h);

    // Enter 입력 시 호출. 본문 텍스트 받음. 호출자가 입력 비우려면 SetText(L"").
    void SetSubmitCallback(SubmitCallback cb) { m_submit = std::move(cb); }
    void SetPasteImageCallback(PasteImageCallback cb) { m_pasteImage = std::move(cb); }
    void SetAttachFileCallback(AttachFileCallback cb) { m_attachFile = std::move(cb); }
    void SetCaptureCallback(CaptureCallback cb) { m_capture = std::move(cb); }
    void SetLoopToggleCallback(LoopToggleCallback cb) { m_loopToggle = std::move(cb); }
    void SetAutoLoopActive(bool on) { m_autoLoopActive = on; Invalidate(); }
    void SetFilesDroppedCallback(FilesDroppedCallback cb) { m_filesDropped = std::move(cb); }
    void SetProviderToggleCallback(ProviderToggleCallback cb) { m_providerToggle = std::move(cb); }
    void SetManagerProviderCallback(ManagerProviderCallback cb) { m_managerProviderChanged = std::move(cb); }
    void SetProviderState(bool claude, bool gemini, bool codex = false) {
        m_useClaude = claude;
        m_useGemini = gemini;
        m_useCodex = codex;
        NormalizeManagerProvider();
        Invalidate();
    }
    void SetManagerProvider(const std::string& provider) {
        m_managerProvider = provider;
        NormalizeManagerProvider();
        Invalidate();
    }

    // 외부에서 텍스트 set / get (전송 후 비우기 등).
    void SetText(const std::wstring& s);
    std::wstring GetText() const { return m_text; }
    bool IsEmpty() const { return m_text.empty(); }

    // 외부에서 placeholder 텍스트 set (default = 영문 안내).
    void SetPlaceholder(const std::wstring& s) { m_placeholder = s; Invalidate(); }

    void InsertMarkdown(const std::wstring& prefix, const std::wstring& suffix);

    using TextChangeCallback = std::function<void()>;
    void SetTextChangeCallback(TextChangeCallback cb) { m_onTextChange = cb; }

    float GetIdealHeight(float width) {
        if (!m_dwFactory || !m_tfText) return 40.0f;
        IDWriteTextLayout* layout = nullptr;
        float h = 10000.0f; // 임시 큰 값
        float w = TextLayoutWidth(width);
        if (w < 1.0f) w = 1.0f;
        std::wstring textToMeasure = m_text.empty() ? L"A" : m_text;
        m_dwFactory->CreateTextLayout(
            textToMeasure.c_str(), (UINT32)textToMeasure.size(),
            m_tfText, w, h, &layout);
        if (!layout) return 40.0f;
        DWRITE_TEXT_METRICS tm;
        layout->GetMetrics(&tm);
        layout->Release();
        return tm.height + 2.0f * kPaddingY + 2.0f; // 약간의 여백 추가
    }

    static constexpr float kFontSize = 14.0f;

private:
    static LRESULT CALLBACK StaticWndProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT WndProc(HWND, UINT, WPARAM, LPARAM);

    bool EnsureFactories();
    bool EnsureDeviceResources();
    void DiscardDeviceResources();
    void OnPaint();

    void OnChar(wchar_t ch);
    void OnKeyDown(WPARAM vk);
    void OnLButtonDown(int x, int y);
    void OnLButtonDblClk(int x, int y);
    void OnMouseMove(int x, int y);
    void OnLButtonUp(int x, int y);
    void OnTimerCaret();
    POINT CaretPoint() const;
    void EnsureCaretVisible();
    void UpdateImeCompositionWindow();
    void DrawAttachButton(float W, float H);
    void DrawCaptureButton(float W, float H);
    void DrawLoopButton(float W, float H);
    bool HitAttachButton(float x, float y) const;
    bool HitCaptureButton(float x, float y) const;
    bool HitProviderButton(float x, float y) const;
    void DrawProviderPills(float W, float H);
    bool ToggleProviderAt(float x, float y);
    bool SetManagerProviderAt(float x, float y);
    void NormalizeManagerProvider();
    float TextLayoutWidth(float width) const;
    float TextOriginX() const { return kPaddingX + kAttachButtonW; }

    void Invalidate() { if (m_hwnd) InvalidateRect(m_hwnd, nullptr, FALSE); }

    // 현재 텍스트의 layout 캐시. 텍스트 바뀔 때마다 무효화.
    void RebuildLayout();

    static const wchar_t* kClassName;
    static bool s_classRegistered;
    static bool EnsureClass(HINSTANCE hInst);

    HWND m_hwnd = nullptr;
    bool m_focused = false;

    ID2D1Factory*           m_d2dFactory = nullptr;
    IDWriteFactory*         m_dwFactory  = nullptr;
    ID2D1HwndRenderTarget*  m_rt         = nullptr;
    IDWriteTextFormat*      m_tfText        = nullptr;
    IDWriteTextFormat*      m_tfPlaceholder = nullptr;
    IDWriteTextFormat*      m_tfIcon        = nullptr;
    IDWriteTextLayout*      m_layoutText    = nullptr;  // 현재 m_text 의 layout 캐시

    ID2D1SolidColorBrush*   m_bgBrush          = nullptr;
    ID2D1SolidColorBrush*   m_borderBrush      = nullptr;
    ID2D1SolidColorBrush*   m_borderFocusBrush = nullptr;
    ID2D1SolidColorBrush*   m_textBrush        = nullptr;
    ID2D1SolidColorBrush*   m_placeholderBrush = nullptr;
    ID2D1SolidColorBrush*   m_caretBrush       = nullptr;
    ID2D1SolidColorBrush*   m_selectionBrush   = nullptr;

    std::wstring   m_text;
    std::wstring   m_placeholder = L"메시지 입력 — Enter 전송, Shift+Enter 줄바꿈";
    int            m_caret       = 0;
    int            m_selAnchor   = 0; // 선택 시작점 (caret과 같으면 선택 없음)
    bool           m_caretVisible = true;
    bool           m_dragging    = false;
    float          m_scrollY      = 0.0f;
    bool           m_pendingProviderClick = false;
    POINT          m_pendingProviderPoint = {};
    SubmitCallback m_submit;
    PasteImageCallback m_pasteImage;
    AttachFileCallback m_attachFile;
    CaptureCallback m_capture;
    LoopToggleCallback m_loopToggle;
    FilesDroppedCallback m_filesDropped;
    ProviderToggleCallback m_providerToggle;
    ManagerProviderCallback m_managerProviderChanged;
    TextChangeCallback m_onTextChange;
    bool           m_useClaude = true;
    bool           m_useGemini = false;
    bool           m_useCodex = false;
    std::string    m_managerProvider = "claude";
    D2D1_RECT_F    m_claudeRect = {};
    D2D1_RECT_F    m_geminiRect = {};
    D2D1_RECT_F    m_codexRect = {};
    D2D1_RECT_F    m_attachRect = {};
    D2D1_RECT_F    m_captureRect = {};
    D2D1_RECT_F    m_loopRect = {};
    bool           m_autoLoopActive = false;

    static constexpr float kPaddingX  = 14.0f;
    static constexpr float kPaddingY  = 8.0f;
    static constexpr float kCornerR   = 12.0f;
    static constexpr float kAttachButtonW = 100.0f;  // attach(28)+camera(28)+loop(28)+gaps
    static constexpr float kProviderAreaW = 246.0f;
    static constexpr UINT_PTR kCaretTimerId = 1001;
    static constexpr UINT_PTR kProviderClickTimerId = 1002;
    static constexpr UINT     kCaretInterval = 500;
};

inline const wchar_t* COrangeInput::kClassName = L"OrangeInputBox";
inline bool COrangeInput::s_classRegistered = false;

inline bool COrangeInput::EnsureClass(HINSTANCE hInst) {
    if (s_classRegistered) return true;
    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    wc.lpfnWndProc   = StaticWndProc;
    wc.hInstance     = hInst;
    wc.hCursor       = LoadCursor(nullptr, IDC_IBEAM);
    wc.hbrBackground = nullptr;  // D2D — 매 프레임 Clear
    wc.lpszClassName = kClassName;
    if (!RegisterClassExW(&wc)) return false;
    s_classRegistered = true;
    return true;
}

inline LRESULT CALLBACK COrangeInput::StaticWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    COrangeInput* self = nullptr;
    if (msg == WM_NCCREATE) {
        auto* cs = (CREATESTRUCTW*)lp;
        self = (COrangeInput*)cs->lpCreateParams;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)self);
    } else {
        self = (COrangeInput*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    }
    if (self) return self->WndProc(hwnd, msg, wp, lp);
    return DefWindowProcW(hwnd, msg, wp, lp);
}

inline LRESULT COrangeInput::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_PAINT:
        OnPaint();
        ValidateRect(hwnd, nullptr);
        return 0;
    case WM_SIZE:
        if (m_rt) {
            D2D1_SIZE_U s = D2D1::SizeU(LOWORD(lp), HIWORD(lp));
            m_rt->Resize(s);
        }
        if (m_layoutText) { m_layoutText->Release(); m_layoutText = nullptr; }
        UpdateImeCompositionWindow();
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_LBUTTONDOWN:
        OnLButtonDown(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        SetFocus(hwnd);
        return 0;
    case WM_LBUTTONDBLCLK:
        OnLButtonDblClk(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        SetFocus(hwnd);
        return 0;
    case WM_MOUSEMOVE:
        OnMouseMove(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        return 0;
    case WM_MOUSEWHEEL: {
        if (!m_layoutText && !m_text.empty()) RebuildLayout();
        if (!m_layoutText) return 0;
        RECT rc;
        GetClientRect(hwnd, &rc);
        DWRITE_TEXT_METRICS tm{};
        m_layoutText->GetMetrics(&tm);
        float viewportH = (float)(rc.bottom - rc.top) - 2.0f * kPaddingY;
        float maxScroll = tm.height > viewportH ? (tm.height - viewportH) : 0.0f;
        int delta = GET_WHEEL_DELTA_WPARAM(wp);
        m_scrollY -= (delta / 120.0f) * 32.0f;
        if (m_scrollY < 0.0f) m_scrollY = 0.0f;
        if (m_scrollY > maxScroll) m_scrollY = maxScroll;
        Invalidate();
        return 0;
    }
    case WM_LBUTTONUP:
        OnLButtonUp(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        return 0;
    case WM_COPY:
    case WM_CUT: {
        if (m_selAnchor != m_caret) {
            int start = m_selAnchor < m_caret ? m_selAnchor : m_caret;
            int len   = m_selAnchor > m_caret ? (m_selAnchor - start) : (m_caret - start);
            std::wstring sel = m_text.substr(start, len);
            if (OpenClipboard(hwnd)) {
                EmptyClipboard();
                HGLOBAL hg = GlobalAlloc(GMEM_MOVEABLE, (sel.size() + 1) * sizeof(wchar_t));
                if (hg) {
                    memcpy(GlobalLock(hg), sel.c_str(), (sel.size() + 1) * sizeof(wchar_t));
                    GlobalUnlock(hg);
                    SetClipboardData(CF_UNICODETEXT, hg);
                }
                CloseClipboard();
            }
            if (msg == WM_CUT) {
                m_text.erase(start, len);
                m_caret = m_selAnchor = start;
                if (m_layoutText) { m_layoutText->Release(); m_layoutText = nullptr; }
                UpdateImeCompositionWindow();
                Invalidate();
                if (m_onTextChange) m_onTextChange();
            }
        }
        return 0;
    }
    case WM_PASTE: {
        UINT pngFormat = RegisterClipboardFormatW(L"PNG");
        if (m_pasteImage &&
            (IsClipboardFormatAvailable(CF_BITMAP) ||
             IsClipboardFormatAvailable(CF_DIB) ||
             IsClipboardFormatAvailable(CF_HDROP) ||
             (pngFormat != 0 && IsClipboardFormatAvailable(pngFormat)))) {
            if (m_pasteImage()) return 0;
        }
        if (OpenClipboard(hwnd)) {
            HANDLE hg = GetClipboardData(CF_UNICODETEXT);
            if (hg) {
                const wchar_t* clip = (const wchar_t*)GlobalLock(hg);
                if (clip) {
                    std::wstring pasted(clip);
                    GlobalUnlock(hg);
                    // sanitize: remove \r
                    pasted.erase(std::remove(pasted.begin(), pasted.end(), L'\r'), pasted.end());
                    
                    if (m_selAnchor != m_caret) {
                        int start = m_selAnchor < m_caret ? m_selAnchor : m_caret;
                        int len   = m_selAnchor > m_caret ? (m_selAnchor - start) : (m_caret - start);
                        m_text.erase(start, len);
                        m_caret = m_selAnchor = start;
                    }
                    m_text.insert(m_caret, pasted);
                    m_caret += (int)pasted.size();
                    m_selAnchor = m_caret;
                    if (m_layoutText) { m_layoutText->Release(); m_layoutText = nullptr; }
                    UpdateImeCompositionWindow();
                    Invalidate();
                    if (m_onTextChange) m_onTextChange();
                }
            }
            CloseClipboard();
        }
        return 0;
    }
    case WM_DROPFILES: {
        HDROP drop = reinterpret_cast<HDROP>(wp);
        std::vector<std::wstring> paths;
        UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
        for (UINT i = 0; i < count; ++i) {
            UINT len = DragQueryFileW(drop, i, nullptr, 0);
            if (len == 0) continue;
            std::wstring path(len + 1, L'\0');
            DragQueryFileW(drop, i, path.data(), len + 1);
            path.resize(len);
            paths.push_back(std::move(path));
        }
        DragFinish(drop);
        if (!paths.empty() && m_filesDropped) m_filesDropped(paths);
        return 0;
    }
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) {
            POINT pt{};
            GetCursorPos(&pt);
            ScreenToClient(hwnd, &pt);
            auto inside = [&](const D2D1_RECT_F& r) {
                return r.right > r.left && pt.x >= r.left && pt.x <= r.right &&
                       pt.y >= r.top && pt.y <= r.bottom;
            };
            SetCursor(LoadCursor(nullptr,
                (inside(m_attachRect) ||
                 inside(m_captureRect) ||
                 inside(m_loopRect) ||
                 inside(m_claudeRect) ||
                 inside(m_geminiRect) ||
                 inside(m_codexRect)) ? IDC_HAND : IDC_IBEAM));
            return TRUE;
        }
        break;
    case WM_SETFOCUS:
        m_focused = true;
        m_caretVisible = true;
        SetTimer(hwnd, kCaretTimerId, kCaretInterval, nullptr);
        UpdateImeCompositionWindow();
        Invalidate();
        return 0;
    case WM_KILLFOCUS:
        m_focused = false;
        m_caretVisible = false;
        KillTimer(hwnd, kCaretTimerId);
        Invalidate();
        return 0;
    case WM_TIMER:
        if (wp == kCaretTimerId) { OnTimerCaret(); return 0; }
        if (wp == kProviderClickTimerId) {
            KillTimer(hwnd, kProviderClickTimerId);
            if (m_pendingProviderClick) {
                m_pendingProviderClick = false;
                ToggleProviderAt((float)m_pendingProviderPoint.x, (float)m_pendingProviderPoint.y);
            }
            return 0;
        }
        break;
    case WM_IME_STARTCOMPOSITION:
    case WM_IME_COMPOSITION:
        UpdateImeCompositionWindow();
        break;
    case WM_CHAR:
    case WM_IME_CHAR:  // 한글 IME 완성 글자도 같이 받음
        OnChar((wchar_t)wp);
        return 0;
    case WM_KEYDOWN:
        OnKeyDown(wp);
        return 0;
    case WM_GETDLGCODE:
        // 방향키·Enter·Tab 모두 본 컨트롤이 처리하게 — 다이얼로그 결 안 잡히게.
        return DLGC_WANTALLKEYS | DLGC_WANTCHARS | DLGC_WANTARROWS;
    case WM_DESTROY:
        KillTimer(hwnd, kCaretTimerId);
        KillTimer(hwnd, kProviderClickTimerId);
        DiscardDeviceResources();
        if (m_layoutText) { m_layoutText->Release(); m_layoutText = nullptr; }
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

inline bool COrangeInput::Create(HWND parent, HINSTANCE hInst) {
    if (!EnsureClass(hInst)) return false;
    m_hwnd = CreateWindowExW(
        0, kClassName, L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP,
        0, 0, 0, 0,
        parent, nullptr, hInst, this);
    if (m_hwnd) DragAcceptFiles(m_hwnd, TRUE);
    return m_hwnd != nullptr;
}

inline void COrangeInput::Layout(int x, int y, int w, int h) {
    if (!m_hwnd) return;
    MoveWindow(m_hwnd, x, y, w, h, TRUE);
}

inline void COrangeInput::SetText(const std::wstring& s) {
    m_text  = s;
    m_caret = (int)m_text.size();
    m_selAnchor = m_caret;
    if (m_layoutText) { m_layoutText->Release(); m_layoutText = nullptr; }
    UpdateImeCompositionWindow();
    Invalidate();
}

inline bool COrangeInput::EnsureFactories() {
    if (!m_d2dFactory) {
        if (FAILED(D2D1CreateFactory(
                D2D1_FACTORY_TYPE_SINGLE_THREADED,
                __uuidof(ID2D1Factory),
                reinterpret_cast<void**>(&m_d2dFactory)))) return false;
    }
    if (!m_dwFactory) {
        if (FAILED(DWriteCreateFactory(
                DWRITE_FACTORY_TYPE_SHARED,
                __uuidof(IDWriteFactory),
                reinterpret_cast<IUnknown**>(&m_dwFactory)))) return false;
    }
    if (!m_tfText) {
        m_dwFactory->CreateTextFormat(
            L"Segoe UI", nullptr,
            DWRITE_FONT_WEIGHT_REGULAR, DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL, kFontSize, L"ko-KR", &m_tfText);
        if (m_tfText) m_tfText->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
    }
    if (!m_tfPlaceholder) {
        m_dwFactory->CreateTextFormat(
            L"Segoe UI", nullptr,
            DWRITE_FONT_WEIGHT_REGULAR, DWRITE_FONT_STYLE_ITALIC,
            DWRITE_FONT_STRETCH_NORMAL, kFontSize, L"ko-KR", &m_tfPlaceholder);
        if (m_tfPlaceholder) m_tfPlaceholder->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
    }
    if (!m_tfIcon) {
        m_dwFactory->CreateTextFormat(
            L"Segoe MDL2 Assets", nullptr,
            DWRITE_FONT_WEIGHT_REGULAR, DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL, 16.0f, L"en-US", &m_tfIcon);
        if (m_tfIcon) {
            m_tfIcon->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            m_tfIcon->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        }
    }
    return m_d2dFactory && m_dwFactory && m_tfText && m_tfPlaceholder && m_tfIcon;
}

inline bool COrangeInput::EnsureDeviceResources() {
    if (m_rt) return true;
    if (!m_hwnd) return false;
    if (!EnsureFactories()) return false;
    RECT rc;
    GetClientRect(m_hwnd, &rc);
    if (rc.right - rc.left <= 0 || rc.bottom - rc.top <= 0) return false;
    D2D1_SIZE_U size = D2D1::SizeU(rc.right - rc.left, rc.bottom - rc.top);
    if (FAILED(m_d2dFactory->CreateHwndRenderTarget(
            D2D1::RenderTargetProperties(),
            D2D1::HwndRenderTargetProperties(m_hwnd, size),
            &m_rt))) return false;

    // OrangeView 카드 결과 일관 — 베이지 톤 + 둥근 모서리.
    m_rt->CreateSolidColorBrush(D2D1::ColorF(0xFDFBF7, 1.0f), &m_bgBrush);       // 조금 더 밝은 입력란 배경
    m_rt->CreateSolidColorBrush(D2D1::ColorF(0xE0D8C8, 1.0f), &m_borderBrush);
    m_rt->CreateSolidColorBrush(D2D1::ColorF(0xFF8800, 1.0f), &m_borderFocusBrush);
    m_rt->CreateSolidColorBrush(D2D1::ColorF(0x2E2A26, 1.0f), &m_textBrush);
    m_rt->CreateSolidColorBrush(D2D1::ColorF(0x8F857A, 1.0f), &m_placeholderBrush);
    m_rt->CreateSolidColorBrush(D2D1::ColorF(0xFF8800, 1.0f), &m_caretBrush);
    m_rt->CreateSolidColorBrush(D2D1::ColorF(0xFF8800, 0.2f), &m_selectionBrush);
    return true;
}

inline float COrangeInput::TextLayoutWidth(float width) const {
    float reserved = (width >= 300.0f) ? kProviderAreaW : 0.0f;
    float w = width - 2.0f * kPaddingX - kAttachButtonW - reserved;
    if (w < 1.0f) w = 1.0f;
    return w;
}

inline void COrangeInput::DiscardDeviceResources() {
    auto rel = [](auto*& p) { if (p) { p->Release(); p = nullptr; } };
    rel(m_selectionBrush);
    rel(m_caretBrush);
    rel(m_placeholderBrush);
    rel(m_textBrush);
    rel(m_borderFocusBrush);
    rel(m_borderBrush);
    rel(m_bgBrush);
    rel(m_rt);
}

inline void COrangeInput::RebuildLayout() {
    if (m_layoutText) { m_layoutText->Release(); m_layoutText = nullptr; }
    if (!EnsureFactories()) return;
    if (m_text.empty()) return;
    RECT rc;
    GetClientRect(m_hwnd, &rc);
    float w = TextLayoutWidth((float)(rc.right - rc.left));
    float h = 10000.0f;
    if (w < 1.0f) w = 1.0f;
    m_dwFactory->CreateTextLayout(
        m_text.c_str(), (UINT32)m_text.size(),
        m_tfText, w, h, &m_layoutText);
}

inline void COrangeInput::OnPaint() {
    if (!EnsureDeviceResources()) return;

    if (!m_layoutText && !m_text.empty()) RebuildLayout();

    m_rt->BeginDraw();
    m_rt->Clear(D2D1::ColorF(0xF7F2EE, 1.0f));  // 부모(OrangeView) 결의 배경 (Titanium Beige)

    RECT rc;
    GetClientRect(m_hwnd, &rc);
    const float W = (float)(rc.right - rc.left);
    const float H = (float)(rc.bottom - rc.top);

    // 둥근 카드 박스 (배경 + 테두리)
    D2D1_ROUNDED_RECT box = D2D1::RoundedRect(
        D2D1::RectF(0.5f, 0.5f, W - 0.5f, H - 0.5f), kCornerR, kCornerR);
    m_rt->FillRoundedRectangle(box, m_bgBrush);
    m_rt->DrawRoundedRectangle(box,
        m_focused ? m_borderFocusBrush : m_borderBrush, m_focused ? 1.5f : 1.0f);
    DrawAttachButton(W, H);
    DrawCaptureButton(W, H);
    DrawLoopButton(W, H);
    DrawProviderPills(W, H);

    // 텍스트 (또는 placeholder) 그리기
    const float textTop = kPaddingY - m_scrollY;
    if (m_text.empty()) {
        D2D1_RECT_F r = D2D1::RectF(TextOriginX(), textTop, TextOriginX() + TextLayoutWidth(W), H - kPaddingY);
        m_rt->DrawText(m_placeholder.c_str(), (UINT32)m_placeholder.size(),
                       m_tfPlaceholder, r, m_placeholderBrush);
    } else if (m_layoutText) {
        // 선택 영역 그리기
        if (m_selAnchor != m_caret && m_selectionBrush) {
            int start = m_selAnchor < m_caret ? m_selAnchor : m_caret;
            int len   = m_selAnchor > m_caret ? (m_selAnchor - start) : (m_caret - start);
            DWRITE_HIT_TEST_METRICS htm[16];
            UINT32 actual = 0;
            HRESULT hr = m_layoutText->HitTestTextRange(
                start, len, TextOriginX(), textTop, htm, 16, &actual);
            if (SUCCEEDED(hr)) {
                for (UINT32 i = 0; i < actual; ++i) {
                    D2D1_RECT_F sr = D2D1::RectF(
                        htm[i].left, htm[i].top,
                        htm[i].left + htm[i].width, htm[i].top + htm[i].height);
                    m_rt->FillRectangle(sr, m_selectionBrush);
                }
            }
        }

        m_rt->DrawTextLayout(D2D1::Point2F(TextOriginX(), textTop),
                             m_layoutText, m_textBrush);
    }

    // caret
    if (m_focused && m_caretVisible) {
        float caretX = TextOriginX();
        float caretY = textTop + 2.0f;
        if (m_layoutText && m_caret > 0) {
            // m_caret 위치의 (x, y) 측정 — HitTestTextPosition.
            DWRITE_HIT_TEST_METRICS htm{};
            float pointX = 0.0f, pointY = 0.0f;
            HRESULT hr = m_layoutText->HitTestTextPosition(
                (UINT32)m_caret, FALSE, &pointX, &pointY, &htm);
            if (SUCCEEDED(hr)) {
                caretX = TextOriginX() + pointX;
                caretY = textTop + pointY + 2.0f;
            }
        }
        const float caretH = kFontSize * 1.2f;
        D2D1_RECT_F caretR = D2D1::RectF(
            caretX, caretY, caretX + 1.5f, caretY + caretH);
        m_rt->FillRectangle(caretR, m_caretBrush);
    }

    HRESULT hr = m_rt->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET) {
        DiscardDeviceResources();
    }
}

inline void COrangeInput::DrawProviderPills(float W, float H) {
    if (!m_rt || W < 300.0f) {
        m_claudeRect = {};
        m_geminiRect = {};
        m_codexRect = {};
        return;
    }

    const float pillH = 26.0f;
    const float pillY = (H - pillH) * 0.5f;
    const float gap = 6.0f;
    const float wClaude = 64.0f;
    const float wGemini = 68.0f;
    const float wCodex = 60.0f;
    const float right = W - 12.0f;
    m_codexRect = D2D1::RectF(right - wCodex, pillY, right, pillY + pillH);
    m_geminiRect = D2D1::RectF(m_codexRect.left - gap - wGemini, pillY,
                               m_codexRect.left - gap, pillY + pillH);
    m_claudeRect = D2D1::RectF(m_geminiRect.left - gap - wClaude, pillY,
                               m_geminiRect.left - gap, pillY + pillH);

    // Provider별 브랜드 색상 — 카드 stripe 색과 통일
    struct ProviderColors {
        D2D1::ColorF fill;
        D2D1::ColorF border;
    };
    // 비활성: 흰 배경 + 옅은 테두리 / 활성: provider 고유 색 채움
    auto makeColors = [](UINT32 hex) -> ProviderColors {
        return {
            D2D1::ColorF(hex, 1.0f),
            D2D1::ColorF(                                              // border = fill 20% darker
                ((hex >> 16) & 0xFF) / 255.0f * 0.80f,
                ((hex >> 8)  & 0xFF) / 255.0f * 0.80f,
                ( hex        & 0xFF) / 255.0f * 0.80f, 1.0f)
        };
    };
    ProviderColors colClaude = makeColors(0xFF8800);  // 오렌지
    ProviderColors colGemini = makeColors(0x1A73E8);  // Google Blue
    ProviderColors colCodex  = makeColors(0x10A37F);  // OpenAI Emerald

    ID2D1SolidColorBrush* inactiveFill   = nullptr;
    ID2D1SolidColorBrush* inactiveBorder = nullptr;
    ID2D1SolidColorBrush* activeText     = nullptr;
    ID2D1SolidColorBrush* inactiveText   = nullptr;
    m_rt->CreateSolidColorBrush(D2D1::ColorF(0xFFFFFF, 0.0f), &inactiveFill);
    m_rt->CreateSolidColorBrush(D2D1::ColorF(0xD8D1C8, 1.0f), &inactiveBorder);
    m_rt->CreateSolidColorBrush(D2D1::ColorF(0xFFFFFF, 1.0f), &activeText);
    m_rt->CreateSolidColorBrush(D2D1::ColorF(0x6E675F, 1.0f), &inactiveText);

    auto drawOne = [&](const wchar_t* label, const D2D1_RECT_F& rect,
                       bool active, bool manager, const ProviderColors& pc) {
        D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(rect, 7.0f, 7.0f);

        ID2D1SolidColorBrush* fillB   = nullptr;
        ID2D1SolidColorBrush* borderB = nullptr;
        if (active || manager) {
            m_rt->CreateSolidColorBrush(pc.fill,   &fillB);
            m_rt->CreateSolidColorBrush(pc.border, &borderB);
        }

        m_rt->FillRoundedRectangle(rr, (active || manager) ? fillB : inactiveFill);
        m_rt->DrawRoundedRectangle(rr, (active || manager) ? borderB : inactiveBorder, 1.0f);

        if (fillB)   fillB->Release();
        if (borderB) borderB->Release();

        IDWriteTextLayout* tl = nullptr;
        UINT32 len = (UINT32)wcslen(label);
        if (m_dwFactory && m_tfText &&
            SUCCEEDED(m_dwFactory->CreateTextLayout(label, len, m_tfText,
                                                    rect.right - rect.left,
                                                    rect.bottom - rect.top, &tl)) && tl) {
            DWRITE_TEXT_RANGE all{0, len};
            tl->SetFontSize(11.0f, all);
            tl->SetFontWeight(DWRITE_FONT_WEIGHT_SEMI_BOLD, all);
            tl->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            tl->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            m_rt->DrawTextLayout(D2D1::Point2F(rect.left, rect.top), tl,
                                 (active || manager) ? activeText : inactiveText);
            tl->Release();
        }
    };

    drawOne(L"Claude", m_claudeRect, m_useClaude, m_managerProvider == "claude", colClaude);
    drawOne(L"Gemini", m_geminiRect, m_useGemini, m_managerProvider == "gemini", colGemini);
    drawOne(L"Codex",  m_codexRect,  m_useCodex,  m_managerProvider == "codex",  colCodex);

    if (activeText)     activeText->Release();
    if (inactiveText)   inactiveText->Release();
    if (inactiveBorder) inactiveBorder->Release();
    if (inactiveFill)   inactiveFill->Release();
}

inline void COrangeInput::DrawAttachButton(float W, float H) {
    if (!m_rt) return;
    const float size = 28.0f;
    const float x = 8.0f;
    const float y = (H - size) * 0.5f;
    m_attachRect = D2D1::RectF(x, y, x + size, y + size);

    ID2D1SolidColorBrush* bg = nullptr;
    ID2D1SolidColorBrush* stroke = nullptr;
    m_rt->CreateSolidColorBrush(D2D1::ColorF(0xFFFFFF, 0.0f), &bg);
    m_rt->CreateSolidColorBrush(D2D1::ColorF(0x8F857A, 1.0f), &stroke);
    if (bg && stroke) {
        D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(m_attachRect, 7.0f, 7.0f);
        m_rt->FillRoundedRectangle(rr, bg);
        m_rt->DrawRoundedRectangle(rr, stroke, 1.0f);

        const wchar_t attachIcon[] = L"\xE723"; // Segoe MDL2 Assets: Attach
        m_rt->DrawTextW(attachIcon, 1, m_tfIcon, m_attachRect, stroke,
                        D2D1_DRAW_TEXT_OPTIONS_NONE, DWRITE_MEASURING_MODE_NATURAL);
    }
    if (stroke) stroke->Release();
    if (bg) bg->Release();
}

inline void COrangeInput::DrawCaptureButton(float W, float H) {
    if (!m_rt) return;
    const float size = 28.0f;
    const float x = 40.0f;
    const float y = (H - size) * 0.5f;
    m_captureRect = D2D1::RectF(x, y, x + size, y + size);

    ID2D1SolidColorBrush* bg = nullptr;
    ID2D1SolidColorBrush* stroke = nullptr;
    m_rt->CreateSolidColorBrush(D2D1::ColorF(0xFFFFFF, 0.0f), &bg);
    m_rt->CreateSolidColorBrush(D2D1::ColorF(0x8F857A, 1.0f), &stroke);
    if (bg && stroke) {
        D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(m_captureRect, 7.0f, 7.0f);
        m_rt->FillRoundedRectangle(rr, bg);
        m_rt->DrawRoundedRectangle(rr, stroke, 1.0f);

        const wchar_t cameraIcon[] = L"\xE722"; // Segoe MDL2 Assets: Camera
        m_rt->DrawTextW(cameraIcon, 1, m_tfIcon, m_captureRect, stroke,
                        D2D1_DRAW_TEXT_OPTIONS_NONE, DWRITE_MEASURING_MODE_NATURAL);
    }
    if (stroke) stroke->Release();
    if (bg) bg->Release();
}

inline void COrangeInput::DrawLoopButton(float W, float H) {
    if (!m_rt) return;
    const float size = 28.0f;
    const float x = 72.0f;
    const float y = (H - size) * 0.5f;
    m_loopRect = D2D1::RectF(x, y, x + size, y + size);

    ID2D1SolidColorBrush* bg     = nullptr;
    ID2D1SolidColorBrush* stroke = nullptr;
    if (m_autoLoopActive) {
        m_rt->CreateSolidColorBrush(D2D1::ColorF(0xFF8800, 1.0f), &bg);
        m_rt->CreateSolidColorBrush(D2D1::ColorF(0xCC6600, 1.0f), &stroke);
    } else {
        m_rt->CreateSolidColorBrush(D2D1::ColorF(0xFFFFFF, 0.0f), &bg);
        m_rt->CreateSolidColorBrush(D2D1::ColorF(0x8F857A, 1.0f), &stroke);
    }
    if (bg && stroke) {
        D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(m_loopRect, 7.0f, 7.0f);
        m_rt->FillRoundedRectangle(rr, bg);
        m_rt->DrawRoundedRectangle(rr, stroke, 1.0f);

        ID2D1SolidColorBrush* iconColor = nullptr;
        m_rt->CreateSolidColorBrush(
            m_autoLoopActive ? D2D1::ColorF(0xFFFFFF, 1.0f) : D2D1::ColorF(0x8F857A, 1.0f),
            &iconColor);
        const wchar_t loopIcon[] = L"\xE72C";  // Segoe MDL2: Refresh
        if (iconColor) {
            m_rt->DrawTextW(loopIcon, 1, m_tfIcon, m_loopRect, iconColor,
                            D2D1_DRAW_TEXT_OPTIONS_NONE, DWRITE_MEASURING_MODE_NATURAL);
            iconColor->Release();
        }
    }
    if (stroke) stroke->Release();
    if (bg) bg->Release();
}

inline bool COrangeInput::HitAttachButton(float x, float y) const {
    return m_attachRect.right > m_attachRect.left &&
           x >= m_attachRect.left && x <= m_attachRect.right &&
           y >= m_attachRect.top && y <= m_attachRect.bottom;
}

inline bool COrangeInput::HitCaptureButton(float x, float y) const {
    return m_captureRect.right > m_captureRect.left &&
           x >= m_captureRect.left && x <= m_captureRect.right &&
           y >= m_captureRect.top && y <= m_captureRect.bottom;
}

inline bool COrangeInput::HitProviderButton(float x, float y) const {
    auto inside = [&](const D2D1_RECT_F& r) {
        return r.right > r.left && x >= r.left && x <= r.right && y >= r.top && y <= r.bottom;
    };
    return inside(m_claudeRect) || inside(m_geminiRect) || inside(m_codexRect);
}

inline bool COrangeInput::ToggleProviderAt(float x, float y) {
    auto inside = [&](const D2D1_RECT_F& r) {
        return r.right > r.left && x >= r.left && x <= r.right && y >= r.top && y <= r.bottom;
    };
    bool changed = false;
    if (inside(m_claudeRect)) {
        m_useClaude = !m_useClaude;
        changed = true;
    } else if (inside(m_geminiRect)) {
        m_useGemini = !m_useGemini;
        changed = true;
    } else if (inside(m_codexRect)) {
        m_useCodex = !m_useCodex;
        changed = true;
    }
    if (!m_useClaude && !m_useGemini && !m_useCodex) {
        m_useClaude = true;
    }
    NormalizeManagerProvider();
    if (changed) {
        if (m_providerToggle) m_providerToggle(m_useClaude, m_useGemini, m_useCodex);
        Invalidate();
    }
    return changed;
}

inline bool COrangeInput::SetManagerProviderAt(float x, float y) {
    auto inside = [&](const D2D1_RECT_F& r) {
        return r.right > r.left && x >= r.left && x <= r.right && y >= r.top && y <= r.bottom;
    };
    std::string provider;
    if (inside(m_claudeRect)) {
        provider = "claude";
        m_useClaude = true;
    } else if (inside(m_geminiRect)) {
        provider = "gemini";
        m_useGemini = true;
    } else if (inside(m_codexRect)) {
        provider = "codex";
        m_useCodex = true;
    } else {
        return false;
    }
    m_managerProvider = provider;
    if (m_providerToggle) m_providerToggle(m_useClaude, m_useGemini, m_useCodex);
    if (m_managerProviderChanged) m_managerProviderChanged(m_managerProvider);
    Invalidate();
    return true;
}

inline void COrangeInput::NormalizeManagerProvider() {
    auto enabled = [&](const std::string& p) {
        return (p == "claude" && m_useClaude) ||
               (p == "gemini" && m_useGemini) ||
               (p == "codex" && m_useCodex);
    };
    if (enabled(m_managerProvider)) return;
    if (m_useClaude) m_managerProvider = "claude";
    else if (m_useGemini) m_managerProvider = "gemini";
    else if (m_useCodex) m_managerProvider = "codex";
    else {
        m_useClaude = true;
        m_managerProvider = "claude";
    }
}

inline POINT COrangeInput::CaretPoint() const {
    POINT pt{ (LONG)TextOriginX(), (LONG)(kPaddingY + 2.0f) };
    if (m_layoutText && m_caret > 0) {
        DWRITE_HIT_TEST_METRICS htm{};
        float pointX = 0.0f, pointY = 0.0f;
        HRESULT hr = m_layoutText->HitTestTextPosition(
            (UINT32)m_caret, FALSE, &pointX, &pointY, &htm);
        if (SUCCEEDED(hr)) {
            pt.x = (LONG)(TextOriginX() + pointX);
            pt.y = (LONG)(kPaddingY + pointY - m_scrollY + 2.0f);
        }
    }
    return pt;
}

inline void COrangeInput::EnsureCaretVisible() {
    if (!m_hwnd) return;
    if (!m_layoutText && !m_text.empty()) RebuildLayout();
    RECT rc;
    GetClientRect(m_hwnd, &rc);
    const float viewportTop = 0.0f;
    const float viewportBottom = (float)(rc.bottom - rc.top) - 2.0f * kPaddingY;
    if (viewportBottom <= 1.0f) {
        m_scrollY = 0.0f;
        return;
    }

    float contentH = 0.0f;
    if (m_layoutText) {
        DWRITE_TEXT_METRICS tm{};
        m_layoutText->GetMetrics(&tm);
        contentH = tm.height;
    }
    const float maxScroll = contentH > viewportBottom ? (contentH - viewportBottom) : 0.0f;

    float pointY = 0.0f;
    if (m_layoutText && m_caret > 0) {
        DWRITE_HIT_TEST_METRICS htm{};
        float pointX = 0.0f;
        if (SUCCEEDED(m_layoutText->HitTestTextPosition((UINT32)m_caret, FALSE, &pointX, &pointY, &htm))) {
            const float caretBottom = pointY + htm.height;
            if (caretBottom - m_scrollY > viewportBottom) {
                m_scrollY = caretBottom - viewportBottom;
            } else if (pointY - m_scrollY < viewportTop) {
                m_scrollY = pointY;
            }
        }
    } else {
        m_scrollY = 0.0f;
    }

    if (m_scrollY < 0.0f) m_scrollY = 0.0f;
    if (m_scrollY > maxScroll) m_scrollY = maxScroll;
}

inline void COrangeInput::UpdateImeCompositionWindow() {
    if (!m_hwnd) return;
    if (!m_layoutText && !m_text.empty()) RebuildLayout();
    EnsureCaretVisible();

    HIMC himc = ImmGetContext(m_hwnd);
    if (!himc) return;

    COMPOSITIONFORM cf{};
    cf.dwStyle = CFS_POINT;
    cf.ptCurrentPos = CaretPoint();
    ImmSetCompositionWindow(himc, &cf);

    CANDIDATEFORM cand{};
    cand.dwIndex = 0;
    cand.dwStyle = CFS_CANDIDATEPOS;
    cand.ptCurrentPos = cf.ptCurrentPos;
    ImmSetCandidateWindow(himc, &cand);

    ImmReleaseContext(m_hwnd, himc);
}

inline void COrangeInput::InsertMarkdown(const std::wstring& prefix, const std::wstring& suffix) {
    if (m_caret < 0) m_caret = 0;
    if (m_caret > (int)m_text.size()) m_caret = (int)m_text.size();

    int start = m_selAnchor < m_caret ? m_selAnchor : m_caret;
    int len   = m_selAnchor > m_caret ? (m_selAnchor - start) : (m_caret - start);
    std::wstring selText = m_text.substr(start, len);

    m_text.erase((size_t)start, len);
    std::wstring inserted = prefix + selText + suffix;
    m_text.insert((size_t)start, inserted);

    m_caret = start + (int)prefix.size() + (int)selText.size(); 
    m_selAnchor = m_caret; // 선택 해제

    if (m_layoutText) { m_layoutText->Release(); m_layoutText = nullptr; }
    m_caretVisible = true;
    Invalidate();
    UpdateImeCompositionWindow();
    if (m_onTextChange) m_onTextChange();
    if (m_hwnd) SetFocus(m_hwnd);
}


inline void COrangeInput::OnChar(wchar_t ch) {
    // 컨트롤 문자 차단 (Backspace·Enter 등은 WM_KEYDOWN 에서 처리). LF(\n)는 허용.
    if (ch < 0x20 && ch != L'\n') return;

    if (m_selAnchor != m_caret) {
        int start = m_selAnchor < m_caret ? m_selAnchor : m_caret;
        int len   = m_selAnchor > m_caret ? (m_selAnchor - start) : (m_caret - start);
        m_text.erase((size_t)start, len);
        m_caret = m_selAnchor = start;
    }

    if (m_caret < 0) m_caret = 0;
    if (m_caret > (int)m_text.size()) m_caret = (int)m_text.size();
    m_text.insert((size_t)m_caret, 1, ch);
    ++m_caret;
    m_selAnchor = m_caret;

    if (m_layoutText) { m_layoutText->Release(); m_layoutText = nullptr; }
    Invalidate();
    UpdateImeCompositionWindow();
    if (m_onTextChange) m_onTextChange();
}

inline void COrangeInput::OnKeyDown(WPARAM vk) {
    bool textChanged = false;
    bool caretChanged = false;
    bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;

    auto deleteSelection = [&]() {
        if (m_selAnchor == m_caret) return false;
        int start = m_selAnchor < m_caret ? m_selAnchor : m_caret;
        int len   = m_selAnchor > m_caret ? (m_selAnchor - start) : (m_caret - start);
        m_text.erase((size_t)start, len);
        m_caret = m_selAnchor = start;
        return true;
    };

    switch (vk) {
    case VK_LEFT:
        if (m_caret > 0) { --m_caret; caretChanged = true; }
        if (!shift) m_selAnchor = m_caret;
        break;
    case VK_RIGHT:
        if (m_caret < (int)m_text.size()) { ++m_caret; caretChanged = true; }
        if (!shift) m_selAnchor = m_caret;
        break;
    case VK_HOME:
        m_caret = 0; caretChanged = true;
        if (!shift) m_selAnchor = m_caret;
        break;
    case VK_END:
        m_caret = (int)m_text.size(); caretChanged = true;
        if (!shift) m_selAnchor = m_caret;
        break;
    case VK_BACK:
        if (deleteSelection()) {
            textChanged = true;
        } else if (m_caret > 0) {
            m_text.erase((size_t)(m_caret - 1), 1);
            --m_caret;
            m_selAnchor = m_caret;
            textChanged = true;
        }
        break;
    case VK_DELETE:
        if (deleteSelection()) {
            textChanged = true;
        } else if (m_caret < (int)m_text.size()) {
            m_text.erase((size_t)m_caret, 1);
            textChanged = true;
        }
        break;
    case VK_RETURN:
        if (shift) {
            // Shift+Enter: 줄바꿈 삽입
            deleteSelection();
            m_text.insert((size_t)m_caret, 1, L'\n');
            ++m_caret;
            m_selAnchor = m_caret;
            textChanged = true;
        } else {
            // Enter = 즉시 전송.
            if (!m_text.empty() && m_submit) {
                m_submit(m_text);
            }
        }
        return;
    case 'A':
        if ((GetKeyState(VK_CONTROL) & 0x8000) != 0) {
            m_selAnchor = 0;
            m_caret = (int)m_text.size();
            caretChanged = true;
            return; // textChanged = false
        }
        break;
    case 'C':
        if ((GetKeyState(VK_CONTROL) & 0x8000) != 0) {
            if (m_hwnd) SendMessage(m_hwnd, WM_COPY, 0, 0);
            return;
        }
        break;
    case 'X':
        if ((GetKeyState(VK_CONTROL) & 0x8000) != 0) {
            if (m_hwnd) SendMessage(m_hwnd, WM_CUT, 0, 0);
            return;
        }
        break;
    case 'V':
        if ((GetKeyState(VK_CONTROL) & 0x8000) != 0) {
            if (m_hwnd) SendMessage(m_hwnd, WM_PASTE, 0, 0);
            return;
        }
        break;
    default:
        return;
    }
    if (textChanged) {
        if (m_layoutText) { m_layoutText->Release(); m_layoutText = nullptr; }
    }
    if (textChanged || caretChanged) {
        m_caretVisible = true;  // 입력 직후 caret 보이게
        UpdateImeCompositionWindow();
        Invalidate();
        if (textChanged && m_onTextChange) m_onTextChange();
    }
}

inline void COrangeInput::OnLButtonDown(int x, int y) {
    if (!EnsureDeviceResources()) return;
    if (!m_layoutText && !m_text.empty()) RebuildLayout();
    if (HitAttachButton((float)x, (float)y)) {
        m_dragging = false;
        if (m_attachFile) m_attachFile();
        return;
    }
    if (HitCaptureButton((float)x, (float)y)) {
        m_dragging = false;
        if (m_capture) m_capture();
        return;
    }
    if (m_loopRect.right > m_loopRect.left &&
        x >= m_loopRect.left && x <= m_loopRect.right &&
        y >= m_loopRect.top  && y <= m_loopRect.bottom) {
        m_dragging = false;
        if (m_loopToggle) m_loopToggle();
        return;
    }
    if (HitProviderButton((float)x, (float)y)) {
        m_dragging = false;
        m_pendingProviderClick = true;
        m_pendingProviderPoint = POINT{x, y};
        SetTimer(m_hwnd, kProviderClickTimerId, GetDoubleClickTime() + 20, nullptr);
        return;
    }

    if (!m_layoutText) {
        m_caret = 0;
    } else {
        BOOL trailing = FALSE, inside = FALSE;
        DWRITE_HIT_TEST_METRICS htm{};
        float relX = (float)x - TextOriginX();
        float relY = (float)y - kPaddingY + m_scrollY;
        if (relX < 0.0f) relX = 0.0f;
        if (relY < 0.0f) relY = 0.0f;
        HRESULT hr = m_layoutText->HitTestPoint(relX, relY, &trailing, &inside, &htm);
        if (SUCCEEDED(hr)) {
            m_caret = (int)htm.textPosition + (trailing ? (int)htm.length : 0);
            if (m_caret > (int)m_text.size()) m_caret = (int)m_text.size();
        }
    }
    
    bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    if (!shift) m_selAnchor = m_caret;

    m_dragging = true;
    SetCapture(m_hwnd);
    m_caretVisible = true;
    UpdateImeCompositionWindow();
    Invalidate();
}

inline void COrangeInput::OnMouseMove(int x, int y) {
    if (!m_dragging || !m_layoutText) return;
    BOOL trailing = FALSE, inside = FALSE;
    DWRITE_HIT_TEST_METRICS htm{};
    float relX = (float)x - TextOriginX();
    float relY = (float)y - kPaddingY + m_scrollY;
    if (relX < 0.0f) relX = 0.0f;
    if (relY < 0.0f) relY = 0.0f;
    HRESULT hr = m_layoutText->HitTestPoint(relX, relY, &trailing, &inside, &htm);
    if (SUCCEEDED(hr)) {
        int newCaret = (int)htm.textPosition + (trailing ? (int)htm.length : 0);
        if (newCaret > (int)m_text.size()) newCaret = (int)m_text.size();
        if (m_caret != newCaret) {
            m_caret = newCaret;
            m_caretVisible = true;
            UpdateImeCompositionWindow();
            Invalidate();
        }
    }
}

inline void COrangeInput::OnLButtonUp(int x, int y) {
    if (m_dragging) {
        m_dragging = false;
        ReleaseCapture();
    }
}

inline void COrangeInput::OnLButtonDblClk(int x, int y) {
    if (HitProviderButton((float)x, (float)y)) {
        KillTimer(m_hwnd, kProviderClickTimerId);
        m_pendingProviderClick = false;
        SetManagerProviderAt((float)x, (float)y);
        m_dragging = false;
        return;
    }
}

inline void COrangeInput::OnTimerCaret() {
    m_caretVisible = !m_caretVisible;
    Invalidate();
}

}  // namespace orange
