#pragma once

// CSidebar — 좌측 세션 패널 (Direct2D + DirectWrite ko-KR).
//
// 활동 패널 (ActivityPanel.h) 과 같은 결로 통일하기 위한 토대입니다.
// 옛 Win32 LISTBOX + LBS_OWNERDRAWFIXED + GDI WM_DRAWITEM 결을 폐기하고,
// 자체 D2D HwndRenderTarget 으로 직접 그립니다.
//
// 단계 1 — 토대:
//   * WNDCLASS 등록 + Win32 자식 윈도우.
//   * D2D Factory + DirectWrite Factory + HwndRenderTarget lifecycle.
//   * 시그니처 (Create / Layout / Refill / SetCurrentChat / Hwnd).
//   * WM_PAINT 는 배경만 칠해두고 행 그리기는 단계 2 에서 추가합니다.
//
// 단계 2 (드로잉), 단계 3 (상호작용), 단계 4 (main.cpp 통합), 단계 5 (옛 결 폐기)
// 는 후속 사이클에서 이어집니다 (tasks/sidebar_d2d_consistency.md).

#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <d2d1.h>
#include <dwrite.h>
#include <wincodec.h>
#include <string>
#include <vector>

#include "Goal.h"  // CWorkStatus, CVerdict
#include "Utils.h" // Utf8ToWide

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "windowscodecs.lib")

namespace orange {

class CSidebar {
public:
    // 사이드바 한 행 데이터 — main.cpp 의 SidebarRow 와 동등한 결.
    // 단계 4 에서 main.cpp 의 g_sidebarRows 가 본 구조로 통일됩니다.
    enum class RowKind { Goal, Project, NewChat, NewGoal, NewProject, Chat, Empty };
    struct Row {
        RowKind       kind = RowKind::Empty;
        int           level = 0;        // 들여쓰기 깊이 (0=goal, 1=project, 2=chat)
        std::wstring  title;
        std::wstring  trailing;         // 우측 시각 (mtime suffix 등)
        CWorkStatus   status = CWorkStatus::Planning;
        CVerdict      verdict = CVerdict::None;
        int           progress = 0;     // 0~100
        bool          isCurrent = false;
        std::string   chatKey;          // chat 행 한정 — 클릭 시 세션 전환 키
        std::string   goalId;           // 자기 id (Goal) 또는 부모 goal id (Project / NewProject)
        std::string   projectId;        // 자기 project id (Project 한정)
        std::string   lastActiveIso;    // Chat 행 마지막 활동 시각 (hover 툴팁)
    };

    CSidebar() = default;
    ~CSidebar() {
        DiscardDeviceResources();
        Release(m_tfBody);
        Release(m_tfDim);
        Release(m_tfHeader);
        Release(m_dwFactory);
        Release(m_d2dFactory);
    }

    // controlId — 부모(main 윈도우) 가 WM_COMMAND/WM_NOTIFY 에서 식별할 자식 ID.
    // 옛 LISTBOX 의 IDC_SIDEBAR 자리 — 단계 4 통합 시 호환을 위한 결.
    bool Create(HWND parent, HINSTANCE hInst, int controlId);

    HWND Hwnd() const { return m_hwnd; }

    // 부모 WM_SIZE 또는 splitter 드래그에서 호출. 절대 좌표/크기.
    void Layout(int x, int y, int w, int h);

    // 행 데이터 갱신 → 다시 그리기. 선택 인덱스는 보존 (범위 밖이면 -1 로 환원).
    void Refill(std::vector<Row> rows);

    // ▶ 강조할 현재 채팅 키 갱신 (행 데이터는 그대로, 강조만 바뀔 때).
    void SetCurrentChat(const std::string& key);

    // 부모 → CSidebar 조회 결 (단계 4 의 main.cpp 통합 자리에서 사용).
    int           SelectedIdx() const { return m_selectedIdx; }
    const Row*    SelectedRow() const {
        if (m_selectedIdx < 0 || m_selectedIdx >= (int)m_rows.size()) return nullptr;
        return &m_rows[m_selectedIdx];
    }
    void          SetSelectedIdx(int idx);
    int           FindChatIndex(const std::string& key,
                                const std::string& goalId = std::string(),
                                const std::string& projectId = std::string()) const;
    int           HoverIdx() const { return m_hoverIdx; }
    int           RowCount() const { return (int)m_rows.size(); }
    const Row*    RowAt(int idx) const {
        if (idx < 0 || idx >= (int)m_rows.size()) return nullptr;
        return &m_rows[idx];
    }

private:
    static LRESULT CALLBACK StaticWndProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT WndProc(HWND, UINT, WPARAM, LPARAM);

    bool EnsureFactories();
    bool EnsureDeviceResources();
    void DiscardDeviceResources();
    void OnPaint();
    void EnsureLogoBitmap();
    void DrawLogo(float width, float height);

    void EnsureToolTip();
    void RelayMouseToToolTip(UINT msg, WPARAM wp, LPARAM lp);
    void UpdateToolTipText();
    int  HitRow(int x, int y) const;
    void SendNotify(WORD code);
    void SetHover(int idx);
    void UpdateScrollInfo();

    static const wchar_t* kClassName;
    static bool s_classRegistered;
    static bool EnsureClass(HINSTANCE hInst);

    template <class T>
    static void Release(T*& p) { if (p) { p->Release(); p = nullptr; } }

    HWND m_hwnd = nullptr;
    int  m_controlId = 0;

    ID2D1Factory*           m_d2dFactory = nullptr;
    IDWriteFactory*         m_dwFactory  = nullptr;
    ID2D1HwndRenderTarget*  m_rt         = nullptr;

    IDWriteTextFormat*      m_tfHeader   = nullptr;
    IDWriteTextFormat*      m_tfBody     = nullptr;
    IDWriteTextFormat*      m_tfDim      = nullptr;

    ID2D1SolidColorBrush*   m_bgBrush         = nullptr;
    ID2D1SolidColorBrush*   m_separatorBrush  = nullptr;
    ID2D1SolidColorBrush*   m_textBrush       = nullptr;
    ID2D1SolidColorBrush*   m_dimBrush        = nullptr;
    ID2D1SolidColorBrush*   m_goalBrush       = nullptr;   // Goal 도트 — 오렌지
    ID2D1SolidColorBrush*   m_projectBrush    = nullptr;   // Project 도트 — 청록
    ID2D1SolidColorBrush*   m_chatBrush       = nullptr;   // Chat 도트 — 베이지
    ID2D1SolidColorBrush*   m_newBrush        = nullptr;   // New* 도트 — 옅은 회갈색
    ID2D1SolidColorBrush*   m_hoverBrush      = nullptr;   // hover 행 배경
    ID2D1SolidColorBrush*   m_currentBrush    = nullptr;   // 현재 채팅 강조 배경
    ID2D1Bitmap*            m_logoBitmap      = nullptr;

    std::vector<Row> m_rows;
    std::string      m_currentKey;  // ▶ 강조할 채팅 키

    // ── 상호작용 자리 (단계 3) ──
    struct RowHit {
        D2D1_RECT_F rect;
        int         idx;
    };
    std::vector<RowHit> m_rowHits;     // 매 OnPaint 에서 채움 (가시 행만)
    int  m_hoverIdx     = -1;
    int  m_selectedIdx  = -1;
    bool m_mouseTracking = false;
    HWND m_hToolTip      = nullptr;

    // ── 스크롤 (Fix: 대표님 피드백 반영) ──
    float m_scrollPos      = 0.0f;
    float m_contentHeight  = 0.0f;
    constexpr static float kRowH = 32.0f; // 높이를 키워 호흡감 확보 (26 -> 32)
    constexpr static float kLogoAreaH = 58.0f;
};

inline const wchar_t* CSidebar::kClassName = L"OrangeSidebar";
inline bool CSidebar::s_classRegistered = false;

inline bool CSidebar::EnsureClass(HINSTANCE hInst) {
    if (s_classRegistered) return true;
    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = StaticWndProc;
    wc.hInstance     = hInst;
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;  // D2D 매 프레임 Clear
    wc.lpszClassName = kClassName;
    if (!RegisterClassExW(&wc)) return false;
    s_classRegistered = true;
    return true;
}

inline LRESULT CALLBACK CSidebar::StaticWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    CSidebar* self = nullptr;
    if (msg == WM_NCCREATE) {
        auto* cs = (CREATESTRUCTW*)lp;
        self = (CSidebar*)cs->lpCreateParams;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)self);
    } else {
        self = (CSidebar*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    }
    if (self) return self->WndProc(hwnd, msg, wp, lp);
    return DefWindowProcW(hwnd, msg, wp, lp);
}

inline LRESULT CSidebar::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
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
        return 0;
    case WM_ERASEBKGND:
        return 1;  // D2D 매 프레임 Clear

    case WM_VSCROLL: {
        SCROLLINFO si{ sizeof(si), SIF_ALL };
        GetScrollInfo(hwnd, SB_VERT, &si);
        int oldPos = si.nPos;
        switch (LOWORD(wp)) {
        case SB_TOP:           si.nPos = si.nMin; break;
        case SB_BOTTOM:        si.nPos = si.nMax; break;
        case SB_LINEUP:        si.nPos -= 1; break;
        case SB_LINEDOWN:      si.nPos += 1; break;
        case SB_PAGEUP:        si.nPos -= si.nPage; break;
        case SB_PAGEDOWN:      si.nPos += si.nPage; break;
        case SB_THUMBTRACK:    si.nPos = si.nTrackPos; break;
        }
        si.fMask = SIF_POS;
        SetScrollInfo(hwnd, SB_VERT, &si, TRUE);
        GetScrollInfo(hwnd, SB_VERT, &si);
        if (si.nPos != oldPos) {
            m_scrollPos = (float)si.nPos * kRowH;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    }
    case WM_MOUSEWHEEL: {
        int delta = GET_WHEEL_DELTA_WPARAM(wp);
        SCROLLINFO si{ sizeof(si), SIF_ALL };
        GetScrollInfo(hwnd, SB_VERT, &si);
        int oldPos = si.nPos;
        si.nPos -= (delta / WHEEL_DELTA) * 3; // 3줄씩 스크롤
        si.fMask = SIF_POS;
        SetScrollInfo(hwnd, SB_VERT, &si, TRUE);
        GetScrollInfo(hwnd, SB_VERT, &si);
        if (si.nPos != oldPos) {
            m_scrollPos = (float)si.nPos * kRowH;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    }

    case WM_MOUSEMOVE: {
        if (!m_mouseTracking) {
            TRACKMOUSEEVENT tme{};
            tme.cbSize    = sizeof(tme);
            tme.dwFlags   = TME_LEAVE;
            tme.hwndTrack = hwnd;
            TrackMouseEvent(&tme);
            m_mouseTracking = true;
        }
        int x = GET_X_LPARAM(lp);
        int y = GET_Y_LPARAM(lp);
        SetHover(HitRow(x, y));
        RelayMouseToToolTip(msg, wp, lp);
        return 0;
    }
    case WM_MOUSELEAVE:
        m_mouseTracking = false;
        SetHover(-1);
        return 0;

    case WM_LBUTTONDOWN: {
        SetFocus(hwnd);
        int x = GET_X_LPARAM(lp);
        int y = GET_Y_LPARAM(lp);
        int idx = HitRow(x, y);
        if (idx >= 0) {
            SetSelectedIdx(idx);
            SendNotify(LBN_SELCHANGE);
        }
        RelayMouseToToolTip(msg, wp, lp);
        return 0;
    }
    case WM_LBUTTONDBLCLK: {
        int x = GET_X_LPARAM(lp);
        int y = GET_Y_LPARAM(lp);
        int idx = HitRow(x, y);
        if (idx >= 0) {
            SetSelectedIdx(idx);
            SendNotify(LBN_DBLCLK);
        }
        return 0;
    }
    case WM_RBUTTONDOWN: {
        int x = GET_X_LPARAM(lp);
        int y = GET_Y_LPARAM(lp);
        int idx = HitRow(x, y);
        if (idx >= 0) {
            SetSelectedIdx(idx);
            SendNotify(LBN_SELCHANGE);
        }
        // 우클릭 → 부모 WM_CONTEXTMENU 라우팅 (스크린 좌표).
        POINT sp = { x, y };
        ClientToScreen(hwnd, &sp);
        HWND parent = GetParent(hwnd);
        if (parent) {
            SendMessageW(parent, WM_CONTEXTMENU,
                         (WPARAM)hwnd,
                         MAKELPARAM(sp.x, sp.y));
        }
        return 0;
    }

    case WM_DESTROY:
        if (m_hToolTip) { DestroyWindow(m_hToolTip); m_hToolTip = nullptr; }
        DiscardDeviceResources();
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

inline bool CSidebar::Create(HWND parent, HINSTANCE hInst, int controlId) {
    if (!EnsureClass(hInst)) return false;
    m_controlId = controlId;
    m_hwnd = CreateWindowExW(
        0, kClassName, L"",
        WS_CHILD | WS_VISIBLE | WS_VSCROLL,
        0, 0, 0, 0,
        parent, (HMENU)(INT_PTR)controlId, hInst, this);
    if (m_hwnd) {
        EnsureToolTip();
        UpdateScrollInfo();
    }
    return m_hwnd != nullptr;
}

inline void CSidebar::Layout(int x, int y, int w, int h) {
    if (!m_hwnd) return;
    MoveWindow(m_hwnd, x, y, w, h, TRUE);
}

inline void CSidebar::Refill(std::vector<Row> rows) {
    m_rows = std::move(rows);
    m_contentHeight = (float)m_rows.size() * kRowH;
    // 선택/호버 인덱스 보존 — 범위 밖이면 환원.
    if (m_selectedIdx >= (int)m_rows.size()) m_selectedIdx = -1;
    if (m_hoverIdx    >= (int)m_rows.size()) m_hoverIdx    = -1;
    if (m_hwnd) {
        UpdateScrollInfo();
        InvalidateRect(m_hwnd, nullptr, FALSE);
    }
}

inline void CSidebar::SetCurrentChat(const std::string& key) {
    if (m_currentKey == key) return;
    m_currentKey = key;
    if (m_hwnd) InvalidateRect(m_hwnd, nullptr, FALSE);
}

inline void CSidebar::SetSelectedIdx(int idx) {
    if (idx < -1 || idx >= (int)m_rows.size()) idx = -1;
    if (idx == m_selectedIdx) return;
    m_selectedIdx = idx;
    if (m_hwnd) InvalidateRect(m_hwnd, nullptr, FALSE);
}

inline int CSidebar::FindChatIndex(const std::string& key,
                                   const std::string& goalId,
                                   const std::string& projectId) const {
    if (key.empty()) return -1;
    int fallback = -1;
    for (int i = 0; i < (int)m_rows.size(); ++i) {
        const Row& row = m_rows[i];
        if (row.kind != RowKind::Chat || row.chatKey != key) continue;
        if (fallback < 0) fallback = i;
        const bool goalMatches = goalId.empty() || row.goalId == goalId;
        const bool projectMatches = projectId.empty() || row.projectId == projectId;
        if (goalMatches && projectMatches) return i;
    }
    return fallback;
}

inline void CSidebar::SetHover(int idx) {
    if (idx == m_hoverIdx) return;
    m_hoverIdx = idx;
    if (m_hwnd) InvalidateRect(m_hwnd, nullptr, FALSE);
    UpdateToolTipText();
}

inline void CSidebar::UpdateScrollInfo() {
    if (!m_hwnd) return;
    RECT rc; GetClientRect(m_hwnd, &rc);
    int viewH = (int)((rc.bottom - rc.top) - kLogoAreaH);
    if (viewH < (int)kRowH) viewH = (int)kRowH;

    SCROLLINFO si{ sizeof(si) };
    si.fMask = SIF_ALL;
    si.nMin  = 0;
    si.nMax  = (int)m_rows.size() - 1;
    si.nPage = (UINT)(viewH / kRowH);
    // 현재 포지션 유지 (클램프)
    si.nPos = (int)(m_scrollPos / kRowH);
    SetScrollInfo(m_hwnd, SB_VERT, &si, TRUE);

    // 다시 읽어서 실제 적용된 pos 인계
    GetScrollInfo(m_hwnd, SB_VERT, &si);
    m_scrollPos = (float)si.nPos * kRowH;
}

inline int CSidebar::HitRow(int x, int y) const {
    for (const auto& h : m_rowHits) {
        if ((float)x >= h.rect.left && (float)x < h.rect.right &&
            (float)y >= h.rect.top  && (float)y < h.rect.bottom) {
            return h.idx;
        }
    }
    return -1;
}

inline void CSidebar::SendNotify(WORD code) {
    HWND parent = GetParent(m_hwnd);
    if (!parent) return;
    SendMessageW(parent, WM_COMMAND,
                 MAKEWPARAM((WORD)m_controlId, code),
                 (LPARAM)m_hwnd);
}

inline void CSidebar::EnsureToolTip() {
    if (m_hToolTip || !m_hwnd) return;
    HINSTANCE hInst = (HINSTANCE)GetWindowLongPtrW(m_hwnd, GWLP_HINSTANCE);
    m_hToolTip = CreateWindowExW(
        WS_EX_TOPMOST, TOOLTIPS_CLASS, nullptr,
        WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP,
        CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
        m_hwnd, nullptr, hInst, nullptr);
    if (!m_hToolTip) return;
    TOOLINFOW ti{};
    ti.cbSize   = sizeof(ti);
    ti.uFlags   = TTF_IDISHWND | TTF_TRANSPARENT;
    ti.hwnd     = m_hwnd;
    ti.uId      = (UINT_PTR)m_hwnd;
    ti.lpszText = (LPWSTR)L"";
    SendMessageW(m_hToolTip, TTM_ADDTOOLW, 0, (LPARAM)&ti);
    SendMessageW(m_hToolTip, TTM_SETMAXTIPWIDTH, 0, 480);
    SendMessageW(m_hToolTip, TTM_SETDELAYTIME, TTDT_AUTOPOP, 30000);
    SendMessageW(m_hToolTip, TTM_SETDELAYTIME, TTDT_INITIAL,    400);
}

inline void CSidebar::RelayMouseToToolTip(UINT msg, WPARAM wp, LPARAM lp) {
    if (!m_hToolTip) return;
    MSG mm{};
    mm.hwnd    = m_hwnd;
    mm.message = msg;
    mm.wParam  = wp;
    mm.lParam  = lp;
    SendMessageW(m_hToolTip, TTM_RELAYEVENT, 0, (LPARAM)&mm);
}

inline void CSidebar::UpdateToolTipText() {
    if (!m_hToolTip) return;
    TOOLINFOW ti{};
    ti.cbSize = sizeof(ti);
    ti.hwnd   = m_hwnd;
    ti.uId    = (UINT_PTR)m_hwnd;
    std::wstring text;
    if (m_hoverIdx >= 0 && m_hoverIdx < (int)m_rows.size()) {
        const Row& r = m_rows[m_hoverIdx];
        text = r.title;
        if (r.kind == RowKind::Chat && !r.lastActiveIso.empty()) {
            text += L"\r\n";
            text += Utf8ToWide(r.lastActiveIso.c_str(), (int)r.lastActiveIso.size());
        }
        if ((r.kind == RowKind::Goal || r.kind == RowKind::Project ||
             r.kind == RowKind::Chat) && r.progress > 0) {
            wchar_t buf[32];
            swprintf_s(buf, L"\r\n진척 %d%%", r.progress);
            text += buf;
        }
    }
    ti.lpszText = text.empty() ? (LPWSTR)L"" : (LPWSTR)text.c_str();
    SendMessageW(m_hToolTip, TTM_UPDATETIPTEXTW, 0, (LPARAM)&ti);
}

inline bool CSidebar::EnsureFactories() {
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
    if (!m_tfHeader) {
        m_dwFactory->CreateTextFormat(
            L"Segoe UI", nullptr,
            DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL, 11.0f, L"ko-KR", &m_tfHeader);
        if (m_tfHeader) m_tfHeader->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    }
    if (!m_tfBody) {
        m_dwFactory->CreateTextFormat(
            L"Segoe UI", nullptr,
            DWRITE_FONT_WEIGHT_REGULAR, DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL, 12.0f, L"ko-KR", &m_tfBody);
        if (m_tfBody) {
            m_tfBody->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
            DWRITE_TRIMMING trim = { DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0 };
            m_tfBody->SetTrimming(&trim, nullptr);
        }
    }
    if (!m_tfDim) {
        m_dwFactory->CreateTextFormat(
            L"Segoe UI", nullptr,
            DWRITE_FONT_WEIGHT_REGULAR, DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL, 11.0f, L"ko-KR", &m_tfDim);
        if (m_tfDim) {
            m_tfDim->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
            DWRITE_TRIMMING trim = { DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0 };
            m_tfDim->SetTrimming(&trim, nullptr);
        }
    }
    return m_d2dFactory && m_dwFactory && m_tfHeader && m_tfBody && m_tfDim;
}

inline bool CSidebar::EnsureDeviceResources() {
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

    // 활동 패널과 같은 라이트 베이지 결.
    m_rt->CreateSolidColorBrush(D2D1::ColorF(0xF7F2EE, 1.0f), &m_bgBrush);
    m_rt->CreateSolidColorBrush(D2D1::ColorF(0xE0D8C8, 1.0f), &m_separatorBrush);
    m_rt->CreateSolidColorBrush(D2D1::ColorF(0x2E2A26, 1.0f), &m_textBrush);
    m_rt->CreateSolidColorBrush(D2D1::ColorF(0x8F857A, 1.0f), &m_dimBrush);
    m_rt->CreateSolidColorBrush(D2D1::ColorF(0xFF8800, 1.0f), &m_goalBrush);     // 선택/현재 포인트
    m_rt->CreateSolidColorBrush(D2D1::ColorF(0x8FA2AA, 1.0f), &m_projectBrush);  // 비활성 도트
    m_rt->CreateSolidColorBrush(D2D1::ColorF(0xC8B89A, 1.0f), &m_chatBrush);     // 비활성 채팅 도트
    m_rt->CreateSolidColorBrush(D2D1::ColorF(0xB0A594, 1.0f), &m_newBrush);      // New* — 옅은 회갈색
    m_rt->CreateSolidColorBrush(D2D1::ColorF(0xFFEFDB, 1.0f), &m_hoverBrush);    // Hover: 오렌지 브랜드 tint
    m_rt->CreateSolidColorBrush(D2D1::ColorF(0xFFDFB8, 1.0f), &m_currentBrush);  // Active: 브랜드 오렌지 강조
    EnsureLogoBitmap();
    return true;
}

inline void CSidebar::DiscardDeviceResources() {
    Release(m_logoBitmap);
    Release(m_currentBrush);
    Release(m_hoverBrush);
    Release(m_newBrush);
    Release(m_chatBrush);
    Release(m_projectBrush);
    Release(m_goalBrush);
    Release(m_dimBrush);
    Release(m_textBrush);
    Release(m_separatorBrush);
    Release(m_bgBrush);
    Release(m_rt);
}

inline void CSidebar::EnsureLogoBitmap() {
    if (m_logoBitmap || !m_rt) return;

    const wchar_t* path = L"O:\\Work\\OrangeLabs\\Orange\\agent\\binary\\image\\orangelabs.png";
    HRESULT coHr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    bool needUninit = SUCCEEDED(coHr);

    IWICImagingFactory* factory = nullptr;
    IWICBitmapDecoder* decoder = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICFormatConverter* converter = nullptr;

    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                                  CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
    if (SUCCEEDED(hr)) {
        hr = factory->CreateDecoderFromFilename(path, nullptr, GENERIC_READ,
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
        hr = m_rt->CreateBitmapFromWicBitmap(converter, nullptr, &m_logoBitmap);
    }

    Release(converter);
    Release(frame);
    Release(decoder);
    Release(factory);
    if (needUninit) CoUninitialize();
    if (FAILED(hr)) Release(m_logoBitmap);
}

inline void CSidebar::DrawLogo(float width, float height) {
    EnsureLogoBitmap();
    if (!m_logoBitmap || !m_rt) return;

    D2D1_SIZE_F src = m_logoBitmap->GetSize();
    if (src.width <= 0.0f || src.height <= 0.0f) return;

    const float maxW = (width - 38.0f) * 0.75f;
    const float maxH = 25.5f;
    float scale = maxW / src.width;
    if (src.height * scale > maxH) scale = maxH / src.height;
    if (scale > 1.0f) scale = 1.0f;

    float w = src.width * scale;
    float h = src.height * scale;
    float x = 20.0f;
    float y = height - kLogoAreaH + (kLogoAreaH - h) * 0.5f;
    m_rt->DrawBitmap(m_logoBitmap,
                     D2D1::RectF(x, y, x + w, y + h),
                     0.92f,
                     D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
}

inline void CSidebar::OnPaint() {
    if (!EnsureDeviceResources()) return;

    m_rt->BeginDraw();
    m_rt->Clear(D2D1::ColorF(0xF7F2EE, 1.0f)); // 기존보다 약간 더 밝고 세련된 베이지

    RECT rc;
    GetClientRect(m_hwnd, &rc);
    const float W = (float)(rc.right - rc.left);
    const float H = (float)(rc.bottom - rc.top);
    const float listBottom = H - kLogoAreaH;

    // 우측 1px 분리선 — 더 부드러운 색상
    m_rt->FillRectangle(D2D1::RectF(W - 1.0f, 0.0f, W, H), m_separatorBrush);

    m_tfBody->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    m_tfDim ->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

    // kRowH 는 클래스 상수로 이전됨 (32.0f).
    constexpr float kIndentPx  = 16.0f;
    constexpr float kDotR      = 4.0f;  // 조금 더 작고 단단한 느낌
    constexpr float kDotAdv    = 18.0f;
    constexpr float kBarW      = 44.0f;
    constexpr float kBarH      = 3.0f;  // 더 얇고 샤프한 바
    constexpr float kMarginH   = 6.0f;  // 좌우 여백

    m_rowHits.clear();
    float y = -m_scrollPos + 4.0f; // 상단 약간의 여백

    for (size_t i = 0; i < m_rows.size(); ++i) {
        const float rowTop = y;
        const float rowBot = y + kRowH - 2.0f; // 행 사이 간격 2px
        
        if (rowBot < 0) { y += kRowH; continue; }
        if (rowTop >= listBottom) break;

        const Row& row = m_rows[i];
        const int  idx = (int)i;

        if (row.kind == RowKind::Empty) {
            D2D1_RECT_F headerRect = D2D1::RectF(kMarginH + 8.0f, rowTop, W - kMarginH, rowBot);
            m_tfHeader->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            m_rt->DrawTextW(row.title.c_str(), (UINT32)row.title.size(), m_tfHeader, headerRect, m_dimBrush);
            y += kRowH;
            continue;
        }

        // ── 행 카드 영역 (둥근 사각형) ──
        D2D1_ROUNDED_RECT rowRR = D2D1::RoundedRect(
            D2D1::RectF(kMarginH, rowTop, W - kMarginH - 1.0f, rowBot), 4.0f, 4.0f);
        
        // hit-test 영역은 클릭 편의상 전체 폭
        D2D1_RECT_F hitRect = D2D1::RectF(0.0f, rowTop, W, rowBot);
        m_rowHits.push_back({ hitRect, idx });

        // ── 행 배경 ──
        if (idx == m_selectedIdx) {
            m_rt->FillRoundedRectangle(rowRR, m_currentBrush);
        } else if (idx == m_hoverIdx) {
            m_rt->FillRoundedRectangle(rowRR, m_hoverBrush);
        }

        float x = kMarginH + 8.0f + row.level * kIndentPx;
        const float midY = (rowTop + rowBot) / 2.0f;

        const bool isCurrentChat =
            (row.kind == RowKind::Chat) &&
            (row.isCurrent || (!m_currentKey.empty() && row.chatKey == m_currentKey));

        // ── 도트: 타입 장식이 아니라 현재/선택 상태 표시를 우선한다. 세로 선택 바는 쓰지 않는다. ──
        ID2D1SolidColorBrush* dotBrush = nullptr;
        const bool isSelected = (idx == m_selectedIdx);
        if (isSelected) {
            dotBrush = m_goalBrush;
        } else {
        switch (row.kind) {
            case RowKind::Goal:
            case RowKind::Project:    dotBrush = m_projectBrush; break;
            case RowKind::Chat:       dotBrush = m_chatBrush; break;
            case RowKind::NewChat:
            case RowKind::NewGoal:
            case RowKind::NewProject: dotBrush = m_newBrush; break;
            default:                  break;
            }
        }
        if (row.kind == RowKind::NewChat && !isCurrentChat && !isSelected) {
            ID2D1SolidColorBrush* plusBrush = m_newBrush ? m_newBrush : m_dimBrush;
            const float cx = x + kDotR;
            const float len = 4.0f;
            m_rt->DrawLine(D2D1::Point2F(cx - len, midY),
                           D2D1::Point2F(cx + len, midY),
                           plusBrush, 1.6f);
            m_rt->DrawLine(D2D1::Point2F(cx, midY - len),
                           D2D1::Point2F(cx, midY + len),
                           plusBrush, 1.6f);
        } else if (dotBrush) {
            D2D1_ELLIPSE dot = D2D1::Ellipse(D2D1::Point2F(x + kDotR, midY), kDotR, kDotR);
            m_rt->FillEllipse(dot, dotBrush);
        }
        x += kDotAdv;

        // ── 제목 ──
        const bool isNew = (row.kind == RowKind::NewChat || row.kind == RowKind::NewGoal || row.kind == RowKind::NewProject);
        ID2D1SolidColorBrush* titleBrush = isNew ? m_dimBrush : m_textBrush;
        D2D1_RECT_F titleRect = D2D1::RectF(x, rowTop, W - 60.0f, rowBot);
        m_tfBody->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        m_rt->DrawTextW(row.title.c_str(), (UINT32)row.title.size(), m_tfBody, titleRect, titleBrush);

        // ── 진척 막대 ──
        const bool hasTrailing = !row.trailing.empty();
        if (row.progress > 0 || (row.kind == RowKind::Chat && !hasTrailing)) {
            const float barX = W - kBarW - kMarginH - 6.0f;
            const float barY = rowBot - 8.0f;
            m_rt->FillRectangle(D2D1::RectF(barX, barY, barX + kBarW, barY + kBarH), m_separatorBrush);
            if (row.progress > 0) {
                float fillW = kBarW * (row.progress / 100.0f);
                m_rt->FillRectangle(D2D1::RectF(barX, barY, barX + fillW, barY + kBarH), m_goalBrush);
            }
        }

        // ── trailing (시각 등) ──
        if (hasTrailing) {
            D2D1_RECT_F trR = D2D1::RectF(W - 60.0f, rowTop, W - kMarginH - 4.0f, rowBot);
            m_tfDim->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
            m_rt->DrawTextW(row.trailing.c_str(), (UINT32)row.trailing.size(), m_tfDim, trR, m_dimBrush);
        }

        y += kRowH;
    }

    DrawLogo(W, H);

    HRESULT hr = m_rt->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET) {
        DiscardDeviceResources();
    }
}

}  // namespace orange
