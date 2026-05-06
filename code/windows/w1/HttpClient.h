#pragma once

// HttpClient ???뉗? WinHTTP HTTPS ?섑띁.
// Phase 1 ?쒖젙: ?숆린/釉붾줈??POST 留? ?몄텧?먮뒗 ?뚯빱 ?ㅻ젅?쒖뿉???몄텧??寃?
// Phase 2?먯꽌 SSE ?ㅽ듃由щ컢 蹂??異붽? ?덉젙.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>
#include <string>

#pragma comment(lib, "winhttp.lib")

namespace orange {

class HttpClient {
public:
    struct Response {
        bool        ok      = false;
        DWORD       status  = 0;     // HTTP ?곹깭 肄붾뱶
        std::string body;             // UTF-8 ?묐떟 諛붾뵒
        std::string error;            // ?ㅻ쪟 ??梨꾩썙吏?    };

    // HTTPS POST. ?묐떟 ?꾩껜瑜?硫붾え由ъ뿉 紐⑥븘 由ы꽩.
    //   host         : "api.anthropic.com"
    //   path         : "/v1/messages"
    //   bodyUtf8     : ?붿껌 諛붾뵒 (UTF-8 諛붿씠??
    //   extraHeaders : "Header1: val\r\nHeader2: val" (留덉?留?CRLF ?쒖쇅)
    static Response PostJson(
        const wchar_t*       host,
        const wchar_t*       path,
        const std::string&   bodyUtf8,
        const std::wstring&  extraHeaders,
        const wchar_t*       userAgent = L"OrangeCode/0.1")
    {
        Response  r;
        HINTERNET hSession = nullptr;
        HINTERNET hConnect = nullptr;
        HINTERNET hRequest = nullptr;

        auto cleanup = [&]() {
            if (hRequest) WinHttpCloseHandle(hRequest);
            if (hConnect) WinHttpCloseHandle(hConnect);
            if (hSession) WinHttpCloseHandle(hSession);
        };

        hSession = WinHttpOpen(userAgent,
                               WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                               WINHTTP_NO_PROXY_NAME,
                               WINHTTP_NO_PROXY_BYPASS,
                               0);
        if (!hSession) {
            r.error = "WinHttpOpen ?ㅽ뙣 (err=" + std::to_string(GetLastError()) + ")";
            cleanup();
            return r;
        }

        // ??꾩븘?? resolve / connect / send / receive (媛곴컖 ms)
        WinHttpSetTimeouts(hSession, 30000, 30000, 30000, 60000);

        hConnect = WinHttpConnect(hSession, host, INTERNET_DEFAULT_HTTPS_PORT, 0);
        if (!hConnect) {
            r.error = "WinHttpConnect ?ㅽ뙣 (err=" + std::to_string(GetLastError()) + ")";
            cleanup();
            return r;
        }

        hRequest = WinHttpOpenRequest(hConnect, L"POST", path, nullptr,
                                      WINHTTP_NO_REFERER,
                                      WINHTTP_DEFAULT_ACCEPT_TYPES,
                                      WINHTTP_FLAG_SECURE);
        if (!hRequest) {
            r.error = "WinHttpOpenRequest ?ㅽ뙣 (err=" + std::to_string(GetLastError()) + ")";
            cleanup();
            return r;
        }

        const wchar_t* headers   = extraHeaders.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS
                                                        : extraHeaders.c_str();
        DWORD           headerLen = extraHeaders.empty() ? 0 : (DWORD)extraHeaders.size();

        BOOL ok = WinHttpSendRequest(hRequest, headers, headerLen,
                                     (LPVOID)bodyUtf8.data(), (DWORD)bodyUtf8.size(),
                                     (DWORD)bodyUtf8.size(), 0);
        if (!ok) {
            r.error = "WinHttpSendRequest ?ㅽ뙣 (err=" + std::to_string(GetLastError()) + ")";
            cleanup();
            return r;
        }

        ok = WinHttpReceiveResponse(hRequest, nullptr);
        if (!ok) {
            r.error = "WinHttpReceiveResponse ?ㅽ뙣 (err=" + std::to_string(GetLastError()) + ")";
            cleanup();
            return r;
        }

        // ?곹깭 肄붾뱶
        DWORD statusCode = 0;
        DWORD statusSize = sizeof(statusCode);
        WinHttpQueryHeaders(hRequest,
                            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX,
                            &statusCode, &statusSize, WINHTTP_NO_HEADER_INDEX);
        r.status = statusCode;

        // 諛붾뵒 ?쎄린
        for (;;) {
            DWORD avail = 0;
            if (!WinHttpQueryDataAvailable(hRequest, &avail)) break;
            if (avail == 0) break;

            std::string chunk(avail, '\0');
            DWORD readBytes = 0;
            if (!WinHttpReadData(hRequest, chunk.data(), avail, &readBytes)) break;
            chunk.resize(readBytes);
            r.body += chunk;
        }

        r.ok = (statusCode >= 200 && statusCode < 300);
        if (!r.ok && r.error.empty()) {
            r.error = "HTTP " + std::to_string(statusCode);
        }
        cleanup();
        return r;
    }
};

}  // namespace orange
