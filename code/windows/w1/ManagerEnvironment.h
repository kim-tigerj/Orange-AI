#pragma once

#include <string>

namespace orange {

struct ManagerEnvironment {
    std::wstring root;
    std::wstring role;
    std::wstring pid;
    std::wstring sessionKey;
};

std::wstring OrangeRootFromModule();
ManagerEnvironment EnsureManagerEnvironment();
std::wstring ManagerAppContextDir();
void EnsureManagerAppContextFiles();
std::wstring BuildManagerPrompt(const std::wstring& userPrompt, const ManagerEnvironment& env);
std::wstring BuildProviderPrompt(const std::wstring& providerLabel,
                                 const std::wstring& userPrompt,
                                 const ManagerEnvironment& env);
std::string BuildPromptApiJson(const std::wstring& action,
                               const std::wstring& chatKey,
                               const std::wstring& inputPath);

} // namespace orange
