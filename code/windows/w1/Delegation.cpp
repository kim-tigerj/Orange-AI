#include "Delegation.h"

#include "Utils.h"

namespace orange {

namespace {

std::wstring JsonStringWideLocal(const Json::Value& value, const char* key) {
    if (!value.isObject() || !value[key].isString()) return {};
    std::string text = value[key].asString();
    return Utf8ToWide(text.c_str(), (int)text.size());
}

void LowerAscii(std::string& text) {
    for (char& ch : text) {
        if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
    }
}

bool AddProvider(std::string name, DelegationRequest& out) {
    LowerAscii(name);
    while (!name.empty() && name.front() == ' ') name.erase(name.begin());
    while (!name.empty() && name.back() == ' ') name.pop_back();
    if (name == "claude") {
        out.providers.push_back(CBackendManager::OutputSource::Claude);
    } else if (name == "gemini") {
        out.providers.push_back(CBackendManager::OutputSource::Gemini);
    } else if (name == "codex") {
        out.providers.push_back(CBackendManager::OutputSource::Codex);
    } else {
        return false;
    }
    out.roster += (out.roster.empty() ? L"" : L", ") + DelegationSourceLabel(out.providers.back());
    return true;
}

} // namespace

std::wstring DelegationSourceLabel(CBackendManager::OutputSource source) {
    if (source == CBackendManager::OutputSource::Claude) return L"Claude";
    if (source == CBackendManager::OutputSource::Gemini) return L"Gemini";
    if (source == CBackendManager::OutputSource::Codex) return L"Codex";
    if (source == CBackendManager::OutputSource::Mock) return L"Mock";
    return L"System";
}

bool ParseDelegationRequest(const Json::Value& args, DelegationRequest& out) {
    out = DelegationRequest{};

    const Json::Value& providers = args["providers"];
    if (providers.isArray()) {
        for (Json::ArrayIndex i = 0; i < providers.size(); ++i) {
            if (providers[i].isString()) AddProvider(providers[i].asString(), out);
        }
    } else if (providers.isString()) {
        std::string csv = providers.asString();
        size_t start = 0;
        while (start < csv.size()) {
            size_t comma = csv.find(',', start);
            std::string item = csv.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
            AddProvider(item, out);
            if (comma == std::string::npos) break;
            start = comma + 1;
        }
    }

    if (out.providers.size() > 4) {
        out.providers.resize(4);
        out.roster.clear();
        for (auto provider : out.providers) {
            out.roster += (out.roster.empty() ? L"" : L", ") + DelegationSourceLabel(provider);
        }
    }

    out.prompt = JsonStringWideLocal(args, "prompt");
    if (out.prompt.empty()) out.prompt = JsonStringWideLocal(args, "task");
    if (out.prompt.empty()) out.prompt = JsonStringWideLocal(args, "request");

    out.scope = JsonStringWideLocal(args, "scope");

    return !out.providers.empty() && !out.prompt.empty();
}

std::wstring BuildDelegatedPrompt(const std::wstring& delegator,
                                  const std::string& chatKey,
                                  const std::wstring& task,
                                  const std::wstring& scope) {
    std::wstring prompt;
    prompt += L"OrangeCode delegated work.\n";
    prompt += L"- Delegated by: " + delegator + L"\n";
    prompt += L"- Current chat key: " + Utf8ToWide(chatKey.c_str(), static_cast<int>(chatKey.size())) + L"\n";
    prompt += L"- Role: reviewer/advisor only. Do NOT edit files. Do NOT request orange.build, orange.capture, or orange.exec_admin.\n";
    prompt += L"- The coordinator (" + delegator + L") is the sole executor. You return analysis, risks, and patch suggestions only.\n";
    if (!scope.empty()) {
        prompt += L"- Your assigned scope: " + scope + L". Do not comment on or propose changes to files outside this scope.\n";
    }
    prompt += L"- The human user is the final decision maker. Do not address the user as any internal supervisor/review role.\n";
    prompt += L"- Return only: findings, risks, and concrete patch recommendation (diff or before/after). No extra commentary.\n\n";
    prompt += L"Delegated task:\n";
    prompt += task;
    return prompt;
}

std::wstring BuildDelegationCard(const std::wstring& delegator,
                                 const std::wstring& roster,
                                 int sent,
                                 const std::wstring& task) {
    std::wstring card;
    card += L"위임 실행\n\n";
    card += L"- 요청자: " + delegator + L"\n";
    card += L"- 대상: " + roster + L"\n";
    card += L"- 실행: " + std::to_wstring(sent) + L"개 백엔드\n";
    card += L"- 표시: 팀원 응답은 각 provider 채팅 블록에 직접 표시\n";
    card += L"- 작업: " + task;
    return card;
}

std::wstring BuildDelegationErrorCard(const std::wstring& reason) {
    return L"위임 요청 차단\n\n"
           L"- 도구: orange.delegate\n"
           L"- 이유: " + reason;
}

} // namespace orange
