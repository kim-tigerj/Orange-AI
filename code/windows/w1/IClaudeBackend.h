#pragma once

// IBackend — LLM 호출 추상화 인터페이스.
//
// 우리 앱은 *어떤 LLM 도구* 든 같은 결로 사용한다. 첫 구현은 claude CLI 의 헤드리스
// 모드(--print --output-format stream-json)지만, Gemini CLI · ChatGPT CLI · 또는
// 어떤 LLM API 든 이 인터페이스만 구현하면 main.cpp 변경 없이 끼울 수 있다.
//
// 현재 구현체:
//   - CliBackend  : LLM CLI 헤드리스 호출 (1차 구현은 claude CLI). Max 등 구독 비용 활용
//   - ReplBackend : LLM CLI 의 REPL 화면을 ConPTY 로 띄우는 시도 (legacy. 비활성)
//   - ApiBackend  : LLM API 직접 호출 (Anthropic API 등, BYOK) — 추후
//
// main.cpp 는 이 인터페이스만 의존. 백엔드 교체 시 main 수정 최소화.

#include <functional>
#include <string>

namespace orange {

class IBackend {
public:
    // 출력 청크 콜백. 워커 스레드에서 호출될 수 있음 — 호출자는 UI 스레드로 마샬링 책임.
    using OutputCallback   = std::function<void(std::wstring)>;
    // 한 턴(요청·응답) 완료 콜백. 워커 스레드에서 호출.
    using TurnDoneCallback = std::function<void()>;
    // 도구 호출 이벤트 콜백 (assistant 메시지 안의 tool_use 발생 시). 워커 스레드.
    // toolName 만 전달 — 호출자는 별도 블록(role "tool") 으로 표시 책임.
    using ToolUseCallback  = std::function<void(std::wstring toolName)>;
    // 에러 이벤트 콜백 (result.is_error 등). 워커 스레드.
    // errorMsg 만 전달 — 호출자는 별도 블록(role "error") 으로 표시 책임.
    using ErrorCallback    = std::function<void(std::wstring errorMsg)>;
    // *일시적 서버 제한* 에러 콜백. result.is_error 중에서 rate limit / overloaded 같은
    // 패턴이 잡히면 errorCb 대신 이 콜백을 호출합니다 — 호출자는 카운트다운 + 자동 재발송
    // 자세를 잡습니다 (auto_retry_rate_limit 사이클). 영구 에러는 기존 errorCb 만.
    using RetryCallback    = std::function<void(std::wstring errorMsg)>;

    virtual ~IBackend() = default;

    // 백엔드 초기화. 출력은 outputCb, 턴 완료는 turnDoneCb, 도구 호출은 toolUseCb, 에러는 errorCb.
    virtual bool Start(OutputCallback outputCb,
                       TurnDoneCallback turnDoneCb = {},
                       ToolUseCallback toolUseCb   = {},
                       ErrorCallback   errorCb     = {},
                       RetryCallback   retryCb     = {}) = 0;

    // 사용자 메시지 전송. 응답은 outputCb 로 스트리밍.
    virtual bool SendPrompt(const std::wstring& prompt) = 0;

    // 정리 (프로세스 종료, 핸들 해제 등). 멱등.
    virtual void Stop() = 0;

    // 진행 중인 한 턴만 중단 (자식 프로세스 종료). Stop 과 달리 백엔드 자체는 살아있음.
    // 미지원 백엔드는 기본 no-op.
    virtual void Cancel() {}

    // 호출 가능 상태인지
    virtual bool IsReady() const = 0;

    // ---- 세션 ID 관리 (resume 용. 미지원 백엔드는 기본 no-op) ----
    virtual std::string CurrentSessionId() const          { return {}; }
    virtual void        SetResumeSessionId(const std::string&) {}
};

}  // namespace orange
