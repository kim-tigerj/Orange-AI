#pragma once

#include <string>
#include <vector>

#include <json/json.h>

#include "BackendManager.h"

namespace orange {

struct DelegationRequest {
    std::vector<CBackendManager::OutputSource> providers;
    std::wstring prompt;
    std::wstring roster;
    std::wstring scope; // optional: files/areas this delegatee is allowed to touch
};

std::wstring DelegationSourceLabel(CBackendManager::OutputSource source);
bool ParseDelegationRequest(const Json::Value& args, DelegationRequest& out);
std::wstring BuildDelegatedPrompt(const std::wstring& delegator,
                                  const std::string& chatKey,
                                  const std::wstring& task,
                                  const std::wstring& scope = {});
std::wstring BuildDelegationCard(const std::wstring& delegator,
                                 const std::wstring& roster,
                                 int sent,
                                 const std::wstring& task);
std::wstring BuildDelegationErrorCard(const std::wstring& reason);

} // namespace orange
