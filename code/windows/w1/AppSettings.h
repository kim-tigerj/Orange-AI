#pragma once

#include <windows.h>
#include <shlobj.h>

#include <fstream>
#include <sstream>
#include <string>

#include <json/json.h>

#include "Utils.h"

namespace orange {

struct BackendSettings {
    bool claude = true;
    bool gemini = false;
    bool codex = false;
    std::string managerProvider = "claude";
};

struct GeminiSettings {
    std::wstring model;
};

struct UserSettings {
    std::wstring userName;  // empty = show L"사용자"
};

class AppSettings {
public:
    static BackendSettings LoadBackendSettings() {
        BackendSettings settings;
        std::wstring path = SettingsPath();
        if (path.empty()) return settings;

        Json::Value root;
        if (!ReadRoot(path, root)) return settings;

        const Json::Value backend = root["backend"];
        if (!backend.isObject()) return settings;

        settings.claude = backend.get("claude", settings.claude).asBool();
        settings.gemini = backend.get("gemini", settings.gemini).asBool();
        settings.codex = backend.get("codex", settings.codex).asBool();
        settings.managerProvider = backend.get("manager_provider", settings.managerProvider).asString();
        if (settings.managerProvider != "claude" &&
            settings.managerProvider != "gemini" &&
            settings.managerProvider != "codex") {
            settings.managerProvider = "claude";
        }
        return settings;
    }

    static GeminiSettings LoadGeminiSettings() {
        GeminiSettings settings;
        std::wstring path = SettingsPath();
        if (path.empty()) return settings;

        Json::Value root;
        if (!ReadRoot(path, root)) return settings;

        const Json::Value gemini = root["gemini"];
        if (!gemini.isObject()) return settings;

        std::string model = gemini.get("model", "").asString();
        if (!model.empty()) {
            settings.model = Utf8ToWide(model.c_str(), static_cast<int>(model.size()));
        }
        return settings;
    }

    static UserSettings LoadUserSettings() {
        UserSettings settings;
        std::wstring path = SettingsPath();
        if (path.empty()) return settings;
        Json::Value root;
        if (!ReadRoot(path, root)) return settings;
        const Json::Value user = root["user"];
        if (!user.isObject()) return settings;
        std::string name = user.get("name", "").asString();
        if (!name.empty())
            settings.userName = Utf8ToWide(name.c_str(), static_cast<int>(name.size()));
        return settings;
    }

    static bool SaveUserName(const std::wstring& name) {
        std::wstring path = SettingsPath();
        if (path.empty()) return false;
        Json::Value root;
        ReadRoot(path, root);
        if (!root.isObject()) root = Json::Value(Json::objectValue);
        root["version"] = 1;
        if (!root["user"].isObject()) root["user"] = Json::Value(Json::objectValue);
        root["user"]["name"] = WideToUtf8(name);
        Json::StreamWriterBuilder writer;
        writer["indentation"] = "  ";
        std::string content = Json::writeString(writer, root);
        std::wstring tmpPath = path + L".tmp";
        {
            std::ofstream ofs(WideToUtf8(tmpPath), std::ios::binary | std::ios::trunc);
            if (!ofs.is_open()) return false;
            ofs.write(content.data(), static_cast<std::streamsize>(content.size()));
            if (!ofs) { ofs.close(); DeleteFileW(tmpPath.c_str()); return false; }
        }
        if (!MoveFileExW(tmpPath.c_str(), path.c_str(),
                         MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            DeleteFileW(tmpPath.c_str());
            return false;
        }
        return true;
    }

    static bool SaveGeminiModel(const std::wstring& model) {
        std::wstring path = SettingsPath();
        if (path.empty()) return false;

        Json::Value root;
        ReadRoot(path, root);
        if (!root.isObject()) root = Json::Value(Json::objectValue);

        root["version"] = 1;
        if (!root["gemini"].isObject()) root["gemini"] = Json::Value(Json::objectValue);
        root["gemini"]["model"] = WideToUtf8(model);

        Json::StreamWriterBuilder writer;
        writer["indentation"] = "  ";
        std::string content = Json::writeString(writer, root);

        std::wstring tmpPath = path + L".tmp";
        {
            std::ofstream ofs(WideToUtf8(tmpPath), std::ios::binary | std::ios::trunc);
            if (!ofs.is_open()) return false;
            ofs.write(content.data(), static_cast<std::streamsize>(content.size()));
            if (!ofs) {
                ofs.close();
                DeleteFileW(tmpPath.c_str());
                return false;
            }
        }
        if (!MoveFileExW(tmpPath.c_str(), path.c_str(),
                         MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            DeleteFileW(tmpPath.c_str());
            return false;
        }
        return true;
    }

    static bool SaveBackendSettings(bool claude, bool gemini, bool codex,
                                    const std::string& managerProvider = "claude") {
        std::wstring path = SettingsPath();
        if (path.empty()) return false;

        Json::Value root;
        ReadRoot(path, root);
        if (!root.isObject()) root = Json::Value(Json::objectValue);

        root["version"] = 1;
        root["backend"]["claude"] = claude;
        root["backend"]["gemini"] = gemini;
        root["backend"]["codex"] = codex;
        root["backend"]["manager_provider"] = managerProvider;
        if (!root["gemini"].isObject()) root["gemini"] = Json::Value(Json::objectValue);
        if (!root["gemini"].isMember("model") || !root["gemini"]["model"].isString()) {
            root["gemini"]["model"] = "";
        }

        Json::StreamWriterBuilder writer;
        writer["indentation"] = "  ";
        std::string content = Json::writeString(writer, root);

        std::wstring tmpPath = path + L".tmp";
        {
            std::ofstream ofs(WideToUtf8(tmpPath), std::ios::binary | std::ios::trunc);
            if (!ofs.is_open()) return false;
            ofs.write(content.data(), static_cast<std::streamsize>(content.size()));
            if (!ofs) {
                ofs.close();
                DeleteFileW(tmpPath.c_str());
                return false;
            }
        }

        if (!MoveFileExW(tmpPath.c_str(), path.c_str(),
                         MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            DeleteFileW(tmpPath.c_str());
            return false;
        }
        return true;
    }

    static std::wstring SettingsPath() {
        wchar_t appdata[MAX_PATH] = L"";
        if (FAILED(SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, appdata))) {
            return L"";
        }

        std::wstring dir = std::wstring(appdata) + L"\\OrangeCode";
        if (!CreateDirectoryW(dir.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
            return L"";
        }
        return dir + L"\\settings.json";
    }

private:
    static bool ReadRoot(const std::wstring& path, Json::Value& root) {
        std::ifstream ifs(WideToUtf8(path), std::ios::binary);
        if (!ifs.is_open()) return false;

        Json::CharReaderBuilder reader;
        std::string errors;
        if (!Json::parseFromStream(reader, ifs, &root, &errors)) {
            root = Json::Value();
            return false;
        }
        return true;
    }
};

} // namespace orange
