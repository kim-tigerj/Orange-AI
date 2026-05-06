#pragma once

// Capture ???꾩쓽 HWND 瑜?PrintWindow + WIC 濡?PNG ???
//
// ?먭? 寃利??꾧뎄. ?뺥??μ씠 ?먭린 Code.exe ?붾㈃??吏곸젒 罹≪퀜??Read ?꾧뎄濡??뺤씤 ??寃利?猷⑦봽媛
// ?뺥????덉뿉???ロ엺?? PowerShell + .NET ?섏〈??鍮쇨퀬 ?⑥씪 exe 濡??앸궡湲??꾪븿.
//
// ?숈옉:
//   1. GetWindowRect ??32bpp top-down DIB ?뱀뀡 ?앹꽦
//   2. PrintWindow(hwnd, memDC, PW_RENDERFULLCONTENT) ??媛?ㅼ졇 ?덉뼱???숈옉, ?붾㈃ ??鍮쇱븮??//   3. WIC IImagingFactory::CreateBitmapFromHBITMAP ??IWICBitmapEncoder(PNG) 濡??뚯씪 ???//
// ?섏〈: windowscodecs.lib, ole32.lib (????main.cpp ?먯꽌 #pragma comment 濡?留곹겕).

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wincodec.h>
#include <combaseapi.h>
#include <string>

#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")

namespace orange {

// ?깃났 ??S_OK. ?ㅽ뙣 ??HRESULT ?먮뒗 E_FAIL.
inline HRESULT CaptureWindowToPng(HWND hwnd, const std::wstring& outPath) {
    if (!hwnd || !IsWindow(hwnd)) return E_INVALIDARG;

    RECT rc{};
    if (!GetWindowRect(hwnd, &rc)) return HRESULT_FROM_WIN32(GetLastError());
    int w = rc.right - rc.left;
    int h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0) return E_INVALIDARG;

    HDC hdcScreen = GetDC(nullptr);
    HDC hdcMem    = CreateCompatibleDC(hdcScreen);
    if (!hdcMem) {
        ReleaseDC(nullptr, hdcScreen);
        return HRESULT_FROM_WIN32(GetLastError());
    }

    BITMAPINFO bi{};
    bi.bmiHeader.biSize        = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth       = w;
    bi.bmiHeader.biHeight      = -h;             // top-down
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    void* pixels = nullptr;
    HBITMAP hbm = CreateDIBSection(hdcMem, &bi, DIB_RGB_COLORS, &pixels, nullptr, 0);
    if (!hbm) {
        DeleteDC(hdcMem);
        ReleaseDC(nullptr, hdcScreen);
        return HRESULT_FROM_WIN32(GetLastError());
    }

    HGDIOBJ oldBm = SelectObject(hdcMem, hbm);
    // PW_RENDERFULLCONTENT (0x2) ??DWM ?⑹꽦???ㅼ젣 肄섑뀗痢?(Direct2D ??
    BOOL pwOk = PrintWindow(hwnd, hdcMem, 0x2);
    SelectObject(hdcMem, oldBm);

    HRESULT hr = pwOk ? S_OK : E_FAIL;

    if (SUCCEEDED(hr)) {
        // CoInitializeEx ???대? 珥덇린?????덉뼱??RPC_E_CHANGED_MODE 硫?洹몃?濡?吏꾪뻾.
        HRESULT coHr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        bool needUninit = SUCCEEDED(coHr);

        IWICImagingFactory*     factory = nullptr;
        IWICBitmap*             wicBmp  = nullptr;
        IWICStream*             stream  = nullptr;
        IWICBitmapEncoder*      encoder = nullptr;
        IWICBitmapFrameEncode*  frame   = nullptr;
        IPropertyBag2*          props   = nullptr;

        hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                              CLSCTX_INPROC_SERVER,
                              IID_PPV_ARGS(&factory));
        if (SUCCEEDED(hr)) hr = factory->CreateBitmapFromHBITMAP(
                                    hbm, nullptr, WICBitmapIgnoreAlpha, &wicBmp);
        if (SUCCEEDED(hr)) hr = factory->CreateStream(&stream);
        if (SUCCEEDED(hr)) hr = stream->InitializeFromFilename(outPath.c_str(), GENERIC_WRITE);
        if (SUCCEEDED(hr)) hr = factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder);
        if (SUCCEEDED(hr)) hr = encoder->Initialize(stream, WICBitmapEncoderNoCache);
        if (SUCCEEDED(hr)) hr = encoder->CreateNewFrame(&frame, &props);
        if (SUCCEEDED(hr)) hr = frame->Initialize(props);
        if (SUCCEEDED(hr)) hr = frame->SetSize((UINT)w, (UINT)h);

        WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGR;
        if (SUCCEEDED(hr)) hr = frame->SetPixelFormat(&fmt);
        if (SUCCEEDED(hr)) hr = frame->WriteSource(wicBmp, nullptr);
        if (SUCCEEDED(hr)) hr = frame->Commit();
        if (SUCCEEDED(hr)) hr = encoder->Commit();

        if (props)   props->Release();
        if (frame)   frame->Release();
        if (encoder) encoder->Release();
        if (stream)  stream->Release();
        if (wicBmp)  wicBmp->Release();
        if (factory) factory->Release();

        if (needUninit) CoUninitialize();
    }

    DeleteObject(hbm);
    DeleteDC(hdcMem);
    ReleaseDC(nullptr, hdcScreen);
    return hr;
}

// PID ??硫붿씤 ?덈룄???대옒?ㅻ챸 ?쇱튂) 瑜?李얜뒗?? ?놁쑝硫?nullptr.
inline HWND FindMainWindowOfPid(DWORD pid, const wchar_t* className) {
    struct Ctx { DWORD pid; const wchar_t* cls; HWND found; };
    Ctx ctx{ pid, className, nullptr };
    EnumWindows([](HWND h, LPARAM lp) -> BOOL {
        auto* c = reinterpret_cast<Ctx*>(lp);
        DWORD p = 0;
        GetWindowThreadProcessId(h, &p);
        if (p != c->pid) return TRUE;
        if (c->cls) {
            wchar_t cls[64] = L"";
            GetClassNameW(h, cls, 64);
            if (wcscmp(cls, c->cls) != 0) return TRUE;
        }
        c->found = h;
        return FALSE;
    }, (LPARAM)&ctx);
    return ctx.found;
}

}  // namespace orange
