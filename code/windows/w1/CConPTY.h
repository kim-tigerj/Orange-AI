#pragma once

// CConPTY - Windows ConPTY (Pseudo Console) wrapper.
// Ported from Orange agent (LiveCMD: OR-977) with yagent-specific
// dependencies removed. Requires Win10 1809+ (build 17763).
// IsSupported() returns false on older Windows.

#include <windows.h>
#include <strsafe.h>
#include <process.h>
#include <functional>

// Forward declarations for SDKs older than 10.0.17763
#ifndef PSEUDOCONSOLE_INHERIT_CURSOR
typedef VOID*  HPCON;
#define PSEUDOCONSOLE_INHERIT_CURSOR  0x1
#endif

#ifndef PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE
#define PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE \
    ProcThreadAttributeValue(22, FALSE, TRUE, FALSE)
#endif

typedef HRESULT(WINAPI* PFN_CreatePseudoConsole)(COORD, HANDLE, HANDLE, DWORD, HPCON*);
typedef HRESULT(WINAPI* PFN_ResizePseudoConsole)(HPCON, COORD);
typedef void   (WINAPI* PFN_ClosePseudoConsole)(HPCON);

// Output callback: return false to stop reading.
typedef std::function<bool(const char*, DWORD)>                          ConPTYOutputCallback;
// Error callback (optional).
typedef std::function<void(void*, bool, const char*, DWORD, const char*, DWORD)> ConPTYErrorCallback;

class CConPTY {
public:
    CConPTY()
        : m_hPC(nullptr)
        , m_hPipeIn(nullptr)
        , m_hPipeOut(nullptr)
        , m_hProcess(nullptr)
        , m_pAttrList(nullptr)
        , m_hReadThread(nullptr)
        , m_hShutdown(nullptr)
        , m_hFirstOutput(nullptr)
    {
        ZeroMemory(&m_error, sizeof(m_error));
    }
    ~CConPTY() {}

    // Runtime check for ConPTY support.
    static bool IsSupported() {
        LoadAPI();
        return s_pfnCreate() != nullptr;
    }

    // Create PTY and spawn pApp with pArg in pCurPath.
    bool Create(PCWSTR pApp, PCWSTR pArg, PCWSTR pCurPath,
                COORD size, ConPTYOutputCallback pCallback)
    {
        if (!IsSupported()) return false;

        bool   bRet         = false;
        HANDLE hPipePTYIn   = nullptr;
        HANDLE hPipePTYOut  = nullptr;
        DWORD  dwError      = 0;
        SetLastError(0);

        do {
            m_hShutdown = ::CreateEvent(nullptr, TRUE, FALSE, nullptr);
            if (!m_hShutdown) {
                ErrorLog(false, "CreateEvent(Shutdown)", dwError = GetLastError()); break;
            }

            m_hFirstOutput = ::CreateEvent(nullptr, TRUE, FALSE, nullptr);
            if (!m_hFirstOutput) {
                ErrorLog(false, "CreateEvent(FirstOutput)", dwError = GetLastError()); break;
            }

            if (!::CreatePipe(&hPipePTYIn, &m_hPipeIn, nullptr, 0)) {
                ErrorLog(false, "CreatePipe(input)", dwError = GetLastError()); break;
            }
            if (!::CreatePipe(&m_hPipeOut, &hPipePTYOut, nullptr, 0)) {
                ErrorLog(false, "CreatePipe(output)", dwError = GetLastError()); break;
            }

            HRESULT hr = s_pfnCreate()(size, hPipePTYIn, hPipePTYOut, 0, &m_hPC);
            if (FAILED(hr)) {
                SetLastError(HRESULT_CODE(hr));
                ErrorLog(false, "CreatePseudoConsole", dwError = HRESULT_CODE(hr)); break;
            }
            ErrorLog(true, "CreatePseudoConsole", 0);

            ::CloseHandle(hPipePTYIn);  hPipePTYIn  = nullptr;
            ::CloseHandle(hPipePTYOut); hPipePTYOut = nullptr;

            SIZE_T attrSize = 0;
            InitializeProcThreadAttributeList(nullptr, 1, 0, &attrSize);
            m_pAttrList = (LPPROC_THREAD_ATTRIBUTE_LIST)
                          HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, attrSize);
            if (!m_pAttrList) {
                ErrorLog(false, "HeapAlloc(AttrList)", dwError = ERROR_OUTOFMEMORY); break;
            }
            if (!InitializeProcThreadAttributeList(m_pAttrList, 1, 0, &attrSize)) {
                ErrorLog(false, "InitializeProcThreadAttributeList", dwError = GetLastError()); break;
            }

            if (!UpdateProcThreadAttribute(m_pAttrList, 0,
                    PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE,
                    m_hPC, sizeof(HPCON), nullptr, nullptr))
            {
                ErrorLog(false, "UpdateProcThreadAttribute", dwError = GetLastError()); break;
            }

            STARTUPINFOEXW       si{};
            PROCESS_INFORMATION  pi{};
            si.StartupInfo.cb    = sizeof(si);
            si.lpAttributeList   = m_pAttrList;

            WCHAR szCommand[1024] = L"";
            if (pArg)
                StringCbPrintfW(szCommand, sizeof(szCommand), L"\"%s\" %s", pApp, pArg);
            else
                StringCbPrintfW(szCommand, sizeof(szCommand), L"\"%s\"", pApp);

            if (!::CreateProcessW(nullptr, szCommand, nullptr, nullptr, FALSE,
                                  EXTENDED_STARTUPINFO_PRESENT, nullptr, pCurPath,
                                  &si.StartupInfo, &pi))
            {
                ErrorLog(false, "CreateProcess", dwError = GetLastError()); break;
            }
            ErrorLog(true, "CreateProcess", 0);

            m_hProcess = pi.hProcess;
            ::CloseHandle(pi.hThread);

            m_outputCallback = pCallback;
            m_hReadThread    = (HANDLE)_beginthreadex(nullptr, 0, ReadThread, this, 0, nullptr);
            if (!m_hReadThread) {
                ErrorLog(false, "_beginthreadex", dwError = GetLastError()); break;
            }

            bRet = true;
        } while (false);

        if (hPipePTYIn)  ::CloseHandle(hPipePTYIn);
        if (hPipePTYOut) ::CloseHandle(hPipePTYOut);
        if (!bRet)       Destroy();
        SetLastError(dwError);
        return bRet;
    }

    // Write raw bytes to PTY stdin.
    bool Write(const void* pData, DWORD dwSize) {
        DWORD dwWritten = 0;
        if (m_hPipeIn &&
            ::WriteFile(m_hPipeIn, pData, dwSize, &dwWritten, nullptr) &&
            dwWritten == dwSize)
        {
            return true;
        }
        if (m_error.pCallback)
            m_error.pCallback(m_error.pContext, false, __FILE__, __LINE__,
                              "WriteFile(PipeIn)", GetLastError());
        return false;
    }

    // Resize the terminal.
    bool Resize(SHORT cols, SHORT rows) {
        if (!m_hPC || !s_pfnResize()) return false;
        COORD size = { cols, rows };
        HRESULT hr = s_pfnResize()(m_hPC, size);
        if (FAILED(hr)) {
            ErrorLog(false, "ResizePseudoConsole", HRESULT_CODE(hr));
            return false;
        }
        return true;
    }

    // Terminate process and clean up all handles.
    void Destroy() {
        if (m_hShutdown) ::SetEvent(m_hShutdown);

        if (m_hPC && s_pfnClose()) {
            s_pfnClose()(m_hPC);
            m_hPC = nullptr;
        }

        if (m_hReadThread) {
            ::WaitForSingleObject(m_hReadThread, 3000);
            ::CloseHandle(m_hReadThread);
            m_hReadThread = nullptr;
        }

        if (m_hPipeIn) {
            ::CloseHandle(m_hPipeIn);
            m_hPipeIn = nullptr;
        }

        if (m_hProcess) {
            DWORD dwWait = ::WaitForSingleObject(m_hProcess, 3000);
            if (WAIT_TIMEOUT == dwWait) {
                ::TerminateProcess(m_hProcess, 0);
                ErrorLog(false, "TerminateProcess(forced)", ::GetProcessId(m_hProcess));
            }
            ::CloseHandle(m_hProcess);
            m_hProcess = nullptr;
        }

        if (m_hPipeOut) {
            ::CloseHandle(m_hPipeOut);
            m_hPipeOut = nullptr;
        }

        if (m_pAttrList) {
            DeleteProcThreadAttributeList(m_pAttrList);
            HeapFree(GetProcessHeap(), 0, m_pAttrList);
            m_pAttrList = nullptr;
        }

        if (m_hShutdown) {
            ::CloseHandle(m_hShutdown);
            m_hShutdown = nullptr;
        }

        if (m_hFirstOutput) {
            ::CloseHandle(m_hFirstOutput);
            m_hFirstOutput = nullptr;
        }

        m_outputCallback = nullptr;
    }

    HANDLE ProcessHandle() const { return m_hProcess; }
    DWORD  ProcessId()     const { return m_hProcess ? ::GetProcessId(m_hProcess) : 0; }

    // Block until the first output arrives (e.g. shell banner).
    bool WaitForFirstOutput(DWORD dwTimeoutMs = 3000) {
        if (!m_hFirstOutput) return false;
        return ::WaitForSingleObject(m_hFirstOutput, dwTimeoutMs) == WAIT_OBJECT_0;
    }

    void SetErrorCallback(void* pContext, ConPTYErrorCallback pCallback) {
        m_error.pContext  = pContext;
        m_error.pCallback = pCallback;
    }

private:
    HPCON   m_hPC;
    HANDLE  m_hPipeIn;
    HANDLE  m_hPipeOut;
    HANDLE  m_hProcess;
    LPPROC_THREAD_ATTRIBUTE_LIST m_pAttrList;
    HANDLE  m_hReadThread;
    HANDLE  m_hShutdown;
    HANDLE  m_hFirstOutput;
    ConPTYOutputCallback m_outputCallback;

    struct {
        void*               pContext;
        ConPTYErrorCallback pCallback;
    } m_error;

    void ErrorLog(bool bSuccess, const char* pCause, DWORD dwError) {
        if (m_error.pCallback)
            m_error.pCallback(m_error.pContext, bSuccess, __FILE__, __LINE__, pCause, dwError);
    }

    // Runtime API loading via static locals (header-only, no ODR violation).
    static PFN_CreatePseudoConsole& s_pfnCreate() { static PFN_CreatePseudoConsole s = nullptr; return s; }
    static PFN_ResizePseudoConsole& s_pfnResize() { static PFN_ResizePseudoConsole s = nullptr; return s; }
    static PFN_ClosePseudoConsole&  s_pfnClose()  { static PFN_ClosePseudoConsole  s = nullptr; return s; }
    static bool& s_bLoaded() { static bool s = false; return s; }

    static bool LoadAPI() {
        if (s_bLoaded()) return s_pfnCreate() != nullptr;
        s_bLoaded() = true;

        HMODULE hKernel = ::GetModuleHandleW(L"kernel32.dll");
        if (!hKernel) return false;

        s_pfnCreate() = (PFN_CreatePseudoConsole)::GetProcAddress(hKernel, "CreatePseudoConsole");
        s_pfnResize() = (PFN_ResizePseudoConsole)::GetProcAddress(hKernel, "ResizePseudoConsole");
        s_pfnClose()  = (PFN_ClosePseudoConsole) ::GetProcAddress(hKernel, "ClosePseudoConsole");

        return s_pfnCreate() != nullptr;
    }

    // PTY output reading thread.
    static unsigned __stdcall ReadThread(void* p) {
        CConPTY* pThis = (CConPTY*)p;
        char     szBuf[32768];
        DWORD    dwRead = 0;

        while (::WaitForSingleObject(pThis->m_hShutdown, 0) != WAIT_OBJECT_0) {
            if (::ReadFile(pThis->m_hPipeOut, szBuf, sizeof(szBuf), &dwRead, nullptr)) {
                if (dwRead > 0) {
                    if (pThis->m_hFirstOutput) {
                        ::SetEvent(pThis->m_hFirstOutput);
                    }
                    if (pThis->m_outputCallback) {
                        if (!pThis->m_outputCallback(szBuf, dwRead)) break;
                    }
                }
            } else {
                DWORD dwErr = GetLastError();
                if (dwErr != ERROR_BROKEN_PIPE) {
                    pThis->ErrorLog(false, "ReadFile(PipeOut)", dwErr);
                }
                break;
            }
        }

        if (pThis->m_outputCallback) {
            pThis->m_outputCallback(nullptr, 0);
        }
        return 0;
    }
};
