#pragma once

// CActivityPanel ???곗륫 ?쒕룞 媛?쒖꽦 ?⑤꼸.
//
// ?ъ슜??04:24 寃곗젙 (*1踰?= ?쒕룞 媛?쒖꽦*) ???좊?.
// Coordination::ListActors() 1.5珥??대쭅 ??媛?actor ????洹몃━湲?
// ????= role ??쨌 role ?쇰꺼 쨌 pid 쨌 last_seen ?쎌떇 + intent ??以??쎌떇.
//
// 2?④퀎 ??Direct2D 寃곕줈 媛덉븘?롮쓬. 蹂몃Ц OrangeView ??移대뱶 寃??κ렐 紐⑥꽌由?룸뵲
// ?삵븳 踰좎씠吏쨌DirectWrite ko-KR ?고듃)怨?寃??쇱튂. GDI 嫄곗튇 寃??먭린.
//
// ?쒓렇?덉쿂(Create/Layout/Poll/Width/Hwnd)??1?④퀎? ?숈씪 ??main.cpp ?듯빀遺
// ??留뚯쭚. ?먯껜 D2D Factory + DirectWrite Factory + HwndRenderTarget.

#include <windows.h>
#include <commctrl.h>
#include <d2d1.h>
#include <dwrite.h>
#include <algorithm>
#include <string>
#include <vector>
#include <memory>
#include <cstdio>

#include "BuildHistory.h"
#include "Coordination.h"
#include "Utils.h"

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")

namespace orange {

class CActivityPanel {
public:
    CActivityPanel() = default;
    ~CActivityPanel() {
        DiscardDeviceResources();
        Release(m_tfBody);
        Release(m_tfDim);
        Release(m_tfHeader);
        Release(m_dwFactory);
        Release(m_d2dFactory);
    }

    bool Create(HWND parent, HINSTANCE hInst);

    HWND Hwnd()  const { return m_hwnd; }
    int  Width() const { return kWidth; }

    void Layout(int parentWidth, int parentHeight);

    // 1.5珥???대㉧?먯꽌 ?몄텧 ??Coordination 媛깆떊 + InvalidateRect.
    void Poll();

    static constexpr int kWidth = 200;

private:
    static LRESULT CALLBACK StaticWndProc(HWND, UINT, WPARAM, LPARAM);
    LRESULT WndProc(HWND, UINT, WPARAM, LPARAM);

    bool EnsureFactories();
    bool EnsureDeviceResources();
    void DiscardDeviceResources();
    void OnPaint();

    static const wchar_t* kClassName;
    static bool s_classRegistered;
    static bool EnsureClass(HINSTANCE hInst);

    static int64_t SecondsAgo(const std::string& iso);
    static std::wstring FmtAge(int64_t sec);

    template <class T>
    static void Release(T*& p) { if (p) { p->Release(); p = nullptr; } }

    HWND m_hwnd = nullptr;

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
    ID2D1SolidColorBrush*   m_managerBrush      = nullptr;
    ID2D1SolidColorBrush*   m_supBrush        = nullptr;
    ID2D1SolidColorBrush*   m_memberBrush     = nullptr;
    ID2D1SolidColorBrush*   m_deadBrush       = nullptr;

    std::vector<CCoordination::ActorStatus> m_actors;

    // hover ?댄똻 ?먮━ ????hit-test ???꾩껜 intent ?쒖떆.
    struct RowHit {
        D2D1_RECT_F  rect;
        std::wstring fullText;   // role(pid) + ?꾩껜 intent
    };
    std::vector<RowHit> m_rowHits;
    HWND m_hToolTip = nullptr;
    int  m_hoverIdx = -1;
    bool m_mouseTracking = false;

    void EnsureToolTip();
    void RelayMouseToToolTip(UINT msg, WPARAM wp, LPARAM lp);
    int  HitRow(int x, int y) const;
};

inline const wchar_t* CActivityPanel::kClassName = L"OrangeActivityPanel";
inline bool CActivityPanel::s_classRegistered = false;

inline bool CActivityPanel::EnsureClass(HINSTANCE hInst) {
    if (s_classRegistered) return true;
    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = StaticWndProc;
    wc.hInstance     = hInst;
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;  // D2D ??留??꾨젅??Clear
    wc.lpszClassName = kClassName;
    if (!RegisterClassExW(&wc)) return false;
    s_classRegistered = true;
    return true;
}

inline LRESULT CALLBACK CActivityPanel::StaticWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    CActivityPanel* self = nullptr;
    if (msg == WM_NCCREATE) {
        auto* cs = (CREATESTRUCTW*)lp;
        self = (CActivityPanel*)cs->lpCreateParams;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)self);
    } else {
        self = (CActivityPanel*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    }
    if (self) return self->WndProc(hwnd, msg, wp, lp);
    return DefWindowProcW(hwnd, msg, wp, lp);
}

inline LRESULT CActivityPanel::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_PAINT: {
        OnPaint();
        ValidateRect(hwnd, nullptr);
        return 0;
    }
    case WM_SIZE:
        if (m_rt) {
            D2D1_SIZE_U s = D2D1::SizeU(LOWORD(lp), HIWORD(lp));
            m_rt->Resize(s);
        }
        return 0;
    case WM_ERASEBKGND:
        return 1;  // D2D 媛 留??꾨젅??Clear
    case WM_MOUSEMOVE: {
        // 留덉슦??異붿쟻 ?쒖옉 ??leave ?대깽??諛쏄린 ?꾪빐.
        if (!m_mouseTracking) {
            TRACKMOUSEEVENT tme{};
            tme.cbSize    = sizeof(tme);
            tme.dwFlags   = TME_LEAVE;
            tme.hwndTrack = hwnd;
            TrackMouseEvent(&tme);
            m_mouseTracking = true;
        }
        int x = LOWORD(lp);
        int y = HIWORD(lp);
        int idx = HitRow(x, y);
        // ????hover ?먮뒗 泥?hover 硫?tooltip ?띿뒪??媛깆떊.
        if (idx != m_hoverIdx) {
            m_hoverIdx = idx;
            if (m_hToolTip) {
                TOOLINFOW ti{};
                ti.cbSize = sizeof(ti);
                ti.hwnd   = hwnd;
                ti.uId    = (UINT_PTR)hwnd;
                if (idx >= 0 && idx < (int)m_rowHits.size()) {
                    ti.lpszText = (LPWSTR)m_rowHits[idx].fullText.c_str();
                } else {
                    ti.lpszText = (LPWSTR)L"";
                }
                SendMessageW(m_hToolTip, TTM_UPDATETIPTEXTW, 0, (LPARAM)&ti);
            }
        }
        RelayMouseToToolTip(msg, wp, lp);
        return 0;
    }
    case WM_MOUSELEAVE:
        m_mouseTracking = false;
        m_hoverIdx = -1;
        // tooltip ?띿뒪??鍮꾩썙 ?リ린.
        if (m_hToolTip) {
            TOOLINFOW ti{};
            ti.cbSize   = sizeof(ti);
            ti.hwnd     = hwnd;
            ti.uId      = (UINT_PTR)hwnd;
            ti.lpszText = (LPWSTR)L"";
            SendMessageW(m_hToolTip, TTM_UPDATETIPTEXTW, 0, (LPARAM)&ti);
        }
        return 0;
    case WM_DESTROY:
        if (m_hToolTip) { DestroyWindow(m_hToolTip); m_hToolTip = nullptr; }
        DiscardDeviceResources();
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

inline bool CActivityPanel::Create(HWND parent, HINSTANCE hInst) {
    if (!EnsureClass(hInst)) return false;
    m_hwnd = CreateWindowExW(
        0, kClassName, L"",
        WS_CHILD | WS_VISIBLE,
        0, 0, 0, 0,
        parent, nullptr, hInst, this);
    if (m_hwnd) EnsureToolTip();
    return m_hwnd != nullptr;
}

inline void CActivityPanel::EnsureToolTip() {
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
    SendMessageW(m_hToolTip, TTM_SETDELAYTIME, TTDT_AUTOPOP, 30000);  // 30珥??쒖떆
    SendMessageW(m_hToolTip, TTM_SETDELAYTIME, TTDT_INITIAL,    400);
}

inline int CActivityPanel::HitRow(int x, int y) const {
    for (size_t i = 0; i < m_rowHits.size(); ++i) {
        const auto& r = m_rowHits[i].rect;
        if ((float)x >= r.left && (float)x < r.right &&
            (float)y >= r.top  && (float)y < r.bottom) {
            return (int)i;
        }
    }
    return -1;
}

inline void CActivityPanel::RelayMouseToToolTip(UINT msg, WPARAM wp, LPARAM lp) {
    if (!m_hToolTip) return;
    MSG mm{};
    mm.hwnd    = m_hwnd;
    mm.message = msg;
    mm.wParam  = wp;
    mm.lParam  = lp;
    SendMessageW(m_hToolTip, TTM_RELAYEVENT, 0, (LPARAM)&mm);
}

inline void CActivityPanel::Layout(int parentWidth, int parentHeight) {
    if (!m_hwnd) return;
    int x = parentWidth - kWidth;
    if (x < 0) x = 0;
    MoveWindow(m_hwnd, x, 0, kWidth, parentHeight, TRUE);
}

inline void CActivityPanel::Poll() {
    m_actors = CCoordination::ListActors();

    // 1?쒓컙 ?댁긽 媛깆떊 ????actor ??*二쎌뿀?ㅺ퀬 蹂닿퀬* ?쒖떆?먯꽌 ?쒖쇅.
    // actor entry json ?먯껜??蹂댁〈 (?곸냽 濡쒓렇). ?⑤꼸? *?댁븘?덈뒗 ?먮쫫* 留?蹂댁엫.
    constexpr int64_t kStaleSeconds = 3600;
    m_actors.erase(
        std::remove_if(m_actors.begin(), m_actors.end(),
            [](const CCoordination::ActorStatus& a) {
                int64_t age = SecondsAgo(a.lastSeenIso);
                return age > kStaleSeconds;
            }),
        m_actors.end());

    if (m_hwnd) InvalidateRect(m_hwnd, nullptr, FALSE);
}

inline int64_t CActivityPanel::SecondsAgo(const std::string& iso) {
    if (iso.size() < 19) return -1;
    int y = 0, mo = 0, d = 0, h = 0, mi = 0, s = 0;
    if (sscanf_s(iso.c_str(), "%d-%d-%dT%d:%d:%d", &y, &mo, &d, &h, &mi, &s) != 6) return -1;
    SYSTEMTIME st{};
    st.wYear = (WORD)y; st.wMonth = (WORD)mo; st.wDay = (WORD)d;
    st.wHour = (WORD)h; st.wMinute = (WORD)mi; st.wSecond = (WORD)s;
    FILETIME ft{};
    if (!SystemTimeToFileTime(&st, &ft)) return -1;
    SYSTEMTIME now{};
    GetSystemTime(&now);
    FILETIME ftNow{};
    SystemTimeToFileTime(&now, &ftNow);
    ULARGE_INTEGER a{}; a.LowPart = ft.dwLowDateTime;    a.HighPart = ft.dwHighDateTime;
    ULARGE_INTEGER b{}; b.LowPart = ftNow.dwLowDateTime; b.HighPart = ftNow.dwHighDateTime;
    if (b.QuadPart < a.QuadPart) return 0;
    return (int64_t)((b.QuadPart - a.QuadPart) / 10000000ULL);
}

inline std::wstring CActivityPanel::FmtAge(int64_t sec) {
    wchar_t buf[16];
    if (sec < 0)         return L"?";
    if (sec < 5)         return L"諛⑷툑";
    if (sec < 60)      { swprintf_s(buf, L"%llds",  (long long)sec);            return buf; }
    if (sec < 3600)    { swprintf_s(buf, L"%lldm",  (long long)(sec / 60));     return buf; }
    if (sec < 86400)   { swprintf_s(buf, L"%lldh",  (long long)(sec / 3600));   return buf; }
                       { swprintf_s(buf, L"%lldd",  (long long)(sec / 86400));  return buf; }
}

inline bool CActivityPanel::EnsureFactories() {
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

inline bool CActivityPanel::EnsureDeviceResources() {
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

    // 蹂몃Ц(#F4EFE9) 蹂대떎 ?댁쭩 吏숈? 踰좎씠吏 ??*?⑤꼸?대씪???곸뿭媛? ?쏀븯寃?遺꾨━.
    m_rt->CreateSolidColorBrush(D2D1::ColorF(0xECE3D6, 1.0f), &m_bgBrush);
    m_rt->CreateSolidColorBrush(D2D1::ColorF(0xD8CFC2, 1.0f), &m_separatorBrush);
    m_rt->CreateSolidColorBrush(D2D1::ColorF(0x2E2A26, 1.0f), &m_textBrush);
    m_rt->CreateSolidColorBrush(D2D1::ColorF(0x6F655A, 1.0f), &m_dimBrush);
    m_rt->CreateSolidColorBrush(D2D1::ColorF(0xFF8800, 1.0f), &m_managerBrush);   // ?뺥??????ㅻ젋吏(釉뚮옖???≪꽱??
    m_rt->CreateSolidColorBrush(D2D1::ColorF(0x6B7A4F, 1.0f), &m_supBrush);     // ?ㅺ컧?????몄씠吏
    m_rt->CreateSolidColorBrush(D2D1::ColorF(0x4A7A8C, 1.0f), &m_memberBrush);  // ??????고???泥?줉
    m_rt->CreateSolidColorBrush(D2D1::ColorF(0xC0B5A4, 1.0f), &m_deadBrush);    // 二쎌? actor ???낆? 踰좎씠吏
    return true;
}

inline void CActivityPanel::DiscardDeviceResources() {
    Release(m_deadBrush);
    Release(m_memberBrush);
    Release(m_supBrush);
    Release(m_managerBrush);
    Release(m_dimBrush);
    Release(m_textBrush);
    Release(m_separatorBrush);
    Release(m_bgBrush);
    Release(m_rt);
}

inline void CActivityPanel::OnPaint() {
    if (!EnsureDeviceResources()) return;

    m_rt->BeginDraw();
    m_rt->Clear(D2D1::ColorF(0xECE3D6, 1.0f));

    RECT rc;
    GetClientRect(m_hwnd, &rc);
    const float W = (float)(rc.right - rc.left);
    const float H = (float)(rc.bottom - rc.top);

    // 醫뚯륫 1px 遺꾨━????梨꾪똿 ?곸뿭怨?媛?쒖쟻 遺꾨━
    m_rt->FillRectangle(D2D1::RectF(0.0f, 0.0f, 1.0f, H), m_separatorBrush);

    const float padX = 14.0f;
    float y = 12.0f;

    // ?ㅻ뜑 ?쇰꺼
    {
        D2D1_RECT_F r = D2D1::RectF(padX, y, W - 10.0f, y + 18.0f);
        const wchar_t* title = L"????;
        m_rt->DrawText(title, (UINT32)wcslen(title), m_tfHeader, r, m_dimBrush);
    }
    y += 22.0f;

    // ?ㅻ뜑 ?꾨옒 ?낆? ?쇱씤
    m_rt->FillRectangle(D2D1::RectF(padX, y, W - 10.0f, y + 1.0f), m_separatorBrush);
    y += 8.0f;

    m_rowHits.clear();
    if (m_actors.empty()) {
        const wchar_t* empty = L"actors ?놁쓬";
        D2D1_RECT_F r = D2D1::RectF(padX, y, W - 10.0f, y + 18.0f);
        m_rt->DrawText(empty, (UINT32)wcslen(empty), m_tfDim, r, m_dimBrush);
    } else {
        for (const auto& a : m_actors) {
            if (y > H - 24.0f) break;
            const float rowTop = y;

            int64_t sec = SecondsAgo(a.lastSeenIso);
            bool dead   = (sec < 0 || sec > 300);
            bool stale  = (!dead && sec > 60);

            ID2D1SolidColorBrush* roleBrush = m_dimBrush;
            if      (a.kind == "manager")      roleBrush = m_managerBrush;
            else if (a.kind == "supervisor") roleBrush = m_supBrush;
            else if (a.kind == "member")     roleBrush = m_memberBrush;
            if (dead) roleBrush = m_deadBrush;

            // role ?????κ렐 ?꾪듃 (5px). 移대뱶 寃곗쓽 ?κ렐 紐⑥꽌由ъ? ???쇱튂.
            const float dotR = 4.5f;
            D2D1_ELLIPSE dot = D2D1::Ellipse(
                D2D1::Point2F(padX + dotR, y + 9.0f), dotR, dotR);
            m_rt->FillEllipse(dot, roleBrush);

            // stale ? ?댁쭩 ?낆? 寃곕줈 ?????꾩뿉 dim ??諛섑닾紐???뼱 ?고븳 寃?
            if (stale && !dead) {
                m_deadBrush->SetOpacity(0.35f);
                m_rt->FillEllipse(dot, m_deadBrush);
                m_deadBrush->SetOpacity(1.0f);
            }

            const float textLeft = padX + 2.0f * dotR + 8.0f;

            const wchar_t* roleLabel;
            if      (a.kind == "manager")      roleLabel = L"manager";
            else if (a.kind == "supervisor") roleLabel = L"supervisor";
            else if (a.kind == "member")     roleLabel = L"member";
            else                             roleLabel = L"?";

            // 泥?以???role ?쇰꺼 + pid + age
            std::wstring line1 = roleLabel;
            if (a.pid != 0) {
                wchar_t pidBuf[32];
                swprintf_s(pidBuf, L"(%lu)", (unsigned long)a.pid);
                line1 += pidBuf;
            }
            line1 += L"  ";
            line1 += FmtAge(sec);
            {
                D2D1_RECT_F r = D2D1::RectF(textLeft, y - 1.0f, W - 10.0f, y + 18.0f);
                m_rt->DrawText(line1.c_str(), (UINT32)line1.size(),
                               m_tfBody, r,
                               dead ? m_dimBrush : m_textBrush);
            }
            y += 18.0f;

            // ?섏㎏ 以???turn 吏꾪뻾 ??prompt 泥?以? ?꾨땲硫?currentIntent
            std::string snippet;
            if (a.turnInProgress && !a.turnPrompt.empty()) {
                snippet = "\xe2\x96\xb6 ";  // ??                snippet += a.turnPrompt;
            } else {
                snippet = a.currentIntent;
            }
            if (!snippet.empty()) {
                std::wstring snippetW = Utf8ToWide(snippet.c_str(), (int)snippet.size());
                if (snippetW.size() > 200) snippetW.resize(200);
                D2D1_RECT_F r = D2D1::RectF(textLeft, y, W - 10.0f, y + 16.0f);
                m_rt->DrawText(snippetW.c_str(), (UINT32)snippetW.size(),
                               m_tfDim, r, m_dimBrush);
                y += 16.0f;
            }

            // ??hit-test ?곸뿭 諛뺢린 ???щ갚 吏곸쟾 源뚯?媛 ??蹂몄껜.
            {
                RowHit hit;
                hit.rect = D2D1::RectF(0.0f, rowTop - 2.0f, W, y + 4.0f);
                std::wstring full = roleLabel;
                if (a.pid != 0) {
                    wchar_t pidBuf[32];
                    swprintf_s(pidBuf, L"(%lu)", (unsigned long)a.pid);
                    full += pidBuf;
                }
                if (!a.actorId.empty()) {
                    full += L" ??";
                    full += Utf8ToWide(a.actorId.c_str(), (int)a.actorId.size());
                }
                if (a.turnInProgress && !a.turnPrompt.empty()) {
                    full += L"\r\n吏꾪뻾 以? ";
                    full += Utf8ToWide(a.turnPrompt.c_str(), (int)a.turnPrompt.size());
                }
                if (!a.currentIntent.empty()) {
                    full += L"\r\n";
                    full += Utf8ToWide(a.currentIntent.c_str(), (int)a.currentIntent.size());
                }
                if (!a.claimFiles.empty()) {
                    full += L"\r\n[claim: ";
                    for (size_t i = 0; i < a.claimFiles.size(); ++i) {
                        if (i > 0) full += L", ";
                        full += Utf8ToWide(a.claimFiles[i].c_str(), (int)a.claimFiles[i].size());
                    }
                    full += L"]";
                }
                hit.fullText = std::move(full);
                m_rowHits.push_back(std::move(hit));
            }

            // ???ъ씠 ?щ갚
            y += 10.0f;
        }
    }

    HRESULT hr = m_rt->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET) {
        DiscardDeviceResources();
    }
}

}  // namespace orange
