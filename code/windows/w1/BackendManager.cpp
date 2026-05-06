#include "BackendManager.h"
#include "ClaudePrintBackend.h"
#include "GeminiPrintBackend.h"
#include "CodexPrintBackend.h"

namespace orange {

CBackendManager::CBackendManager()
    : m_useClaude(true), m_useGemini(false), m_useCodex(false) {}

std::string CBackendManager::NormalizeChatKey(const std::string& chatKey) {
    return chatKey.empty() ? std::string("default") : chatKey;
}

void CBackendManager::Start(
    std::function<void(const std::string&, OutputSource, std::wstring)> onOutput,
    std::function<void(const std::string&)> onDone,
    std::function<void(const std::string&, std::wstring)> onRetry)
{
    m_onOutput = std::move(onOutput);
    m_onDone   = std::move(onDone);
    m_onRetry  = std::move(onRetry);
}

CBackendManager::ChatBackends* CBackendManager::FindSlot(const std::string& chatKey) {
    std::string key = NormalizeChatKey(chatKey);
    auto it = m_slots.find(key);
    if (it == m_slots.end()) return nullptr;
    return &it->second;
}

IBackend* CBackendManager::AcquireBackend(const std::string& chatKey, OutputSource source) {
    std::string key = NormalizeChatKey(chatKey);
    ChatBackends& slot = m_slots[key];

    std::vector<std::unique_ptr<IBackend>>* list = nullptr;
    if (source == OutputSource::Claude) list = &slot.claude;
    else if (source == OutputSource::Gemini) list = &slot.gemini;
    else if (source == OutputSource::Codex)  list = &slot.codex;
    if (!list) return nullptr;

    for (auto& b : *list) {
        if (b && b->IsReady()) return b.get();
    }

    std::unique_ptr<IBackend> backend;
    if (source == OutputSource::Claude) backend = std::make_unique<ClaudePrintBackend>();
    else if (source == OutputSource::Gemini) backend = std::make_unique<GeminiPrintBackend>();
    else if (source == OutputSource::Codex)  backend = std::make_unique<CodexPrintBackend>();
    if (!backend) return nullptr;

    bool started = backend->Start(
        [this, key, source](std::wstring text) {
            if (m_onOutput) m_onOutput(key, source, std::move(text));
        },
        [this, key]() {
            if (m_onDone) m_onDone(key);
        },
        {},
        {},
        [this, key](std::wstring errMsg) {
            if (m_onRetry) m_onRetry(key, std::move(errMsg));
        });
    if (!started) return nullptr;

    IBackend* raw = backend.get();
    list->push_back(std::move(backend));
    return raw;
}

void CBackendManager::SetUseClaude(bool use) { m_useClaude = use; }
void CBackendManager::SetUseGemini(bool use) { m_useGemini = use; }
void CBackendManager::SetUseCodex(bool use)  { m_useCodex  = use; }

// ── 단순 병렬 전송 ────────────────────────────────────────────────────────────

int CBackendManager::SendParallel(const std::string& chatKey, const std::wstring& prompt) {
    int sentCount = 0;
    auto sendOne = [&](OutputSource source) {
        IBackend* b = AcquireBackend(chatKey, source);
        if (b && b->SendPrompt(prompt)) ++sentCount;
    };
    if (m_useClaude) sendOne(OutputSource::Claude);
    if (m_useGemini) sendOne(OutputSource::Gemini);
    if (m_useCodex)  sendOne(OutputSource::Codex);

    const std::string key = NormalizeChatKey(chatKey);
    if (sentCount == 0 && m_onOutput) {
        bool anyActive = m_useClaude || m_useGemini || m_useCodex;
        m_onOutput(key, OutputSource::System,
            anyActive
                ? L"[백엔드 오류] 선택된 LLM을 시작하지 못했습니다. CLI 설치·인증·PATH를 확인하세요.\r\n"
                : L"[백엔드 오류] 선택된 LLM이 없습니다. Claude·Gemini·Codex 중 하나를 선택하세요.\r\n");
        if (m_onDone) m_onDone(key);
    }
    return sentCount;
}

// ── 3-provider coordination protocol ─────────────────────────────────────────

int CBackendManager::SendToProviders(const std::string& chatKey,
                                     const std::wstring& prompt,
                                     const std::vector<OutputSource>& providers) {
    int sentCount = 0;
    for (OutputSource source : providers) {
        if (source != OutputSource::Claude &&
            source != OutputSource::Gemini &&
            source != OutputSource::Codex) {
            continue;
        }
        IBackend* b = AcquireBackend(chatKey, source);
        if (b && b->SendPrompt(prompt)) ++sentCount;
    }

    const std::string key = NormalizeChatKey(chatKey);
    if (sentCount == 0 && m_onOutput) {
        m_onOutput(key, OutputSource::System,
                   L"[백엔드 오류] 위임 대상 LLM을 시작하지 못했습니다. CLI 설치·인증·PATH를 확인하세요.\r\n");
    }
    return sentCount;
}

/* static */ std::wstring CBackendManager::MakeNominationPrompt(const std::wstring& original) {
    std::wstring summary = (original.size() > 200)
        ? original.substr(0, 200) + L"…"
        : original;
    return
        L"[Coordination meeting] Decide the responsible model for the request below.\n"
        L"요청: " + summary + L"\n\n"
        L"이 요청이 네 전문 영역이면 첫 줄에 CLAIM, 아니면 DEFER를 써라.\n"
        L"이유를 한 줄만 덧붙이고 그 외 내용은 쓰지 마라.";
}

/* static */ CBackendManager::OutputSource CBackendManager::PickWinner(
    const std::map<OutputSource, CouncilBallot>& ballots,
    bool useClaude, bool useGemini, bool useCodex)
{
    auto claimed = [&](OutputSource src) -> bool {
        auto it = ballots.find(src);
        return it != ballots.end() && it->second.text.find(L"CLAIM") != std::wstring::npos;
    };
    // 우선순위: Claude > Gemini > Codex
    if (useClaude && claimed(OutputSource::Claude)) return OutputSource::Claude;
    if (useGemini && claimed(OutputSource::Gemini)) return OutputSource::Gemini;
    if (useCodex  && claimed(OutputSource::Codex))  return OutputSource::Codex;
    // 아무도 안 나서면 기본값
    if (useClaude) return OutputSource::Claude;
    if (useGemini) return OutputSource::Gemini;
    return OutputSource::Codex;
}

/* static */ std::wstring CBackendManager::SourceLabel(OutputSource src) {
    switch (src) {
    case OutputSource::Claude: return L"Claude";
    case OutputSource::Gemini: return L"Gemini";
    case OutputSource::Codex:  return L"Codex";
    default:                   return L"?";
    }
}

void CBackendManager::BeginCouncil(const std::string& chatKey, const std::wstring& prompt) {
    // 이전 회의 임시 백엔드 정리 (이전 백엔드들은 이미 완료 상태)
    m_councilBackends.clear();

    auto session = std::make_shared<CouncilSession>();
    session->chatKey = NormalizeChatKey(chatKey);
    session->originalPrompt = prompt;

    int count = 0;
    if (m_useClaude) { session->ballots[OutputSource::Claude] = {}; ++count; }
    if (m_useGemini) { session->ballots[OutputSource::Gemini] = {}; ++count; }
    if (m_useCodex)  { session->ballots[OutputSource::Codex]  = {}; ++count; }
    session->pending = count;
    m_activeCouncil = session;

    if (m_onOutput) {
        m_onOutput(session->chatKey, OutputSource::System,
            L"[회의] " + std::to_wstring(count) + L"명이 담당자를 정하는 중...\r\n");
    }

    std::wstring nomPrompt = MakeNominationPrompt(prompt);

    auto nominate = [&](OutputSource src) {
        std::unique_ptr<IBackend> temp;
        if      (src == OutputSource::Claude) temp = std::make_unique<ClaudePrintBackend>();
        else if (src == OutputSource::Gemini) temp = std::make_unique<GeminiPrintBackend>();
        else                                  temp = std::make_unique<CodexPrintBackend>();

        // weak_ptr 캡처 — CouncilSession 이 tempBackend 를 소유하지 않으므로 순환 참조 없음
        std::weak_ptr<CouncilSession> weak = session;

        bool ok = temp->Start(
            [weak, src](std::wstring text) {
                if (auto s = weak.lock()) {
                    std::lock_guard<std::mutex> lk(s->mu);
                    s->ballots[src].text += text;
                }
            },
            [this, weak, src]() {
                auto s = weak.lock();
                if (!s) return;
                bool doFinalize = false;
                {
                    std::lock_guard<std::mutex> lk(s->mu);
                    s->ballots[src].done = true;
                    if (--s->pending == 0 && !s->finalized) {
                        s->finalized = true;
                        doFinalize = true;
                    }
                }
                if (doFinalize) FinalizeCouncil(s);
            });

        if (ok) {
            temp->SendPrompt(nomPrompt);
            m_councilBackends.push_back(std::move(temp));
        } else {
            // 시작 실패 — pending 수동 감소
            std::lock_guard<std::mutex> lk(session->mu);
            session->ballots[src].done = true;
            --session->pending;
        }
    };

    if (m_useClaude) nominate(OutputSource::Claude);
    if (m_useGemini) nominate(OutputSource::Gemini);
    if (m_useCodex)  nominate(OutputSource::Codex);

    // 모두 시작 실패한 경우 즉시 마무리
    bool allFailed = false;
    {
        std::lock_guard<std::mutex> lk(session->mu);
        allFailed = (session->pending == 0 && !session->finalized);
        if (allFailed) session->finalized = true;
    }
    if (allFailed) FinalizeCouncil(session);
}

void CBackendManager::FinalizeCouncil(std::shared_ptr<CouncilSession> session) {
    OutputSource winner = PickWinner(session->ballots, m_useClaude, m_useGemini, m_useCodex);

    // 당선 이유 추출 (CLAIM/DEFER 줄 다음 첫 번째 줄)
    std::wstring reason;
    auto it = session->ballots.find(winner);
    if (it != session->ballots.end() && !it->second.text.empty()) {
        const std::wstring& text = it->second.text;
        size_t nl = text.find(L'\n');
        if (nl != std::wstring::npos) {
            reason = text.substr(nl + 1);
            while (!reason.empty() &&
                   (reason.front() == L'\r' || reason.front() == L'\n' || reason.front() == L' '))
                reason.erase(reason.begin());
            size_t nl2 = reason.find_first_of(L"\r\n");
            if (nl2 != std::wstring::npos) reason = reason.substr(0, nl2);
        }
    }

    std::wstring label = SourceLabel(winner);
    std::wstring msg = L"[회의] " + label + L" 담당";
    if (!reason.empty()) msg += L" — " + reason;
    msg += L"\r\n";

    if (m_onOutput) m_onOutput(session->chatKey, OutputSource::System, msg);

    // 당첨자에게만 원본 프롬프트 전송
    IBackend* backend = AcquireBackend(session->chatKey, winner);
    if (backend) {
        backend->SendPrompt(session->originalPrompt);
    } else {
        if (m_onOutput)
            m_onOutput(session->chatKey, OutputSource::System, L"[오류] 담당 백엔드 시작 실패\r\n");
        if (m_onDone) m_onDone(session->chatKey);
    }
}

// ── 공개 API ─────────────────────────────────────────────────────────────────

int CBackendManager::SendMessage(const std::string& chatKey, const std::wstring& prompt) {
    int activeCount = (int)m_useClaude + (int)m_useGemini + (int)m_useCodex;

    if (activeCount == 0) {
        const std::string key = NormalizeChatKey(chatKey);
        if (m_onOutput)
            m_onOutput(key, OutputSource::System,
                L"[백엔드 오류] 선택된 LLM이 없습니다. Claude·Gemini·Codex 중 하나를 선택하세요.\r\n");
        if (m_onDone) m_onDone(key);
        return 0;
    }

    // 2개 이상 활성 → coordination meeting으로 단일 담당자 결정
    return SendParallel(chatKey, prompt);
}

void CBackendManager::Cancel(const std::string& chatKey) {
    ChatBackends* slot = FindSlot(chatKey);
    if (!slot) return;
    for (auto& b : slot->claude) if (b) b->Cancel();
    for (auto& b : slot->gemini) if (b) b->Cancel();
    for (auto& b : slot->codex)  if (b) b->Cancel();
}

void CBackendManager::CancelAll() {
    for (auto& kv : m_slots) {
        for (auto& b : kv.second.claude) if (b) b->Cancel();
        for (auto& b : kv.second.gemini) if (b) b->Cancel();
        for (auto& b : kv.second.codex)  if (b) b->Cancel();
    }
    for (auto& b : m_councilBackends) if (b) b->Cancel();
}

} // namespace orange
