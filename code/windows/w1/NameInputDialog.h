#pragma once

// NameInputDialog ????以??대쫫 ?낅젰 modal dialog.
//
// ?ъ슜:
//   std::wstring name = orange::PromptForName(parentHwnd, L"??紐⑺몴", L"紐⑺몴 ?대쫫");
//   if (name.empty()) return;  // 痍⑥냼
//
// 援ъ“: ?먯껜 ?덈룄???대옒??+ 紐⑤떖 硫붿떆吏 猷⑦봽 (parent 鍮꾪솢?깊솕).
// ?몃? .rc 由ъ냼???섏〈 X ??肄붾뱶留뚯쑝濡?諛뺥엺??

#include <windows.h>
#include <commctrl.h>
#include <string>

namespace orange {

namespace detail_namedlg {

constexpr wchar_t kClassName[] = L"OrangeNameInputDialog";
constexpr int    kIdcEdit     = 1001;
constexpr int    kIdcOk       = 1002;
constexpr int    kIdcCancel   = 1003;
constexpr int    kIdcLabel    = 1004;

struct DlgState {
    std::wstring  prompt;
    std::wstring  result;
    std::wstring  initialText;   // ?쒖옉 ??EDIT ??誘몃━ 梨꾩슱 ?띿뒪??(?몄쭛 ?쒖젏???ъ슜).
    bool          confirmed = false;
};

inline LRESULT CALLBACK NameDlgProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_CREATE) {
        auto* cs = (CREATESTRUCTW*)lp;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
        auto* st = (DlgState*)cs->lpCreateParams;

        HINSTANCE hInst = (HINSTANCE)GetWindowLongPtrW(hwnd, GWLP_HINSTANCE);
        // ?쇰꺼 ???꾩そ
        CreateWindowExW(0, L"STATIC", st->prompt.c_str(),
            WS_CHILD | WS_VISIBLE | SS_LEFT,
            12, 12, 280, 18,
            hwnd, (HMENU)(LONG_PTR)kIdcLabel, hInst, nullptr);
        // EDIT ????以? ?쒖옉 ?띿뒪?멸? ?덉쑝硫?諛뺢퀬 ?꾩껜 selection ?쇰줈 ?ъ슜?먭? 利됱떆 ??뼱?곌린 媛?ν븯寃?
        HWND hEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT",
            st->initialText.c_str(),
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
            12, 36, 280, 24,
            hwnd, (HMENU)(LONG_PTR)kIdcEdit, hInst, nullptr);
        // OK
        CreateWindowExW(0, L"BUTTON", L"?뺤씤",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
            136, 72, 76, 28,
            hwnd, (HMENU)(LONG_PTR)kIdcOk, hInst, nullptr);
        // Cancel
        CreateWindowExW(0, L"BUTTON", L"痍⑥냼",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
            218, 72, 76, 28,
            hwnd, (HMENU)(LONG_PTR)kIdcCancel, hInst, nullptr);

        // ?고듃 ???쒖뒪??硫붿떆吏 ?고듃 (?쒓뎅??吏??蹂댁옣)
        NONCLIENTMETRICSW ncm{}; ncm.cbSize = sizeof(ncm);
        SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
        HFONT hFont = CreateFontIndirectW(&ncm.lfMessageFont);
        EnumChildWindows(hwnd, [](HWND ch, LPARAM lp) -> BOOL {
            SendMessageW(ch, WM_SETFONT, (WPARAM)lp, TRUE);
            return TRUE;
        }, (LPARAM)hFont);
        SetWindowLongPtrW(hwnd, (int)(GWLP_USERDATA + sizeof(LONG_PTR)), (LONG_PTR)hFont);

        SetFocus(hEdit);
        // ?쒖옉 ?띿뒪?멸? ?덉쑝硫??꾩껜 ?좏깮???ъ슜?먭? 怨㏃옣 ??뼱?곌린 媛??
        if (!st->initialText.empty()) {
            SendMessageW(hEdit, EM_SETSEL, 0, -1);
        }
        return 0;
    }
    if (msg == WM_COMMAND) {
        auto* st = (DlgState*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
        if (LOWORD(wp) == kIdcOk || (LOWORD(wp) == kIdcEdit && HIWORD(wp) == EN_CHANGE)) {
            // EN_CHANGE ??臾댁떆 (?뺤씤 踰꾪듉 ?쒖꽦/鍮꾪솢??遺꾧린 ?놁씠 ?⑥닚)
        }
        if (LOWORD(wp) == kIdcOk) {
            HWND hEdit = GetDlgItem(hwnd, kIdcEdit);
            int len = GetWindowTextLengthW(hEdit);
            std::wstring buf(len, L'\0');
            if (len > 0) GetWindowTextW(hEdit, buf.data(), len + 1);
            // trim 怨듬갚
            size_t b = 0, e = buf.size();
            while (b < e && (buf[b] == L' ' || buf[b] == L'\t')) ++b;
            while (e > b && (buf[e - 1] == L' ' || buf[e - 1] == L'\t')) --e;
            st->result = buf.substr(b, e - b);
            st->confirmed = !st->result.empty();
            DestroyWindow(hwnd);
            return 0;
        }
        if (LOWORD(wp) == kIdcCancel) {
            st->confirmed = false;
            st->result.clear();
            DestroyWindow(hwnd);
            return 0;
        }
    }
    if (msg == WM_CLOSE) {
        DestroyWindow(hwnd);
        return 0;
    }
    if (msg == WM_DESTROY) {
        // ?고듃 ?댁젣
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
    wc.lpfnWndProc   = NameDlgProc;
    wc.hInstance     = hInst;
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = kClassName;
    wc.cbWndExtra    = sizeof(LONG_PTR) * 2;  // [DlgState*, HFONT]
    RegisterClassExW(&wc);
    done = true;
}

}  // namespace detail_namedlg

// ?щ윭 以??띿뒪???낅젰 modal dialog. 鍮?臾몄옄??= 痍⑥냼.
// purpose / criteria / 湲??명듃 ????以꾨줈 ???닿린???먮━?먯꽌 ?ъ슜.
// ?먯껜 ?덈룄???대옒???좎꽕 (NameInputDialog ??EDIT 媛 ??以?ES_AUTOHSCROLL ??遺꾨━).
inline std::wstring PromptForLongText(HWND parent, const wchar_t* title, const wchar_t* prompt,
                                       const wchar_t* initialText = L"") {
    constexpr int dlgW = 480, dlgH = 300;
    constexpr wchar_t kLongClass[] = L"OrangeLongTextDialog";

    struct State {
        std::wstring  prompt;
        std::wstring  result;
        std::wstring  initialText;
        bool          confirmed = false;
    };

    static bool sClassRegistered = false;
    HINSTANCE hInst = parent ? (HINSTANCE)GetWindowLongPtrW(parent, GWLP_HINSTANCE)
                              : GetModuleHandleW(nullptr);

    auto Proc = +[](HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) -> LRESULT {
        if (msg == WM_CREATE) {
            auto* cs = (CREATESTRUCTW*)lp;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
            auto* st = (State*)cs->lpCreateParams;

            HINSTANCE hi = (HINSTANCE)GetWindowLongPtrW(hwnd, GWLP_HINSTANCE);
            CreateWindowExW(0, L"STATIC", st->prompt.c_str(),
                WS_CHILD | WS_VISIBLE | SS_LEFT,
                12, 12, 440, 18,
                hwnd, (HMENU)(LONG_PTR)1004, hi, nullptr);
            HWND hEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT",
                st->initialText.c_str(),
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL |
                ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN,
                12, 36, 440, 200,
                hwnd, (HMENU)(LONG_PTR)1001, hi, nullptr);
            CreateWindowExW(0, L"BUTTON", L"?뺤씤",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                296, 248, 76, 28,
                hwnd, (HMENU)(LONG_PTR)1002, hi, nullptr);
            CreateWindowExW(0, L"BUTTON", L"痍⑥냼",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                378, 248, 76, 28,
                hwnd, (HMENU)(LONG_PTR)1003, hi, nullptr);

            // ?고듃 ???쒖뒪??硫붿떆吏 ?고듃 (?쒓뎅??蹂댁옣).
            NONCLIENTMETRICSW ncm{}; ncm.cbSize = sizeof(ncm);
            SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
            HFONT hFont = CreateFontIndirectW(&ncm.lfMessageFont);
            EnumChildWindows(hwnd, [](HWND ch, LPARAM lp) -> BOOL {
                SendMessageW(ch, WM_SETFONT, (WPARAM)lp, TRUE);
                return TRUE;
            }, (LPARAM)hFont);
            SetWindowLongPtrW(hwnd, (int)(GWLP_USERDATA + sizeof(LONG_PTR)), (LONG_PTR)hFont);

            SetFocus(hEdit);
            if (!st->initialText.empty()) {
                SendMessageW(hEdit, EM_SETSEL, 0, -1);
            }
            return 0;
        }
        if (msg == WM_COMMAND) {
            auto* st = (State*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
            if (LOWORD(wp) == 1002) {  // ?뺤씤
                HWND hEdit = GetDlgItem(hwnd, 1001);
                int len = GetWindowTextLengthW(hEdit);
                std::wstring buf(len, L'\0');
                if (len > 0) GetWindowTextW(hEdit, buf.data(), len + 1);
                st->result = buf;
                st->confirmed = true;
                DestroyWindow(hwnd);
                return 0;
            }
            if (LOWORD(wp) == 1003) {  // 痍⑥냼
                st->confirmed = false;
                st->result.clear();
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
    };

    if (!sClassRegistered) {
        WNDCLASSEXW wc{};
        wc.cbSize        = sizeof(wc);
        wc.style         = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc   = Proc;
        wc.hInstance     = hInst;
        wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = kLongClass;
        wc.cbWndExtra    = sizeof(LONG_PTR) * 2;
        if (RegisterClassExW(&wc)) sClassRegistered = true;
    }

    State st;
    st.prompt      = prompt ? prompt : L"?댁슜";
    st.initialText = initialText ? initialText : L"";

    RECT pr; GetClientRect(parent ? parent : GetDesktopWindow(), &pr);
    POINT center = { (pr.right - pr.left) / 2, (pr.bottom - pr.top) / 2 };
    if (parent) ClientToScreen(parent, &center);
    int x = center.x - dlgW / 2;
    int y = center.y - dlgH / 2;

    if (parent) EnableWindow(parent, FALSE);

    HWND dlg = CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_TOPMOST,
        kLongClass, title ? title : L"?댁슜 ?몄쭛",
        WS_CAPTION | WS_SYSMENU | WS_POPUP | WS_VISIBLE,
        x, y, dlgW, dlgH,
        parent, nullptr, hInst, &st);

    if (dlg) {
        MSG msg;
        while (GetMessageW(&msg, nullptr, 0, 0) > 0 && IsWindow(dlg)) {
            // multi-line EDIT ?먯꽑 Enter 媛 以꾨컮轅??먯꽭??dialog ?먯꽭?먯꽌 Enter ?⑥텞?ㅻ뒗 諛뺤? ?딆쓬.
            // Esc 留?痍⑥냼濡?
            if (msg.message == WM_KEYDOWN && msg.wParam == VK_ESCAPE) {
                SendMessageW(dlg, WM_COMMAND, 1003, 0);
                continue;
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
    return st.confirmed ? st.result : std::wstring();
}

// ??以??대쫫 ?낅젰 modal dialog ?꾩?. 鍮?臾몄옄??= 痍⑥냼.
// initialText 媛 ?덉쑝硫?EDIT ??誘몃━ 梨꾩슦怨??꾩껜 ?좏깮 ???몄쭛 ?먯꽭 (?? 硫뷀? ?대쫫 蹂寃? ???ъ슜.
inline std::wstring PromptForName(HWND parent, const wchar_t* title, const wchar_t* prompt,
                                    const wchar_t* initialText = L"") {
    HINSTANCE hInst = parent ? (HINSTANCE)GetWindowLongPtrW(parent, GWLP_HINSTANCE)
                              : GetModuleHandleW(nullptr);
    detail_namedlg::EnsureClassRegistered(hInst);

    detail_namedlg::DlgState st;
    st.prompt = prompt ? prompt : L"?대쫫";
    st.initialText = initialText ? initialText : L"";

    // 遺紐??붾㈃ 以묒븰 ?꾩튂 怨꾩궛 (parent ?놁쑝硫??곗뒪?ы넲 以묒븰).
    constexpr int dlgW = 320, dlgH = 144;
    RECT pr; GetClientRect(parent ? parent : GetDesktopWindow(), &pr);
    POINT center = { (pr.right - pr.left) / 2, (pr.bottom - pr.top) / 2 };
    if (parent) ClientToScreen(parent, &center);
    int x = center.x - dlgW / 2;
    int y = center.y - dlgH / 2;

    // parent 鍮꾪솢?깊솕 ??紐⑤떖 寃?
    if (parent) EnableWindow(parent, FALSE);

    HWND dlg = CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_TOPMOST,
        detail_namedlg::kClassName, title ? title : L"?대쫫 ?낅젰",
        WS_CAPTION | WS_SYSMENU | WS_POPUP | WS_VISIBLE,
        x, y, dlgW, dlgH,
        parent, nullptr, hInst, &st);

    // 紐⑤떖 硫붿떆吏 猷⑦봽 ??DestroyWindow ??WM_DESTROY ??PostQuitMessage 濡?醫낅즺.
    if (dlg) {
        MSG msg;
        while (GetMessageW(&msg, nullptr, 0, 0) > 0 && IsWindow(dlg)) {
            // Tab/Esc/Enter ?ㅻ낫??寃곗쓣 dialog 寃곕줈 泥섎━
            if (msg.message == WM_KEYDOWN) {
                if (msg.wParam == VK_ESCAPE) {
                    SendMessageW(dlg, WM_COMMAND, detail_namedlg::kIdcCancel, 0);
                    continue;
                }
                if (msg.wParam == VK_RETURN) {
                    SendMessageW(dlg, WM_COMMAND, detail_namedlg::kIdcOk, 0);
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
    return st.confirmed ? st.result : std::wstring();
}

}  // namespace orange
