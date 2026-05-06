#pragma once

// MetaEditDialog ??紐⑺몴/?꾨줈?앺듃??硫뷀? (title/purpose/criteria/status/progress) 瑜?// ??dialog ?먯꽌 ?쒓볼踰덉뿉 ?몄쭛?⑸땲??
//
// 吏곸쟾 ?ъ씠?대뱾?먯꽌??PromptForName + PromptForLongText 媛 4媛??곗냽 (title ??purpose ??// criteria ??status YESNO ??progress) ?뺥깭濡??ъ슜??遺?댁씠 而몄뒿?덈떎. 蹂?dialog ????// 臾띠쓬?쇰줈 紐⑤뱺 ?먮━瑜?蹂댁뿬 以띾땲??
//
// ?ъ슜:
//   orange::MetaEditValues v;
//   v.title    = L"湲곗〈 ?쒕ぉ";
//   v.purpose  = L"湲곗〈 紐⑹쟻";
//   v.criteria = L"湲곗〈 ?됯? 湲곗?";
//   v.status   = CWorkStatus::InProgress;
//   v.progress = 35;
//   if (orange::EditMeta(parent, L"紐⑺몴 硫뷀? ?몄쭛", &v)) {
//       // ?ъ슜?먭? ?뺤씤 ??v 媛 ??媛믪쑝濡?媛깆떊??
//   }

#include <windows.h>
#include <commctrl.h>
#include <string>
#include "Goal.h"  // CWorkStatus ?뺤쓽

namespace orange {

struct MetaEditValues {
    std::wstring  title;
    std::wstring  purpose;
    std::wstring  criteria;
    CWorkStatus   status   = CWorkStatus::Planning;
    CVerdict      verdict  = CVerdict::None;
    int           progress = 0;
    std::wstring  lastEvaluatedIso;  // ?쒖떆 ?꾩슜 ??dialog 媛 *留덉?留??됯? ?쒓컖* ?쇰꺼濡?蹂댁뿬以띾땲??
};

namespace detail_metadlg {

constexpr wchar_t kClassName[] = L"OrangeMetaEditDialog";
constexpr int    kIdcTitle    = 1101;
constexpr int    kIdcPurpose  = 1102;
constexpr int    kIdcCriteria = 1103;
constexpr int    kIdcStatus   = 1104;
constexpr int    kIdcProgress = 1105;
constexpr int    kIdcVerdict  = 1108;
constexpr int    kIdcOk       = 1106;
constexpr int    kIdcCancel   = 1107;

struct DlgState {
    MetaEditValues  current;
    MetaEditValues  edited;
    bool            confirmed = false;
};

inline LRESULT CALLBACK MetaProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_CREATE) {
        auto* cs = (CREATESTRUCTW*)lp;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
        auto* st = (DlgState*)cs->lpCreateParams;

        HINSTANCE hi = (HINSTANCE)GetWindowLongPtrW(hwnd, GWLP_HINSTANCE);

        const int W = 540, marginX = 14;
        int y = 14;

        // ?쒕ぉ
        CreateWindowExW(0, L"STATIC", L"?쒕ぉ",
            WS_CHILD | WS_VISIBLE | SS_LEFT,
            marginX, y, 80, 18, hwnd, nullptr, hi, nullptr);
        CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", st->current.title.c_str(),
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
            marginX + 80, y - 2, W - marginX * 2 - 80, 24,
            hwnd, (HMENU)(LONG_PTR)kIdcTitle, hi, nullptr);
        y += 32;

        // 紐⑹쟻
        CreateWindowExW(0, L"STATIC", L"紐⑹쟻 (purpose)",
            WS_CHILD | WS_VISIBLE | SS_LEFT,
            marginX, y, 200, 18, hwnd, nullptr, hi, nullptr);
        y += 18;
        CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", st->current.purpose.c_str(),
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL |
            ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN,
            marginX, y, W - marginX * 2, 100,
            hwnd, (HMENU)(LONG_PTR)kIdcPurpose, hi, nullptr);
        y += 110;

        // ?됯? 湲곗?
        CreateWindowExW(0, L"STATIC", L"?됯? 湲곗? (criteria)",
            WS_CHILD | WS_VISIBLE | SS_LEFT,
            marginX, y, 200, 18, hwnd, nullptr, hi, nullptr);
        y += 18;
        CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", st->current.criteria.c_str(),
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL |
            ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN,
            marginX, y, W - marginX * 2, 100,
            hwnd, (HMENU)(LONG_PTR)kIdcCriteria, hi, nullptr);
        y += 110;

        // 吏꾪뻾 ?곹깭 ??肄ㅻ낫
        CreateWindowExW(0, L"STATIC", L"吏꾪뻾 ?곹깭",
            WS_CHILD | WS_VISIBLE | SS_LEFT,
            marginX, y, 80, 18, hwnd, nullptr, hi, nullptr);
        HWND hCombo = CreateWindowExW(0, WC_COMBOBOXW, L"",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
            marginX + 80, y - 2, 160, 200,
            hwnd, (HMENU)(LONG_PTR)kIdcStatus, hi, nullptr);
        const wchar_t* labels[6] = {
            L"planning", L"in_progress", L"blocked",
            L"paused", L"done", L"abandoned"
        };
        for (int i = 0; i < 6; ++i) {
            SendMessageW(hCombo, CB_ADDSTRING, 0, (LPARAM)labels[i]);
        }
        SendMessageW(hCombo, CB_SETCURSEL, (WPARAM)st->current.status, 0);

        // 吏꾩쿃
        CreateWindowExW(0, L"STATIC", L"吏꾩쿃 %",
            WS_CHILD | WS_VISIBLE | SS_LEFT,
            marginX + 260, y, 60, 18, hwnd, nullptr, hi, nullptr);
        wchar_t pBuf[16];
        swprintf_s(pBuf, L"%d", st->current.progress);
        CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", pBuf,
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL | ES_NUMBER,
            marginX + 320, y - 2, 80, 24,
            hwnd, (HMENU)(LONG_PTR)kIdcProgress, hi, nullptr);
        y += 36;

        // ?됯? ?먯젙 ??蹂?肄ㅻ낫 (none/pass/partial/weak/fail).
        CreateWindowExW(0, L"STATIC", L"?됯? ?먯젙",
            WS_CHILD | WS_VISIBLE | SS_LEFT,
            marginX, y, 80, 18, hwnd, nullptr, hi, nullptr);
        HWND hVerdict = CreateWindowExW(0, WC_COMBOBOXW, L"",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
            marginX + 80, y - 2, 160, 200,
            hwnd, (HMENU)(LONG_PTR)kIdcVerdict, hi, nullptr);
        const wchar_t* verdictLabels[5] = {
            L"none", L"pass", L"partial", L"weak", L"fail"
        };
        for (int i = 0; i < 5; ++i) {
            SendMessageW(hVerdict, CB_ADDSTRING, 0, (LPARAM)verdictLabels[i]);
        }
        SendMessageW(hVerdict, CB_SETCURSEL, (WPARAM)st->current.verdict, 0);
        y += 36;

        // 留덉?留??됯? ?쒓컖 ???쒖떆 ?꾩슜 ?쇰꺼. 鍮꾩뼱 ?덉쑝硫?*?놁쓬*.
        {
            std::wstring labelText = L"留덉?留??됯?: ";
            labelText += st->current.lastEvaluatedIso.empty()
                         ? std::wstring(L"?놁쓬")
                         : st->current.lastEvaluatedIso;
            CreateWindowExW(0, L"STATIC", labelText.c_str(),
                WS_CHILD | WS_VISIBLE | SS_LEFT,
                marginX, y, W - marginX * 2, 18, hwnd, nullptr, hi, nullptr);
        }
        y += 24;

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
            // 紐⑤뱺 而⑦듃濡?媛??섏쭛.
            auto getText = [&](int id) {
                HWND h = GetDlgItem(hwnd, id);
                int len = GetWindowTextLengthW(h);
                std::wstring buf(len, L'\0');
                if (len > 0) GetWindowTextW(h, buf.data(), len + 1);
                return buf;
            };
            st->edited.title    = getText(kIdcTitle);
            st->edited.purpose  = getText(kIdcPurpose);
            st->edited.criteria = getText(kIdcCriteria);
            int sel = (int)SendMessageW(GetDlgItem(hwnd, kIdcStatus), CB_GETCURSEL, 0, 0);
            if (sel < 0 || sel > 5) sel = (int)st->current.status;
            st->edited.status = (CWorkStatus)sel;
            int vsel = (int)SendMessageW(GetDlgItem(hwnd, kIdcVerdict), CB_GETCURSEL, 0, 0);
            if (vsel < 0 || vsel > 4) vsel = (int)st->current.verdict;
            st->edited.verdict = (CVerdict)vsel;
            int pv = _wtoi(getText(kIdcProgress).c_str());
            if (pv < 0) pv = 0;
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
    wc.lpfnWndProc   = MetaProc;
    wc.hInstance     = hInst;
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = kClassName;
    wc.cbWndExtra    = sizeof(LONG_PTR) * 2;
    if (RegisterClassExW(&wc)) done = true;
}

}  // namespace detail_metadlg

// 硫뷀? ?몄쭛 dialog ?꾩?. ?ъ슜?먭? ?뺤씤?섎㈃ *out ??紐⑤뱺 ?꾨뱶瑜???媛믪쑝濡?媛깆떊?섍퀬 true 諛섑솚.
// 痍⑥냼硫?*out 洹몃?濡? false 諛섑솚.
inline bool EditMeta(HWND parent, const wchar_t* title, MetaEditValues* inout) {
    if (!inout) return false;
    HINSTANCE hInst = parent ? (HINSTANCE)GetWindowLongPtrW(parent, GWLP_HINSTANCE)
                              : GetModuleHandleW(nullptr);
    detail_metadlg::EnsureClassRegistered(hInst);

    detail_metadlg::DlgState st;
    st.current = *inout;

    constexpr int dlgW = 540, dlgH = 524;  // verdict 肄ㅻ낫 以?異붽?濡?36px ??
    RECT pr; GetClientRect(parent ? parent : GetDesktopWindow(), &pr);
    POINT center = { (pr.right - pr.left) / 2, (pr.bottom - pr.top) / 2 };
    if (parent) ClientToScreen(parent, &center);
    int x = center.x - dlgW / 2;
    int y = center.y - dlgH / 2;

    if (parent) EnableWindow(parent, FALSE);

    HWND dlg = CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_TOPMOST,
        detail_metadlg::kClassName, title ? title : L"硫뷀? ?몄쭛",
        WS_CAPTION | WS_SYSMENU | WS_POPUP | WS_VISIBLE,
        x, y, dlgW, dlgH,
        parent, nullptr, hInst, &st);

    if (dlg) {
        MSG msg;
        while (GetMessageW(&msg, nullptr, 0, 0) > 0 && IsWindow(dlg)) {
            // multi-line EDIT 媛 ?덉뼱 Enter ?⑥텞?ㅻ뒗 諛뺤? ?딆쓬. Esc 留?痍⑥냼.
            if (msg.message == WM_KEYDOWN && msg.wParam == VK_ESCAPE) {
                SendMessageW(dlg, WM_COMMAND, detail_metadlg::kIdcCancel, 0);
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
    if (st.confirmed) {
        *inout = st.edited;
        return true;
    }
    return false;
}

}  // namespace orange
