#pragma once

// ClaudeCliBackend — claude cli (Anthropic Claude Code) 를 ConPTY 자식으로 띄워
// stdin/stdout 으로 양방향 대화. Max 구독 OAuth 인증 사용 시 토큰 비용 ≈ 0.

#include <windows.h>
#include <string>

#include "IClaudeBackend.h"
#include "CConPTY.h"
#include "Utils.h"

namespace orange {

class ClaudeCliBackend : public IBackend {
public:
    ClaudeCliBackend()           = default;
    ~ClaudeCliBackend() override { Stop(); }

    bool Start(OutputCallback outputCb,
               TurnDoneCallback /*turnDoneCb*/ = {}) override {
        m_outputCb = std::move(outputCb);

        if (!CConPTY::IsSupported()) {
            Emit(L"[이 Windows에서 ConPTY 미지원입니다. Win10 1809 이상 필요.]\r\n");
            return false;
        }

        ClaudeLocation cl = FindClaude();
        if (cl.path.empty()) {
            Emit(L"[claude 미발견. claude cli 설치 후 다시 실행하세요.]\r\n"
                 L"[설치: npm install -g @anthropic-ai/claude-code]\r\n");
            return false;
        }

        Emit(std::wstring(L"[claude 발견: ") + cl.path + L"]\r\n");

        COORD size = { 120, 40 };
        auto cb = [this](const char* d, DWORD l) -> bool {
            return OnConPtyData(d, l);
        };

        bool ok;
        if (cl.directExe) {
            ok = m_conPty.Create(cl.path.c_str(), nullptr, nullptr, size, cb);
        } else {
            std::wstring args = L"/c \"" + cl.path + L"\"";
            ok = m_conPty.Create(L"cmd.exe", args.c_str(), nullptr, size, cb);
        }

        if (!ok) {
            Emit(L"[claude 실행 실패. 경로 확인 후 재시도]\r\n");
            return false;
        }

        m_ready = true;
        return true;
    }

    bool SendPrompt(const std::wstring& prompt) override {
        if (!m_ready) return false;
        std::string utf8 = WideToUtf8(prompt) + "\r";  // ConPTY 는 CR 을 Enter 로 받음
        return m_conPty.Write(utf8.data(), (DWORD)utf8.size());
    }

    void Stop() override {
        if (m_ready) {
            m_conPty.Destroy();
            m_ready = false;
        }
    }

    bool IsReady() const override { return m_ready; }

private:
    struct ClaudeLocation {
        std::wstring path;
        bool         directExe;
    };

    static ClaudeLocation FindClaude() {
        auto isExe = [](const std::wstring& p) {
            if (p.size() < 4) return false;
            return _wcsicmp(p.c_str() + p.size() - 4, L".exe") == 0;
        };

        static const wchar_t* names[] = {
            L"claude.exe", L"claude.cmd", L"claude.bat"
        };

        // 1) PATH 검색
        for (auto* name : names) {
            wchar_t buf[MAX_PATH] = L"";
            DWORD got = SearchPathW(nullptr, name, nullptr, MAX_PATH, buf, nullptr);
            if (got > 0 && got < MAX_PATH) {
                return { std::wstring(buf), isExe(std::wstring(buf)) };
            }
        }

        // 2) %APPDATA%\npm\ — npm 전역 설치 기본 위치
        wchar_t appdata[MAX_PATH] = L"";
        if (GetEnvironmentVariableW(L"APPDATA", appdata, MAX_PATH) > 0) {
            for (auto* name : names) {
                std::wstring p = std::wstring(appdata) + L"\\npm\\" + name;
                if (GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES) {
                    return { p, isExe(p) };
                }
            }
        }

        // 3) %LOCALAPPDATA% 하위 흔한 Anthropic 설치 경로
        wchar_t localApp[MAX_PATH] = L"";
        if (GetEnvironmentVariableW(L"LOCALAPPDATA", localApp, MAX_PATH) > 0) {
            const wchar_t* tails[] = {
                L"\\Programs\\claude\\claude.exe",
                L"\\AnthropicClaude\\claude.exe",
                L"\\Anthropic\\Claude\\claude.exe",
            };
            for (auto* tail : tails) {
                std::wstring p = std::wstring(localApp) + tail;
                if (GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES) {
                    return { p, true };
                }
            }
        }

        return { {}, false };
    }

    // ANSI 이스케이프 시퀀스 제거 (CSI / OSC + 기타 C0 제어문자)
    static std::string StripAnsi(const char* data, DWORD len) {
        std::string out;
        out.reserve(len);
        for (DWORD i = 0; i < len; ++i) {
            unsigned char c = (unsigned char)data[i];
            if (c == 0x1B) {
                if (++i >= len) break;
                char next = data[i];
                if (next == '[') {
                    ++i;
                    while (i < len) {
                        unsigned char ch = (unsigned char)data[i];
                        if (ch >= 0x40 && ch <= 0x7E) break;
                        ++i;
                    }
                } else if (next == ']') {
                    ++i;
                    while (i < len) {
                        if (data[i] == 0x07) break;
                        if (data[i] == 0x1B && i + 1 < len && data[i + 1] == '\\') {
                            ++i;
                            break;
                        }
                        ++i;
                    }
                }
            } else if (c == '\r' || c == '\n' || c == '\t' || c >= 0x20) {
                out.push_back(data[i]);
            }
        }
        return out;
    }

    // UTF-8 멀티바이트 시퀀스가 완결된 마지막 위치 반환.
    // ReadFile 경계에서 한국어 같은 멀티바이트가 잘릴 때 버퍼링 위해 사용.
    static size_t FindUtf8SafeEnd(const std::string& buf) {
        size_t n = buf.size();
        if (n == 0) return 0;

        // 마지막 lead 바이트 위치 찾기 (continuation 바이트 거꾸로 스킵)
        size_t startOfLast = n;
        while (startOfLast > 0) {
            unsigned char c = (unsigned char)buf[startOfLast - 1];
            startOfLast--;
            if ((c & 0xC0) != 0x80) break;            // non-continuation
            if (n - startOfLast > 4) return n;        // 너무 많은 continuation = 손상, 그냥 처리
        }

        unsigned char lead = (unsigned char)buf[startOfLast];
        int seqLen;
        if (lead < 0x80)              seqLen = 1;
        else if ((lead & 0xE0) == 0xC0) seqLen = 2;
        else if ((lead & 0xF0) == 0xE0) seqLen = 3;
        else if ((lead & 0xF8) == 0xF0) seqLen = 4;
        else return n;                                // 손상된 lead, 그냥 처리

        return ((n - startOfLast) >= (size_t)seqLen) ? n : startOfLast;
    }

    bool OnConPtyData(const char* data, DWORD len) {
        if (data == nullptr) {
            // EOF — 남은 보류 바이트 강제 flush
            if (!m_pendingBytes.empty()) {
                std::string  stripped = StripAnsi(m_pendingBytes.data(),
                                                  (DWORD)m_pendingBytes.size());
                std::wstring wide     = Utf8ToWide(stripped.c_str(),
                                                   (int)stripped.size());
                std::wstring crlf     = NormalizeNewlines(wide);
                if (!crlf.empty()) Emit(std::move(crlf));
                m_pendingBytes.clear();
            }
            Emit(L"\r\n[claude 프로세스 종료됨]\r\n");
            m_ready = false;
            return false;
        }

        // 새 바이트를 보류 버퍼에 누적
        m_pendingBytes.append(data, len);

        // 완결된 UTF-8 시퀀스만 처리, 미완성은 다음 콜백까지 보류
        size_t safeEnd = FindUtf8SafeEnd(m_pendingBytes);
        if (safeEnd == 0) return true;

        std::string  stripped = StripAnsi(m_pendingBytes.data(), (DWORD)safeEnd);
        std::wstring wide     = Utf8ToWide(stripped.c_str(), (int)stripped.size());
        std::wstring crlf     = NormalizeNewlines(wide);
        if (!crlf.empty()) Emit(std::move(crlf));

        m_pendingBytes.erase(0, safeEnd);
        return true;
    }

    void Emit(std::wstring text) {
        if (m_outputCb) m_outputCb(std::move(text));
    }

    CConPTY        m_conPty;
    OutputCallback m_outputCb;
    bool           m_ready = false;
    std::string    m_pendingBytes;  // ConPTY chunk 경계의 미완성 UTF-8 바이트 보류
};

}  // namespace orange
