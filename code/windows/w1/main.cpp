#include "MainWindow.h"
#include "Capture.h"

#include "ManagerEnvironment.h"
#include "AttachmentStore.h"
#include "AppSettings.h"
#include "Persistence.h"

#include <shellapi.h>
#include <fstream>
#include <string>
#include <json/json.h>

#pragma comment(lib, "shell32.lib")

namespace {

constexpr wchar_t kWindowClass[] = L"OrangeCodeMainWindow";

std::wstring GetArgValue(int argc, wchar_t** argv, const wchar_t* name) {
    std::wstring prefix = std::wstring(name) + L"=";
    for (int i = 1; i + 1 < argc; ++i) {
        if (wcscmp(argv[i], name) == 0) return argv[i + 1];
    }
    for (int i = 1; i < argc; ++i) {
        std::wstring arg = argv[i] ? argv[i] : L"";
        if (arg.rfind(prefix, 0) == 0) return arg.substr(prefix.size());
    }
    return {};
}

bool HasArg(int argc, wchar_t** argv, const wchar_t* name) {
    std::wstring prefix = std::wstring(name) + L"=";
    for (int i = 1; i < argc; ++i) {
        if (wcscmp(argv[i], name) == 0) return true;
        std::wstring arg = argv[i] ? argv[i] : L"";
        if (arg.rfind(prefix, 0) == 0) return true;
    }
    return false;
}

unsigned GetUIntArgValue(int argc, wchar_t** argv, const wchar_t* name, unsigned fallback) {
    std::wstring v = GetArgValue(argc, argv, name);
    if (v.empty()) return fallback;
    wchar_t* end = nullptr;
    unsigned long parsed = wcstoul(v.c_str(), &end, 10);
    if (!end || *end != L'\0') return fallback;
    return static_cast<unsigned>(parsed);
}

std::wstring ReadUtf8TextFile(const std::wstring& path) {
    if (path.empty()) return {};
    std::ifstream ifs(orange::WideToUtf8(path), std::ios::binary);
    if (!ifs.is_open()) return {};
    std::string data((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    if (data.size() >= 3 &&
        (unsigned char)data[0] == 0xEF &&
        (unsigned char)data[1] == 0xBB &&
        (unsigned char)data[2] == 0xBF) {
        data.erase(0, 3);
    }
    return orange::Utf8ToWide(data.c_str(), (int)data.size());
}

int RunCaptureCommand(const std::wstring& outPath) {
    if (outPath.empty()) return 2;

    DWORD targetPid = GetCurrentProcessId();
    wchar_t pidEnv[32] = L"";
    if (GetEnvironmentVariableW(L"ORANGE_CODE_PID", pidEnv, 32) > 0) {
        wchar_t* end = nullptr;
        unsigned long parsed = wcstoul(pidEnv, &end, 10);
        if (parsed > 0 && end && *end == L'\0') {
            targetPid = static_cast<DWORD>(parsed);
        }
    }

    HWND target = orange::FindMainWindowOfPid(targetPid, kWindowClass);
    if (!target) return 3;

    HRESULT hr = orange::CaptureWindowToPng(target, outPath);
    return SUCCEEDED(hr) ? 0 : 4;
}

int RunManagerEnvDiagnostic(const std::wstring& outPath) {
    if (outPath.empty()) return 2;

    orange::ManagerEnvironment env = orange::EnsureManagerEnvironment();
    std::ofstream ofs(orange::WideToUtf8(outPath), std::ios::binary);
    if (!ofs.is_open()) return 4;

    ofs << "ORANGE_CODE_ROOT=" << orange::WideToUtf8(env.root) << "\n";
    ofs << "ORANGE_CODE_ROLE=" << orange::WideToUtf8(env.role) << "\n";
    ofs << "ORANGE_CODE_PID=" << orange::WideToUtf8(env.pid) << "\n";
    ofs << "ORANGE_CODE_SESSION_KEY=" << orange::WideToUtf8(env.sessionKey) << "\n";
    return 0;
}

int RunAttachmentDiagnostic(const std::wstring& outPath) {
    if (outPath.empty()) return 2;

    orange::EnsureManagerEnvironment();
    std::wstring sessionKey = L"default";
    wchar_t envSession[256] = L"";
    if (GetEnvironmentVariableW(L"ORANGE_CODE_SESSION_KEY", envSession, 256) > 0 && envSession[0] != L'\0') {
        sessionKey = envSession;
    }

    orange::AttachmentRecord sample;
    sample.id = L"att_diag";
    sample.kind = L"file";
    sample.name = L"diagnostic.txt";
    sample.originalPath = outPath;
    sample.storedPath = outPath;
    sample.mime = L"text/plain";
    sample.sizeBytes = 0;

    if (!orange::AttachmentStore::WriteManifest(sessionKey, { sample })) return 4;

    std::wstring manifest = orange::AttachmentStore::ManifestPath(sessionKey);
    std::ofstream ofs(orange::WideToUtf8(outPath), std::ios::binary);
    if (!ofs.is_open()) return 5;
    ofs << "ATTACHMENT_MANIFEST=" << orange::WideToUtf8(manifest) << "\n";
    return 0;
}

int RunAddAttachmentDiagnostic(const std::wstring& sourcePath, const std::wstring& outPath) {
    if (sourcePath.empty() || outPath.empty()) return 2;

    orange::EnsureManagerEnvironment();
    std::wstring sessionKey = L"default";
    wchar_t envSession[256] = L"";
    if (GetEnvironmentVariableW(L"ORANGE_CODE_SESSION_KEY", envSession, 256) > 0 && envSession[0] != L'\0') {
        sessionKey = envSession;
    }

    orange::AttachmentRecord rec;
    if (!orange::AttachmentStore::AddFile(sessionKey, sourcePath, &rec)) return 4;
    orange::Persistence::SaveAttachmentRecord(orange::WideToUtf8(sessionKey), rec);

    std::ofstream ofs(orange::WideToUtf8(outPath), std::ios::binary);
    if (!ofs.is_open()) return 5;
    ofs << "ATTACHMENT_MANIFEST=" << orange::WideToUtf8(orange::AttachmentStore::ManifestPath(sessionKey)) << "\n";
    ofs << "ID=" << orange::WideToUtf8(rec.id) << "\n";
    ofs << "KIND=" << orange::WideToUtf8(rec.kind) << "\n";
    ofs << "STORED_PATH=" << orange::WideToUtf8(rec.storedPath) << "\n";
    ofs << "THUMBNAIL_PATH=" << orange::WideToUtf8(rec.thumbnailPath) << "\n";
    ofs << "SIZE_BYTES=" << rec.sizeBytes << "\n";
    return 0;
}

int RunManagerPromptDiagnostic(const std::wstring& outPath) {
    if (outPath.empty()) return 2;
    orange::ManagerEnvironment env = orange::EnsureManagerEnvironment();
    std::wstring prompt = orange::BuildManagerPrompt(L"diagnose prompt", env);
    std::ofstream ofs(orange::WideToUtf8(outPath), std::ios::binary);
    if (!ofs.is_open()) return 4;
    ofs << orange::WideToUtf8(prompt);
    return 0;
}

int RunShutdownRunningAppsCommand() {
    UINT msg = RegisterWindowMessageW(L"OrangeCode.ShutdownForUpdate.v1");
    if (!msg) return 1;
    PostMessageW(HWND_BROADCAST, msg, 0, 0);
    Sleep(250);
    return 0;
}

int RunChatApiCommand(const std::wstring& action,
                      const std::wstring& chatKey,
                      const std::wstring& outPath,
                      unsigned limit,
                      const std::wstring& inputPath = std::wstring(),
                      const std::wstring& title = std::wstring(),
                      bool overwrite = false) {
    if (action.empty() || outPath.empty()) return 2;
    orange::EnsureManagerEnvironment();

    Json::Value result;
    std::string key = orange::WideToUtf8(chatKey);
    if (action == L"list") {
        result = orange::Persistence::ChatApiList((int)limit);
    } else if (action == L"show") {
        result = orange::Persistence::ChatApiShow(key, (int)limit);
    } else if (action == L"attachments") {
        result = orange::Persistence::ChatApiAttachments(key);
    } else if (action == L"rename") {
        result = orange::Persistence::ChatApiRename(key, orange::WideToUtf8(title));
        if (!result.isMember("error")) {
            UINT msg = RegisterWindowMessageW(L"OrangeCode.ChatSessionsChanged.v1");
            if (msg) PostMessageW(HWND_BROADCAST, msg, 0, 0);
        }
    } else if (action == L"import") {
        std::wstring input = ReadUtf8TextFile(inputPath);
        result = orange::Persistence::ChatApiImport(
            key,
            orange::WideToUtf8(title),
            input,
            overwrite);
        result["input_path"] = orange::WideToUtf8(inputPath);
        if (!result.isMember("error")) {
            UINT msg = RegisterWindowMessageW(L"OrangeCode.ChatSessionsChanged.v1");
            if (msg) PostMessageW(HWND_BROADCAST, msg, 0, 0);
        }
    } else {
        result["api"] = "orange.chat.v1";
        result["error"] = "unknown chat api action";
        result["action"] = orange::WideToUtf8(action);
    }

    Json::StreamWriterBuilder builder;
    builder["indentation"] = "  ";
    std::string data = Json::writeString(builder, result);
    std::ofstream ofs(orange::WideToUtf8(outPath), std::ios::binary);
    if (!ofs.is_open()) return 4;
    ofs.write(data.data(), (std::streamsize)data.size());
    return ofs.good() ? 0 : 5;
}

int RunPromptApiCommand(const std::wstring& action,
                        const std::wstring& chatKey,
                        const std::wstring& outPath,
                        const std::wstring& inputPath = std::wstring()) {
    if (outPath.empty()) return 2;
    orange::EnsureManagerEnvironment();
    std::string data = orange::BuildPromptApiJson(action, chatKey, inputPath);
    std::ofstream ofs(orange::WideToUtf8(outPath), std::ios::binary);
    if (!ofs.is_open()) return 4;
    ofs.write(data.data(), (std::streamsize)data.size());
    return ofs.good() ? 0 : 5;
}

} // namespace

int APIENTRY wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR, int nShow) {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    orange::StartupOptions startupOptions;
    orange::BackendSettings backendSettings = orange::AppSettings::LoadBackendSettings();
    startupOptions.useClaude = backendSettings.claude;
    startupOptions.useGemini = backendSettings.gemini;
    startupOptions.useCodex = backendSettings.codex;
    startupOptions.managerProvider = backendSettings.managerProvider;
    if (argv) {
        if (HasArg(argc, argv, L"--shutdown-running")) {
            int rc = RunShutdownRunningAppsCommand();
            LocalFree(argv);
            return rc;
        }
        if (HasArg(argc, argv, L"--capture")) {
            std::wstring capturePath = GetArgValue(argc, argv, L"--capture");
            int rc = RunCaptureCommand(capturePath);
            LocalFree(argv);
            return rc;
        }
        if (HasArg(argc, argv, L"--diagnose-manager-env")) {
            std::wstring managerEnvPath = GetArgValue(argc, argv, L"--diagnose-manager-env");
            int rc = RunManagerEnvDiagnostic(managerEnvPath);
            LocalFree(argv);
            return rc;
        }
        if (HasArg(argc, argv, L"--diagnose-attachments")) {
            std::wstring attachmentDiagPath = GetArgValue(argc, argv, L"--diagnose-attachments");
            int rc = RunAttachmentDiagnostic(attachmentDiagPath);
            LocalFree(argv);
            return rc;
        }
        if (HasArg(argc, argv, L"--diagnose-add-attachment")) {
            std::wstring addAttachmentPath = GetArgValue(argc, argv, L"--diagnose-add-attachment");
            std::wstring outPath = GetArgValue(argc, argv, L"--diagnose-output");
            if (outPath.empty()) outPath = addAttachmentPath + L".diagnostic.txt";
            int rc = RunAddAttachmentDiagnostic(addAttachmentPath, outPath);
            LocalFree(argv);
            return rc;
        }
        if (HasArg(argc, argv, L"--diagnose-manager-prompt")) {
            std::wstring managerPromptPath = GetArgValue(argc, argv, L"--diagnose-manager-prompt");
            int rc = RunManagerPromptDiagnostic(managerPromptPath);
            LocalFree(argv);
            return rc;
        }
        if (HasArg(argc, argv, L"--chat-api")) {
            std::wstring chatApiAction = GetArgValue(argc, argv, L"--chat-api");
            std::wstring outPath = GetArgValue(argc, argv, L"--chat-api-output");
            std::wstring chatKey = GetArgValue(argc, argv, L"--chat-key");
            std::wstring inputPath = GetArgValue(argc, argv, L"--chat-api-input");
            std::wstring title = GetArgValue(argc, argv, L"--title");
            unsigned limit = GetUIntArgValue(argc, argv, L"--limit", 40);
            bool overwrite = HasArg(argc, argv, L"--overwrite");
            int rc = RunChatApiCommand(chatApiAction, chatKey, outPath, limit, inputPath, title, overwrite);
            LocalFree(argv);
            return rc;
        }
        if (HasArg(argc, argv, L"--prompt-api")) {
            std::wstring promptApiAction = GetArgValue(argc, argv, L"--prompt-api");
            std::wstring outPath = GetArgValue(argc, argv, L"--prompt-api-output");
            std::wstring chatKey = GetArgValue(argc, argv, L"--chat-key");
            std::wstring inputPath = GetArgValue(argc, argv, L"--prompt-api-input");
            int rc = RunPromptApiCommand(promptApiAction, chatKey, outPath, inputPath);
            LocalFree(argv);
            return rc;
        }
        startupOptions.testPrompt = GetArgValue(argc, argv, L"--test-prompt");
        if (startupOptions.testPrompt.empty()) {
            startupOptions.testPrompt = ReadUtf8TextFile(GetArgValue(argc, argv, L"--test-prompt-file"));
        }
        startupOptions.testCapturePath = GetArgValue(argc, argv, L"--test-capture");
        startupOptions.testCaptureDelayMs = GetUIntArgValue(argc, argv, L"--test-capture-ms", 2500);
        startupOptions.testExitMs = GetUIntArgValue(argc, argv, L"--test-exit-ms", 6000);
        startupOptions.testResponse = GetArgValue(argc, argv, L"--test-response");
        if (startupOptions.testResponse.empty()) {
            startupOptions.testResponse = ReadUtf8TextFile(GetArgValue(argc, argv, L"--test-response-file"));
        }
        startupOptions.testResponseDelayMs = GetUIntArgValue(argc, argv, L"--test-response-ms", 900);
        startupOptions.testPasteClipboardImage = HasArg(argc, argv, L"--test-paste-clipboard-image");
        startupOptions.testAttachmentPath = GetArgValue(argc, argv, L"--test-attachment");
        startupOptions.testShowRetry = HasArg(argc, argv, L"--test-show-retry");
        std::wstring backend = GetArgValue(argc, argv, L"--test-backend");
        if (backend == L"gemini") {
            startupOptions.useClaude = false;
            startupOptions.useGemini = true;
            startupOptions.useCodex = false;
            startupOptions.managerProvider = "gemini";
            startupOptions.backendFromFlag = true;
        } else if (backend == L"claude") {
            startupOptions.useClaude = true;
            startupOptions.useGemini = false;
            startupOptions.useCodex = false;
            startupOptions.managerProvider = "claude";
            startupOptions.backendFromFlag = true;
        } else if (backend == L"codex") {
            startupOptions.useClaude = false;
            startupOptions.useGemini = false;
            startupOptions.useCodex = true;
            startupOptions.managerProvider = "codex";
            startupOptions.backendFromFlag = true;
        } else if (backend == L"both") {
            startupOptions.useClaude = true;
            startupOptions.useGemini = true;
            startupOptions.useCodex = false;
            startupOptions.backendFromFlag = true;
        } else if (backend == L"all") {
            startupOptions.useClaude = true;
            startupOptions.useGemini = true;
            startupOptions.useCodex = true;
        } else if (backend == L"mock" || backend == L"offline") {
            startupOptions.useClaude = false;
            startupOptions.useGemini = true;
            startupOptions.useCodex = false;
            startupOptions.useMockBackend = true;
            startupOptions.backendFromFlag = true;
        }
        if (HasArg(argc, argv, L"--gemini")) {
            startupOptions.useClaude = false;
            startupOptions.useGemini = true;
            startupOptions.useCodex = false;
            startupOptions.managerProvider = "gemini";
            startupOptions.backendFromFlag = true;
        }
        if (HasArg(argc, argv, L"--codex")) {
            startupOptions.useClaude = false;
            startupOptions.useGemini = false;
            startupOptions.useCodex = true;
            startupOptions.managerProvider = "codex";
            startupOptions.backendFromFlag = true;
        }
        LocalFree(argv);
    }

    orange::CMainWindow app;
    app.SetStartupOptions(std::move(startupOptions));
    if (app.Create(hInst, nShow)) {
        app.Run();
    }
    return 0;
}
