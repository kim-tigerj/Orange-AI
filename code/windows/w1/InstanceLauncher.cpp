// CInstanceLauncher 구현. 자식 Code.exe spawn / 옛 인스턴스 정리 / sessionId 검증.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#include <stdio.h>
#include <vector>

#include "InstanceLauncher.h"
#include "TaskSpecValidator.h"

#pragma comment(lib, "shell32.lib")

namespace orange {

bool CInstanceLauncher::IsLikelyUuid(const std::string& s) {
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

std::wstring CInstanceLauncher::SelfModulePath() {
    wchar_t exePath[MAX_PATH] = L"";
    if (GetModuleFileNameW(nullptr, exePath, MAX_PATH) == 0) return {};
    return std::wstring(exePath);
}

std::wstring CInstanceLauncher::NewUniqueSessionKey() {
    wchar_t buf[32];
    swprintf_s(buf, L"new_%llu", (unsigned long long)GetTickCount64());
    return std::wstring(buf);
}

bool CInstanceLauncher::Spawn(const SpawnOptions& opts) {
    const std::wstring exePath = SelfModulePath();
    if (exePath.empty()) return false;

    std::wstring args;
    auto append = [&](const std::wstring& part) {
        if (!args.empty()) args += L" ";
        args += part;
    };

    if (opts.replacePid != 0) {
        wchar_t buf[16];
        swprintf_s(buf, L"%lu", opts.replacePid);
        append(std::wstring(L"--replace-pid ") + buf);
    }
    if (!opts.sessionKey.empty()) {
        append(std::wstring(L"--session \"") + opts.sessionKey + L"\"");
    }
    if (!opts.bootstrap.empty()) {
        // 따옴표 escape 책임은 호출자. 안에 " 가 들어가면 깨질 수 있음.
        append(std::wstring(L"--bootstrap \"") + opts.bootstrap + L"\"");
    }
    if (!opts.goalId.empty()) {
        append(std::wstring(L"--goal-id \"") + opts.goalId + L"\"");
    }
    if (!opts.projectId.empty()) {
        append(std::wstring(L"--project-id \"") + opts.projectId + L"\"");
    }

    HINSTANCE r = ShellExecuteW(nullptr, L"open", exePath.c_str(),
                                args.c_str(), nullptr, SW_SHOWNORMAL);
    return ((INT_PTR)r) > 32;
}

bool CInstanceLauncher::SpawnMember(const SpawnMemberOptions& opts,
                                    std::wstring* outErrorMessage) {
    auto setError = [outErrorMessage](const std::wstring& msg) {
        if (outErrorMessage) *outErrorMessage = msg;
    };

    const std::wstring exePath = SelfModulePath();
    if (exePath.empty()) {
        setError(L"OrangeCode.exe 경로를 알 수 없습니다.");
        return false;
    }
    if (opts.taskId.empty() || opts.taskSpecPath.empty()) {
        setError(L"taskId 와 taskSpecPath 는 필수입니다.");
        return false;
    }

    // 명세 검증 ? 정팀장의 *번역 역할* 시스템 강제.
    {
        // taskSpecPath 가 상대 경로면 ORANGE_CODE_ROOT 기준으로 해석.
        std::wstring resolvedPath = opts.taskSpecPath;
        if (resolvedPath.size() < 2 || (resolvedPath[1] != L':' && resolvedPath[0] != L'\\' && resolvedPath[0] != L'/')) {
            wchar_t rootBuf[MAX_PATH] = L"";
            if (GetEnvironmentVariableW(L"ORANGE_CODE_ROOT", rootBuf, MAX_PATH) > 0) {
                resolvedPath = std::wstring(rootBuf) + L"\\" + opts.taskSpecPath;
            }
        }
        auto result = CTaskSpecValidator::ValidateFile(resolvedPath);
        if (!result.ok) {
            setError(result.errorMessage);
            return false;
        }
    }

    // 부모(정팀장) PID 환경변수로 set ? 자식이 자기가 누구의 팀원인지 인지.
    // ShellExecute 가 부모 env 인계하므로 set 후 spawn.
    wchar_t parentBuf[16];
    swprintf_s(parentBuf, L"%lu", GetCurrentProcessId());
    SetEnvironmentVariableW(L"ORANGE_CODE_PARENT_PID", parentBuf);

    // 인자 빌드.
    std::wstring args = L"--role member --task-id \"" + opts.taskId + L"\""
                       L" --task-spec \"" + opts.taskSpecPath + L"\"";
    if (!opts.sessionKey.empty()) {
        args += L" --session \"" + opts.sessionKey + L"\"";
    } else {
        // 세션 키 없으면 task_id 그대로 (사람 친화 ? reports/<task_id>.md 와 매칭).
        args += L" --session \"" + opts.taskId + L"\"";
    }

    // bootstrap ? 자식이 자세 잡고 작업 명세 읽음.
    std::wstring bootstrap = L"당신은 팀원 인스턴스. CLAUDE.md §1 매 spawn 첫 행동 따라가되, 작업 명세는 "
                              L"$ORANGE_CODE_ROOT/" + opts.taskSpecPath +
                              L" 에 있다. 읽고 수행 후 결과를 reports/" + opts.taskId +
                              L".md 에 박은 뒤 MESSAGES.md 에 supervisor:host 와 manager:$ORANGE_CODE_PARENT_PID "
                              L"양쪽으로 완료 알림 (4축 자기 평가 같이).";
    args += L" --bootstrap \"" + bootstrap + L"\"";

    HINSTANCE r = ShellExecuteW(nullptr, L"open", exePath.c_str(),
                                args.c_str(), nullptr, SW_SHOWNORMAL);
    return ((INT_PTR)r) > 32;
}

bool CInstanceLauncher::SpawnSupervisor(const SpawnSupervisorOptions& opts) {
    const std::wstring exePath = SelfModulePath();
    if (exePath.empty()) return false;

    // 부모 PID 인계 ? 누가 오감독을 임명했는지 추적.
    wchar_t parentBuf[16];
    swprintf_s(parentBuf, L"%lu", GetCurrentProcessId());
    SetEnvironmentVariableW(L"ORANGE_CODE_PARENT_PID", parentBuf);

    // 세션 키 ? 없으면 sup_<tickcount>.
    std::wstring sessionKey = opts.sessionKey;
    if (sessionKey.empty()) {
        wchar_t buf[32];
        swprintf_s(buf, L"sup_%llu", (unsigned long long)GetTickCount64());
        sessionKey = buf;
    }

    std::wstring args = L"--role supervisor --session \"" + sessionKey + L"\"";

    // bootstrap ? 오감독 자세 명시. context hint 가 있으면 같이.
    std::wstring bootstrap =
        L"당신은 오감독 인스턴스. CLAUDE.md §1 매 spawn 첫 행동 따라가되 자세는 *4축 리뷰 종합·평가*. "
        L"정팀장이 임명한 자리. 호스트 오감독(supervisor:host) 또는 다른 오감독과 평행으로 작동 가능. "
        L"정팀장은 절대 오감독 역할 X ? 모든 4축 종합·평가는 오감독 인스턴스가 한다.";
    if (!opts.reason.empty()) {
        bootstrap += L" 임명 사유: " + opts.reason + L".";
    }
    if (!opts.contextHint.empty()) {
        bootstrap += L" 컨텍스트: " + opts.contextHint + L".";
    }
    args += L" --bootstrap \"" + bootstrap + L"\"";

    HINSTANCE r = ShellExecuteW(nullptr, L"open", exePath.c_str(),
                                args.c_str(), nullptr, SW_SHOWNORMAL);
    return ((INT_PTR)r) > 32;
}

void CInstanceLauncher::ReplaceOldInstance(DWORD oldPid, const wchar_t* windowClass) {
    if (oldPid == 0 || oldPid == GetCurrentProcessId()) return;

    // PROCESS_TERMINATE 도 같이 ? WAIT_TIMEOUT 시 강제 종료 fallback 에 필요.
    HANDLE hProc = OpenProcess(SYNCHRONIZE | PROCESS_TERMINATE, FALSE, oldPid);
    if (!hProc) return;

    struct EnumCtx {
        DWORD          pid;
        HWND           found;
        const wchar_t* cls;
    };
    EnumCtx ctx{ oldPid, nullptr, windowClass };
    EnumWindows([](HWND h, LPARAM lp) -> BOOL {
        DWORD pid = 0;
        GetWindowThreadProcessId(h, &pid);
        auto* c = reinterpret_cast<EnumCtx*>(lp);
        if (pid == c->pid) {
            wchar_t cls[64] = L"";
            GetClassNameW(h, cls, 64);
            if (wcscmp(cls, c->cls) == 0) {
                c->found = h;
                return FALSE;
            }
        }
        return TRUE;
    }, (LPARAM)&ctx);

    if (ctx.found) {
        PostMessageW(ctx.found, WM_CLOSE, 0, 0);
    } else {
        // 윈도우를 못 찾았더라도 PID 가 살아있다면 일단 WM_CLOSE 시도 (메시지 루프만 살아있을 수도 있음)
        // 사실 윈도우가 없으면 WM_CLOSE 는 의미 없으나, 안전 가드.
    }

    // 옛 인스턴스가 turn 진행 중 멎어 메시지 루프 막힌 경우 WM_CLOSE 처리 못 함 → 죽지 않음.
    // WAIT_TIMEOUT 이면 TerminateProcess 로 강제 정리해서 새 인스턴스와 *둘 다 떠있는* 사고 회피.
    // 5초는 사용자 입장에서 너무 길 수 있으므로 3초로 단축하되, 확실히 대기.
    DWORD waitResult = WaitForSingleObject(hProc, 3000);
    if (waitResult == WAIT_TIMEOUT) {
        // 여전히 살아있다면 무조건 강제 종료.
        TerminateProcess(hProc, 1);
        WaitForSingleObject(hProc, 1000);  // OS 가 자원 정리할 시간을 잠깐 줌
    }
    CloseHandle(hProc);
}

void CInstanceLauncher::CleanupOtherInstances(DWORD selfPid, const wchar_t* windowClass) {
    if (windowClass == nullptr || *windowClass == 0) return;

    struct EnumCtx {
        DWORD              self;
        const wchar_t*     cls;
        std::vector<HWND>  hwnds;
        std::vector<DWORD> pids;
    };
    EnumCtx ctx{ selfPid, windowClass, {}, {} };
    EnumWindows([](HWND h, LPARAM lp) -> BOOL {
        DWORD pid = 0;
        GetWindowThreadProcessId(h, &pid);
        auto* c = reinterpret_cast<EnumCtx*>(lp);
        if (pid == 0 || pid == c->self) return TRUE;
        wchar_t cls[64] = L"";
        GetClassNameW(h, cls, 64);
        if (wcscmp(cls, c->cls) == 0) {
            c->hwnds.push_back(h);
            c->pids.push_back(pid);
        }
        return TRUE;
    }, (LPARAM)&ctx);

    if (ctx.hwnds.empty()) return;

    // 1단계: 모두에게 동시에 WM_CLOSE ? 직렬 5N 초 대기를 회피.
    for (HWND h : ctx.hwnds) PostMessageW(h, WM_CLOSE, 0, 0);

    // 2단계: 각 PID OpenProcess + 합쳐 한 번 5초 대기.
    std::vector<HANDLE> handles;
    handles.reserve(ctx.pids.size());
    for (DWORD pid : ctx.pids) {
        HANDLE hProc = OpenProcess(SYNCHRONIZE | PROCESS_TERMINATE, FALSE, pid);
        if (hProc) handles.push_back(hProc);
    }
    if (handles.empty()) return;

    // WaitForMultipleObjects 는 최대 64. 보통 좀비 5 개 이하라 거의 통과.
    DWORD count = (DWORD)handles.size();
    if (count > MAXIMUM_WAIT_OBJECTS) count = MAXIMUM_WAIT_OBJECTS;
    WaitForMultipleObjects(count, handles.data(), TRUE, 5000);

    // 3단계: 5초 안에 안 죽은 프로세스만 TerminateProcess fallback.
    for (HANDLE h : handles) {
        if (WaitForSingleObject(h, 0) == WAIT_TIMEOUT) {
            TerminateProcess(h, 1);
            WaitForSingleObject(h, 1000);
        }
        CloseHandle(h);
    }
}

}  // namespace orange
