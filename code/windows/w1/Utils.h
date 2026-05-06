#pragma once

// 공통 유틸리티: 인코딩 변환, 줄바꿈 정규화

#include <windows.h>
#include <string>

namespace orange {

inline std::wstring Utf8ToWide(const char* data, int len) {
    if (len <= 0) return {};
    int wlen = MultiByteToWideChar(CP_UTF8, 0, data, len, nullptr, 0);
    std::wstring out(wlen, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, data, len, out.data(), wlen);
    return out;
}

inline std::string WideToUtf8(const std::wstring& w) {
    if (w.empty()) return "";
    int u8len = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(),
                                    nullptr, 0, nullptr, nullptr);
    std::string out(u8len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(),
                        out.data(), u8len, nullptr, nullptr);
    return out;
}

// 현재 exe 위치에서 위로 올라가며 CLAUDE.md 가 있는 디렉터리(=프로젝트 루트)를 찾는다.
// 못 찾으면 빈 문자열.
inline std::wstring FindProjectRoot() {
    wchar_t exePath[MAX_PATH] = L"";
    if (GetModuleFileNameW(nullptr, exePath, MAX_PATH) == 0) return L"";
    std::wstring path = exePath;
    for (int hop = 0; hop < 12; ++hop) {
        size_t lastSlash = path.find_last_of(L"\\/");
        if (lastSlash == std::wstring::npos) break;
        path.resize(lastSlash);
        std::wstring claudeMd = path + L"\\CLAUDE.md";
        if (GetFileAttributesW(claudeMd.c_str()) != INVALID_FILE_ATTRIBUTES) {
            return path;
        }
    }
    return L"";
}

// claude CLI 의 sessionId(UUID v4) 형식 검증. 36자 "8-4-4-4-12" 16진수 패턴.
// --resume 인자, 영속화된 session_id 필드, 도처에서 같은 정의 필요해 공통 위치로.
inline bool IsLikelyUuid(const std::string& s) {
    if (s.size() != 36) return false;
    for (size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        const bool isDashPos = (i == 8 || i == 13 || i == 18 || i == 23);
        if (isDashPos) {
            if (c != '-') return false;
        } else {
            const bool isHex =
                (c >= '0' && c <= '9') ||
                (c >= 'a' && c <= 'f') ||
                (c >= 'A' && c <= 'F');
            if (!isHex) return false;
        }
    }
    return true;
}

// 줄바꿈 정규화:
//  - LF → CRLF (Edit 컨트롤은 CRLF만 줄바꿈으로 인식)
//  - CRLF → 그대로
//  - 단독 CR → 제거 (터미널 cursor refresh 용. Edit 에선 의미 없음)
inline std::wstring NormalizeNewlines(const std::wstring& in) {
    std::wstring out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); ++i) {
        wchar_t c = in[i];
        if (c == L'\r') {
            if (i + 1 < in.size() && in[i + 1] == L'\n') {
                out += L"\r\n";
                ++i;
            }
            // 단독 CR 은 버림
        } else if (c == L'\n') {
            out += L"\r\n";
        } else {
            out += c;
        }
    }
    return out;
}

// MSBuild.exe 경로를 찾는다. build.sh 의 로직을 참고.
inline std::wstring FindMSBuildExePath() {
    std::wstring msbuildPath = L"C:\\Program Files\\Microsoft Visual Studio\\2022\\Professional\\MSBuild\\Current\\Bin\\MSBuild.exe";
    DWORD fileAttrs = GetFileAttributesW(msbuildPath.c_str());
    if (fileAttrs != INVALID_FILE_ATTRIBUTES && !(fileAttrs & FILE_ATTRIBUTE_DIRECTORY)) {
        return msbuildPath;
    }

    const std::wstring editions[] = { L"Community", L"Enterprise", L"BuildTools" };
    for (const auto& ed : editions) {
        msbuildPath = L"C:\\Program Files\\Microsoft Visual Studio\\2022\\" + ed + L"\\MSBuild\\Current\\Bin\\MSBuild.exe";
        fileAttrs = GetFileAttributesW(msbuildPath.c_str());
        if (fileAttrs != INVALID_FILE_ATTRIBUTES && !(fileAttrs & FILE_ATTRIBUTE_DIRECTORY)) {
            return msbuildPath;
        }
    }
    return L"";
}

}  // namespace orange
