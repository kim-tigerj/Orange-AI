#include "MainWindow.h"

#include "Capture.h"
#include "Utils.h"

#include <process.h>

namespace orange {

namespace {

UINT ShutdownForUpdateMessage() {
    static UINT msg = RegisterWindowMessageW(L"OrangeCode.ShutdownForUpdate.v1");
    return msg;
}

std::wstring CurrentTimestampForFile() {
    SYSTEMTIME st{};
    GetLocalTime(&st);
    wchar_t buf[32] = L"";
    swprintf_s(buf, L"%04u%02u%02u-%02u%02u%02u",
               st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    return buf;
}

std::wstring CurrentExeBuildLabel() {
    wchar_t path[MAX_PATH] = L"";
    DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return L"unknown";

    WIN32_FILE_ATTRIBUTE_DATA attr{};
    if (!GetFileAttributesExW(path, GetFileExInfoStandard, &attr)) return L"unknown";

    FILETIME localFt{};
    SYSTEMTIME st{};
    if (!FileTimeToLocalFileTime(&attr.ftLastWriteTime, &localFt) ||
        !FileTimeToSystemTime(&localFt, &st)) {
        return L"unknown";
    }

    wchar_t buf[64] = L"";
    swprintf_s(buf, L"%04u-%02u-%02u %02u:%02u",
               st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute);
    return buf;
}

std::wstring ShortPathLabel(const std::wstring& path) {
    if (path.empty()) return L"none";
    if (path.size() <= 54) return path;
    return L"..." + path.substr(path.size() - 51);
}

std::wstring FolderOf(const std::wstring& path) {
    size_t pos = path.find_last_of(L"\\/");
    if (pos == std::wstring::npos) return L".";
    return path.substr(0, pos);
}

std::wstring FileUriForFolder(const std::wstring& path) {
    std::wstring folder = FolderOf(path);
    std::wstring uri = L"file:///";
    for (wchar_t ch : folder) {
        if (ch == L'\\') uri += L'/';
        else if (ch == L' ') uri += L"%20";
        else uri += ch;
    }
    return uri;
}

std::wstring FileUriForPath(const std::wstring& path) {
    std::wstring uri = L"file:///";
    for (wchar_t ch : path) {
        if (ch == L'\\') uri += L'/';
        else if (ch == L' ') uri += L"%20";
        else uri += ch;
    }
    return uri;
}

std::wstring PathWithFileAndFolderOpenIcons(const std::wstring& path) {
    if (path.empty()) return L"none";
    return ShortPathLabel(path) + L"\n  [파일 열기](" + FileUriForPath(path) +
           L")  [폴더 열기](" + FileUriForFolder(path) + L")";
}

std::wstring DefaultCapturePath() {
    std::wstring root = FindProjectRoot();
    if (root.empty()) root = L".";
    return root + L"\\tools\\capture-" + CurrentTimestampForFile() + L".png";
}

std::wstring BuildLogPath(const std::wstring& root) {
    if (root.empty()) return {};
    return root + L"\\build_log.txt";
}

bool WriteBuildLog(const std::wstring& root,
                   int exitCode,
                   DWORD elapsedMs,
                   const std::wstring& command,
                   const std::wstring& output) {
    std::wstring path = BuildLogPath(root);
    if (path.empty()) return false;

    std::wstring text;
    text += L"OrangeCode build log\n";
    text += L"timestamp: " + CurrentTimestampForFile() + L"\n";
    text += L"command: " + command + L"\n";
    text += L"exit_code: " + std::to_wstring(exitCode) + L"\n";
    text += L"elapsed_ms: " + std::to_wstring(elapsedMs) + L"\n\n";
    text += output;
    if (text.empty() || text.back() != L'\n') text += L"\n";

    std::string bytes = WideToUtf8(text);
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;

    DWORD written = 0;
    BOOL ok = WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr);
    CloseHandle(file);
    return ok && written == bytes.size();
}

ULONGLONG FileSizeOrZero(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA attr{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &attr)) return 0;
    ULARGE_INTEGER size{};
    size.HighPart = attr.nFileSizeHigh;
    size.LowPart = attr.nFileSizeLow;
    return size.QuadPart;
}

void CloseHandleIfValid(HANDLE& h) {
    if (h) {
        CloseHandle(h);
        h = nullptr;
    }
}

std::wstring BytesToWideBestEffort(const std::string& bytes) {
    if (bytes.empty()) return {};
    int wlen = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                   bytes.data(), (int)bytes.size(), nullptr, 0);
    UINT cp = CP_UTF8;
    DWORD flags = MB_ERR_INVALID_CHARS;
    if (wlen <= 0) {
        cp = CP_ACP;
        flags = 0;
        wlen = MultiByteToWideChar(cp, flags, bytes.data(), (int)bytes.size(), nullptr, 0);
    }
    if (wlen <= 0) return L"[output decode failed]";
    std::wstring out(wlen, L'\0');
    MultiByteToWideChar(cp, flags, bytes.data(), (int)bytes.size(), out.data(), wlen);
    return out;
}

std::wstring TailLines(std::wstring text, size_t maxChars) {
    if (text.size() <= maxChars) return text;
    return L"...\n" + text.substr(text.size() - maxChars);
}

bool FileExists(const std::wstring& path) {
    DWORD attr = GetFileAttributesW(path.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

std::wstring UniqueBuildBackupPath(const std::wstring& path) {
    std::wstring stamp = CurrentTimestampForFile() + L"-pid" + std::to_wstring(GetCurrentProcessId());
    for (int i = 0; i < 100; ++i) {
        std::wstring candidate = path + L"." + stamp;
        if (i > 0) candidate += L"-" + std::to_wstring(i);
        candidate += L".old";
        if (!FileExists(candidate)) return candidate;
    }
    return {};
}

bool MoveReleaseOutputAside(const std::wstring& path, std::wstring& log) {
    if (!FileExists(path)) return true;

    std::wstring backup = UniqueBuildBackupPath(path);
    if (backup.empty()) {
        log += L"[build prep] No unique backup name for " + path + L"\n";
        return false;
    }

    if (MoveFileW(path.c_str(), backup.c_str())) {
        log += L"[build prep] moved " + path + L" -> " + backup + L"\n";
        return true;
    }

    DWORD err = GetLastError();
    log += L"[build prep] failed to move " + path + L" (GetLastError=" + std::to_wstring(err) + L")\n";
    return false;
}

bool PrepareReleaseOutputForBuild(const std::wstring& root, std::wstring& log) {
    const std::wstring releaseDir = root + L"\\bin\\Release\\";
    bool ok = true;
    ok = MoveReleaseOutputAside(releaseDir + L"OrangeCode.exe", log) && ok;
    ok = MoveReleaseOutputAside(releaseDir + L"OrangeCode.pdb", log) && ok;
    ok = MoveReleaseOutputAside(releaseDir + L"Code.exe", log) && ok;
    ok = MoveReleaseOutputAside(releaseDir + L"Code.pdb", log) && ok;
    return ok;
}

unsigned __stdcall RunBuildThreadProc(void* raw) {
    CMainWindow* self = static_cast<CMainWindow*>(raw);
    if (!self) return 0;

    DWORD started = GetTickCount();
    auto* result = new BuildResultPacket();
    std::wstring root = FindProjectRoot();
    const std::wstring command = L"MSBuild.exe Code.sln -p:Configuration=Release -p:Platform=x64 -m -v:minimal";
    if (root.empty()) {
        result->exitCode = -1;
        result->output = L"Project root was not found.";
        PostMessageW(self->Hwnd(), WM_APP + 3, 0, reinterpret_cast<LPARAM>(result));
        return 0;
    }

    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE stdoutRead = nullptr;
    HANDLE stdoutWrite = nullptr;
    if (!CreatePipe(&stdoutRead, &stdoutWrite, &sa, 0)) {
        result->exitCode = -1;
        result->output = L"CreatePipe failed.";
        WriteBuildLog(root, result->exitCode, GetTickCount() - started, command, result->output);
        PostMessageW(self->Hwnd(), WM_APP + 3, 0, reinterpret_cast<LPARAM>(result));
        return 0;
    }
    SetHandleInformation(stdoutRead, HANDLE_FLAG_INHERIT, 0);

    std::wstring msbuildPath = FindMSBuildExePath();
    if (msbuildPath.empty()) {
        result->exitCode = -1;
        result->output = L"MSBuild.exe path not found.";
        WriteBuildLog(root, result->exitCode, GetTickCount() - started, command, result->output);
        PostMessageW(self->Hwnd(), WM_APP + 3, 0, reinterpret_cast<LPARAM>(result));
        return 0;
    }

    std::wstring prepLog;
    if (!PrepareReleaseOutputForBuild(root, prepLog)) {
        result->exitCode = -1;
        result->output = prepLog + L"\nBuild stopped because existing Release output could not be moved aside.";
        WriteBuildLog(root, result->exitCode, GetTickCount() - started, command, result->output);
        PostMessageW(self->Hwnd(), WM_APP + 3, 0, reinterpret_cast<LPARAM>(result));
        return 0;
    }

    // Preserve the relaunch environment variable
    std::wstring cmd = L"cmd.exe /c \"set ORANGE_CODE_RELAUNCH_AFTER_BUILD=1&& \"" + msbuildPath + L"\" Code.sln -p:Configuration=Release -p:Platform=x64 -m -v:minimal\"";
    std::vector<wchar_t> mutableCmd(cmd.begin(), cmd.end());
    mutableCmd.push_back(L'\0');

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = stdoutWrite;
    si.hStdError = stdoutWrite;
    PROCESS_INFORMATION pi{};

    BOOL ok = CreateProcessW(nullptr, mutableCmd.data(), nullptr, nullptr, TRUE,
                             CREATE_NO_WINDOW, nullptr, root.c_str(), &si, &pi);
    CloseHandleIfValid(stdoutWrite);
    if (!ok) {
        CloseHandleIfValid(stdoutRead);
        result->exitCode = -1;
        result->output = L"CreateProcess failed for MSBuild.exe Code.sln.";
        WriteBuildLog(root, result->exitCode, GetTickCount() - started, command, result->output);
        PostMessageW(self->Hwnd(), WM_APP + 3, 0, reinterpret_cast<LPARAM>(result));
        return 0;
    }

    CloseHandle(pi.hThread);
    std::string bytes;
    char buf[4096];
    DWORD readBytes = 0;
    while (ReadFile(stdoutRead, buf, sizeof(buf), &readBytes, nullptr) && readBytes > 0) {
        bytes.append(buf, buf + readBytes);
    }
    CloseHandleIfValid(stdoutRead);

    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD exitCode = 0;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hProcess);

    result->exitCode = static_cast<int>(exitCode);
    result->elapsedMs = GetTickCount() - started;
    result->output = prepLog + BytesToWideBestEffort(bytes);
    WriteBuildLog(root, result->exitCode, result->elapsedMs, command, result->output);
    PostMessageW(self->Hwnd(), WM_APP + 3, 0, reinterpret_cast<LPARAM>(result));
    return 0;
}

} // namespace

void CMainWindow::StartBuildCommand() {
    if (!m_view) return;
    if (m_buildRunning) {
        m_view->NewBlock(L"task");
        m_view->AppendText(L"빌드\n\n- 상태: 이미 실행 중");
        m_view->ScrollLatestIntoView();
        return;
    }

    m_buildRunning = true;
    m_view->NewBlock(L"task");
    m_view->AppendText(L"빌드\n\n- 명령: MSBuild.exe Code.sln\n- 상태: 실행 중\n- 원칙: LLM 호출 없이 로컬 검증");
    m_view->ScrollLatestIntoView();

    uintptr_t h = _beginthreadex(nullptr, 0, RunBuildThreadProc, this, 0, nullptr);
    if (h) {
        CloseHandle(reinterpret_cast<HANDLE>(h));
    } else {
        m_buildRunning = false;
        m_view->NewBlock(L"error");
        m_view->AppendText(L"[Build thread start failed]");
    }
}

void CMainWindow::AppendBuildResultCard(int exitCode, DWORD elapsedMs, const std::wstring& output) {
    m_buildRunning = false;
    if (!m_view) return;

    std::wstring card;
    card += L"빌드 결과\n\n";
    card += L"- 명령: MSBuild.exe Code.sln\n";
    card += L"- 종료 코드: " + std::to_wstring(exitCode) + L"\n";
    card += L"- 시간: " + std::to_wstring(elapsedMs / 1000) + L"." + std::to_wstring((elapsedMs % 1000) / 100) + L"s\n";
    card += L"- 실행 파일: " + CurrentExeBuildLabel() + L"\n";
    std::wstring root = FindProjectRoot();
    if (!root.empty()) {
        card += L"- 로그: " + PathWithFileAndFolderOpenIcons(BuildLogPath(root)) + L"\n";
    }
    card += L"\n";
    card += L"```text\n";
    card += TailLines(output, 1600);
    card += L"\n```";

    m_view->NewBlock(exitCode == 0 ? L"task" : L"error");
    m_view->AppendText(card);
    m_view->ScrollLatestIntoView();

    const bool automatedTest =
        !m_startupOptions.testPrompt.empty() ||
        !m_startupOptions.testCapturePath.empty() ||
        m_startupOptions.testPasteClipboardImage ||
        !m_startupOptions.testAttachmentPath.empty() ||
        m_startupOptions.useMockBackend;
    if (exitCode == 0 && !automatedTest && !m_captureAfterBuild) {
        if (m_forceRelaunchAfterBuild) {
            m_forceRelaunchAfterBuild = false;
            m_relaunchPending = false;
            RelaunchAfterSuccessfulBuild();
        } else if (m_pendingBackendCount > 0) {
            m_relaunchPending = true;
            m_view->NewBlock(L"task");
            m_view->AppendText(L"앱 갱신 대기\n\n- 빌드 성공\n- 상태: 팀원 작업 완료 후 재시작 예정\n- 이유: 코드 수정 중인 팀원이 완료되기 전에 재시작하지 않음");
            m_view->ScrollLatestIntoView();
        } else {
            RelaunchAfterSuccessfulBuild();
        }
        return;
    }

    if (m_captureAfterBuild) {
        m_captureAfterBuild = false;
        if (exitCode == 0) {
            RunCaptureCommand();
        } else {
            m_view->NewBlock(L"error");
            m_view->AppendText(L"검증 중단\n\n- 이유: 빌드 실패\n- 다음: 빌드 오류를 수정한 뒤 /verify 재실행");
            m_view->ScrollLatestIntoView();
        }
    }
}

void CMainWindow::RelaunchAfterSuccessfulBuild() {
    wchar_t exePath[MAX_PATH] = L"";
    DWORD n = GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return;

    UINT msg = ShutdownForUpdateMessage();
    if (msg) PostMessageW(HWND_BROADCAST, msg, 0, 0);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    std::wstring cmd = L"\"" + std::wstring(exePath) + L"\"";
    std::vector<wchar_t> mutableCmd(cmd.begin(), cmd.end());
    mutableCmd.push_back(L'\0');

    BOOL ok = CreateProcessW(nullptr, mutableCmd.data(), nullptr, nullptr, FALSE,
                             0, nullptr, nullptr, &si, &pi);
    if (ok) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    } else if (m_view) {
        m_view->NewBlock(L"error");
        m_view->AppendText(L"앱 갱신 실패\n\n- 빌드는 성공했지만 새 정팀장 실행에 실패했습니다.");
    }
}

void CMainWindow::RunCaptureCommand() {
    if (!m_view) return;

    std::wstring outPath = DefaultCapturePath();
    DWORD started = GetTickCount();
    HRESULT hr = CaptureWindowToPng(m_hwnd, outPath);
    DWORD elapsed = GetTickCount() - started;
    ULONGLONG size = SUCCEEDED(hr) ? FileSizeOrZero(outPath) : 0;
    AppendCaptureResultCard(hr, outPath, size, elapsed);
    if (SUCCEEDED(hr)) {
        AddAttachmentPaths({ outPath });
    }
}

void CMainWindow::AppendCaptureResultCard(HRESULT hr, const std::wstring& path, ULONGLONG fileSize, DWORD elapsedMs) {
    if (!m_view) return;

    std::wstring card;
    card += L"캡처 결과\n\n";
    card += L"- 상태: ";
    card += SUCCEEDED(hr) ? L"성공\n" : L"실패\n";
    card += L"- HRESULT: 0x";
    wchar_t hex[16] = L"";
    swprintf_s(hex, L"%08X", (unsigned)hr);
    card += hex;
    card += L"\n";
    card += L"- 경로: " + PathWithFileAndFolderOpenIcons(path) + L"\n";
    card += L"- 크기: " + std::to_wstring(fileSize / 1024) + L" KB\n";
    card += L"- 시간: " + std::to_wstring(elapsedMs) + L" ms";

    m_view->NewBlock(SUCCEEDED(hr) ? L"task" : L"error");
    m_view->AppendText(card);
    m_view->ScrollLatestIntoView();
}

void CMainWindow::RunTestCapture() {
    if (m_startupOptions.testCapturePath.empty()) return;
    HRESULT hr = CaptureWindowToPng(m_hwnd, m_startupOptions.testCapturePath);
    if (FAILED(hr) && m_view) {
        m_view->NewBlock(L"error");
        m_view->AppendText(L"[Automatic test capture failed]\r\n");
    }
}

} // namespace orange
