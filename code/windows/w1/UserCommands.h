#pragma once

// CUserCommands — 사용자 명령 영속 큐 (USER_COMMANDS.md) 헬퍼.
//
// main.cpp 의 DispatchPrompt 가 매 사용자 입력을 자동으로 pending 항목으로 prepend.
// 정팀장은 매 spawn 시 USER_COMMANDS.md 를 읽고 pending 명령 최우선 처리.
//
// spawn 끊김·인스턴스 죽음에도 *처리 안 된 명령은 디스크에 남음* — 다음 정팀장 자동 이어받음.

#include <windows.h>
#include <fstream>
#include <sstream>
#include <string>

#include "Utils.h"

namespace orange {

class CUserCommands {
public:
    // USER_COMMANDS.md 경로. ORANGE_CODE_ROOT 기반.
    static std::wstring FilePath() {
        wchar_t buf[MAX_PATH] = L"";
        DWORD got = GetEnvironmentVariableW(L"ORANGE_CODE_ROOT", buf, MAX_PATH);
        if (got == 0 || got >= MAX_PATH) {
            // fallback: 현재 작업 디렉토리.
            return L"USER_COMMANDS.md";
        }
        return std::wstring(buf) + L"\\USER_COMMANDS.md";
    }

    // 새 사용자 명령을 pending 항목으로 prepend (최신이 위).
    // promptUtf8: 사용자 원문 (UTF-8). 첫 줄을 제목으로 추출, 본문 전체 그대로 누적.
    static bool AppendPending(const std::string& promptUtf8) {
        if (promptUtf8.empty()) return false;
        std::wstring path = FilePath();
        std::string utf8path = WideToUtf8(path);

        // 제목 = 첫 줄 (최대 80자). 본문 = 원문 전체.
        std::string title = promptUtf8;
        size_t nl = title.find('\n');
        if (nl != std::string::npos) title.resize(nl);
        if (title.size() > 80) title.resize(80);

        // 시각 — UTC ISO 짧게.
        SYSTEMTIME st;
        GetSystemTime(&st);
        char tsBuf[40];
        sprintf_s(tsBuf, "%04d-%02d-%02d %02d:%02d",
                  st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute);

        std::string entry;
        entry += "## ";
        entry += tsBuf;
        entry += " — pending — ";
        entry += title;
        entry += "\n\n";
        entry += promptUtf8;
        entry += "\n\n";

        // 기존 파일 읽기.
        std::string existing;
        {
            std::ifstream ifs(utf8path, std::ios::binary);
            if (ifs.is_open()) {
                std::stringstream buf;
                buf << ifs.rdbuf();
                existing = buf.str();
            }
        }

        // sep ("---") 다음 자리에 prepend (최신이 위).
        const std::string sep = "---";
        std::string out;
        if (!existing.empty()) {
            size_t sepPos = existing.find(sep);
            if (sepPos != std::string::npos) {
                size_t after = existing.find('\n', sepPos);
                if (after == std::string::npos) after = existing.size();
                else                            after += 1;  // 줄바꿈 다음
                // 빈 줄 흡수.
                while (after < existing.size() && existing[after] == '\n') ++after;
                out  = existing.substr(0, after);
                if (!out.empty() && out.back() != '\n') out += '\n';
                out += entry;
                out += existing.substr(after);
            } else {
                out  = existing;
                if (!out.empty() && out.back() != '\n') out += '\n';
                out += "\n";
                out += entry;
            }
        } else {
            // 파일 없거나 비어있음 — 헤더만 박힌 빈 파일로 가정하고 entry 만 적음.
            out += "# Orange Code — 사용자 명령 영속 큐\n\n";
            out += "> 정팀장은 매 spawn 시 이 파일을 읽고 *pending* 명령을 최우선 처리.\n\n";
            out += "---\n\n";
            out += entry;
        }

        // atomic write.
        std::string tmpPath = utf8path + ".tmp";
        {
            std::ofstream ofs(tmpPath, std::ios::binary | std::ios::trunc);
            if (!ofs.is_open()) return false;
            ofs.write(out.data(), out.size());
            if (!ofs.good()) return false;
        }
        std::wstring wTmp = path + L".tmp";
        return ::MoveFileExW(wTmp.c_str(), path.c_str(),
                            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
    }
};

}  // namespace orange
