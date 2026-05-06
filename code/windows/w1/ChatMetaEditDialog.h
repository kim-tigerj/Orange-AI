#pragma once

// ChatMetaEditDialog ??梨꾪똿 (ChatSession) ??硫뷀? (title + progress) 瑜?// ??dialog ?먯꽌 ?몄쭛?⑸땲??
//
// 吏곸쟾源뚯? ?ъ씠?쒕컮 ?고겢由?硫붾돱??*梨꾪똿 ?대쫫 蹂寃? + *吏꾩쿃 %* ????ぉ???곕줈 ?덉뿀?붾뜲,
// MetaEditDialog (Goal/Project) ??寃곗쓣 ?곕씪 ??臾띠쓬 dialog 濡??듯빀?⑸땲??

#include <windows.h>
#include <commctrl.h>
#include <string>

namespace orange {

struct ChatMetaEditValues {
    std::wstring  title;
    int           progress = 0;
};

namespace detail_chatmetadlg {

constexpr wchar_t kClassName[] = L"OrangeChatMetaEditDialog";
constexpr int    kIdcTitle    = 1201;
constexpr int    kIdcProgress = 1202;
constexpr int    kIdcOk       = 1203;
constexpr int    kIdcCancel   = 1204;

struct DlgState {
    ChatMetaEditValues  current;
    ChatMetaEditValues  edited;
    bool                confirmed = false;
};

inline LRESULT CALLBACK ChatMetaProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_CREATE) {
        auto* cs = (CREATESTRUCTW*)lp;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
        auto* st = (DlgState*)cs->lpCreateParams;

        HINSTANCE hi = (HINSTANCE)GetWindowLongPtrW(hwnd, GWLP_HINSTANCE);
        const int W = 380, marginX = 14;
        int y = 14;

        // 梨꾪똿 ?대쫫
        CreateWindowExW(0, L"STATIC", L"梨꾪똿 ?대쫫",
            WS_CHILD | WS_VISIBLE | SS_LEFT,
            marginX, y, 80, 18, hwnd, nullptr, hi, nullptr);
        CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", st->current.title.c_str(),
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
            marginX + 84, y - 2, W - marginX * 2 - 84, 24,
            hwnd, (HMENU)(LONG_PTR)kIdcTitle, hi, nullptr);
        y += 32;

        // 吏꾩쿃
        CreateWindowExW(0, L"STATIC", L"吏꾩쿃 %",
            WS_CHILD | WS_VISIBLE | SS_LEFT,
            marginX, y, 80, 18, hwnd, nullptr, hi, nullptr);
        wchar_t pBuf[16];
        swprintf_s(pBuf, L"%d", st->current.progress);
        CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", pBuf,
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL | ES_NUMBER,
            marginX + 84, y - 2, 80, 24,
            hwnd, (HMENU)(LONG_PTR)kIdcProgress, hi, nullptr);
        y += 36;

        // ?뺤씤 / 痍⑥냼
        CreateWindowExW(0, L"BUTTON", L"?뺤씤",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
            W - marginX - 168, y, 76, 28,
            hwnd, (HMENU)(LONG_PTR)kIdcOk, hi, nullptr);
        CreateWindowExW(0, L"BUTTON", L"痍⑥냼",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
            W - marginX - 84, y, 76, 28,
            hwnd, (HMENU)(LONG_PTR)kIdcCancel, hi, nullptr);

        // ?고듃 ???쒖뒪??硫붿떆吏 ?고듃.
        NONCLIENTMETRICSW ncm{}; ncm.cbSize = sizeof(ncm);
        SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
        HFONT hFont = CreateFontIndirectW(&ncm.lfMessageFont);
        EnumChildWindows(hwnd, [](HWND ch, LPARAM lp) -> BOOL {
            SendMessageW(ch, WM_SETFONT, (WPARAM)lp, TRUE);
            return TRUE;
        }, (LPARAM)hFont);
        SetWindowLongPtrW(hwnd, (int)(GWLP_USERDATA + sizeof(LONG_PTR)), (LONG_PTR)hFont);

        SetFocus(GetDlgItem(hwnd, kIdcTitle));
        SendMessageW(GetDlgItem(hwnd, kIdcTitle), EM_SETSEL, 0, -1);
        return 0;
    }
    if (msg == WM_COMMAND) {
        auto* st = (DlgState*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
        if (LOWORD(wp) == kIdcOk) {
            auto getText = [&](int id) {
                HWND h = GetDlgItem(hwnd, id);
                int len = GetWindowTextLengthW(h);
                std::wstring buf(len, L'\0');
                if (len > 0) GetWindowTextW(h, buf.data(), len + 1);
                return buf;
            };
            st->edited.title = getText(kIdcTitle);
            int pv = _wtoi(getText(kIdcProgress).c_str());
            if (pv < 0)   pv = 0;
            if (pv > 100) pv = 100;
            st->edited.progress = pv;
            st->confirmed = true;
            DestroyWindow(hwnd);
            return 0;
        }
        if (LOWORD(wp) == kIdcCancel) {
            st->confirmed = false;
            DestroyWindow(hwnd);
            return 0;
        }
    }
    if (msg == WM_CLOSE) { DestroyWindow(hwnd); return 0; }
    if (msg == WM_DESTROY) {
        HFONT hFont = (HFONT)GetWindowLongPtrW(hwnd, (int)(GWLP_USERDATA + sizeof(LONG_PTR)));
        if (hFont) DeleteObject(hFont);
        PostThreadMessageW(GetCurrentThreadId(), WM_NULL, 0, 0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

inline void EnsureClassRegistered(HINSTANCE hInst) {
    static bool done = false;
    if (done) return;
    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = ChatMetaProc;
    wc.hInstance     = hInst;
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = kClassName;
    wc.cbWndExtra    = sizeof(LONG_PTR) * 2;
    if (RegisterClassExW(&wc)) done = true;
}

}  // namespace detail_chatmetadlg

inline bool EditChatMeta(HWND parent, ChatMetaEditValues* inout) {
    if (!inout) return false;
    HINSTANCE hInst = parent ? (HINSTANCE)GetWindowLongPtrW(parent, GWLP_HINSTANCE)
                              : GetModuleHandleW(nullptr);
    detail_chatmetadlg::EnsureClassRegistered(hInst);

    detail_chatmetadlg::DlgState st;
    st.current = *inout;

    constexpr int dlgW = 380, dlgH = 160;
    RECT pr; GetClientRect(parent ? parent : GetDesktopWindow(), &pr);
    POINT center = { (pr.right - pr.left) / 2, (pr.bottom - pr.top) / 2 };
    if (parent) ClientToScreen(parent, &center);
    int x = center.x - dlgW / 2;
    int y = center.y - dlgH / 2;

    if (parent) EnableWindow(parent, FALSE);

    HWND dlg = CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_TOPMOST,
        detail_chatmetadlg::kClassName, L"梨꾪똿 硫뷀? ?몄쭛",
        WS_CAPTION | WS_SYSMENU | WS_POPUP | WS_VISIBLE,
        x, y, dlgW, dlgH,
        parent, nullptr, hInst, &st);

    if (dlg) {
        MSG msg;
        while (GetMessageW(&msg, nullptr, 0, 0) > 0 && IsWindow(dlg)) {
            if (msg.message == WM_KEYDOWN) {
                if (msg.wParam == VK_ESCAPE) {
                    SendMessageW(dlg, WM_COMMAND, detail_chatmetadlg::kIdcCancel, 0);
                    continue;
                }
                if (msg.wParam == VK_RETURN) {
                    SendMessageW(dlg, WM_COMMAND, detail_chatmetadlg::kIdcOk, 0);
                    continue;
                }
            }
            if (IsDialogMessageW(dlg, &msg)) continue;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    if (parent) {
        EnableWindow(parent, TRUE);
        SetForegroundWindow(parent);
    }
    if (st.confirmed) {
        *inout = st.edited;
        return true;
    }
    return false;
}

}  // namespace orange
