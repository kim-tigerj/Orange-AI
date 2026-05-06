#pragma once
#include <memory>
#include <string>
#include <functional>
#include <map>
#include <mutex>
#include <vector>
#include "IClaudeBackend.h"
#include "GeminiPrintBackend.h"
#include "CodexPrintBackend.h"

namespace orange {

class CBackendManager {
public:
    enum class OutputSource { System, Claude, Gemini, Codex, Mock };

    CBackendManager();
    ~CBackendManager() { CancelAll(); }

    void Start(std::function<void(const std::string&, OutputSource, std::wstring)> onOutput,
               std::function<void(const std::string&)> onDone,
               std::function<void(const std::string&, std::wstring)> onRetry = {});

    void SetUseClaude(bool use);
    void SetUseGemini(bool use);
    void SetUseCodex(bool use);

    int SendMessage(const std::string& chatKey, const std::wstring& prompt);
    int SendToProviders(const std::string& chatKey,
                        const std::wstring& prompt,
                        const std::vector<OutputSource>& providers);
    void Cancel(const std::string& chatKey);
    void CancelAll();

private:
    struct ChatBackends {
        std::vector<std::unique_ptr<IBackend>> claude;
        std::vector<std::unique_ptr<IBackend>> gemini;
        std::vector<std::unique_ptr<IBackend>> codex;
    };

    // Council / coordination related types
    struct CouncilBallot {
        std::wstring text;  // 지명 응답 전문
        bool done = false;
    };

    struct CouncilSession {
        std::string chatKey;
        std::wstring originalPrompt;
        std::map<OutputSource, CouncilBallot> ballots;
        int pending = 0;
        bool finalized = false;
        std::mutex mu;  // ballots/pending/finalized 보호
    };

    IBackend* AcquireBackend(const std::string& chatKey, OutputSource source);
    ChatBackends* FindSlot(const std::string& chatKey);
    static std::string NormalizeChatKey(const std::string& chatKey);

    // 단순 병렬 전송 (백엔드 1개이거나 회의 불필요할 때)
    int SendParallel(const std::string& chatKey, const std::wstring& prompt);

    // 3-provider coordination protocol
    void BeginCouncil(const std::string& chatKey, const std::wstring& prompt);
    void FinalizeCouncil(std::shared_ptr<CouncilSession> session);
    static std::wstring MakeNominationPrompt(const std::wstring& original);
    static OutputSource PickWinner(const std::map<OutputSource, CouncilBallot>& ballots,
                                   bool useClaude, bool useGemini, bool useCodex);
    static std::wstring SourceLabel(OutputSource src);

    std::map<std::string, ChatBackends> m_slots;

    bool m_useClaude;
    bool m_useGemini;
    bool m_useCodex;

    // Council 상태 (shared_ptr + CouncilSession::mu 로 스레드 안전 보장)
    std::shared_ptr<CouncilSession> m_activeCouncil;
    // 지명용 임시 백엔드 (채팅 히스토리와 무관; 회의 후 교체)
    std::vector<std::unique_ptr<IBackend>> m_councilBackends;

    std::function<void(const std::string&, OutputSource, std::wstring)> m_onOutput;
    std::function<void(const std::string&)> m_onDone;
    std::function<void(const std::string&, std::wstring)> m_onRetry;
};

} // namespace orange
