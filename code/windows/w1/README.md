# Orange Code

Orange Code is a native Windows coding assistant app. Its in-app manager actor is called **정팀장** and uses role name `manager`.

## Goal

Build a practical replacement for Claude Code:

- chat with a coding manager,
- edit and verify code,
- preserve sessions and work state,
- coordinate manager/member/supervisor actors,
- keep UI lightweight and native.

## Roles

| Role | Meaning |
| --- | --- |
| `manager` | 정팀장. The main in-app actor. Receives user requests and manages work. |
| `member` | A bounded worker for one task. |
| `supervisor` | 오감독/reviewer/recovery actor. |

Use `manager` consistently in code, prompts, docs, and issue text.

## Build

```sh
tools/build.sh
bin/Release/OrangeCode.exe
```

## Offline UI Test

```powershell
bin/Release/OrangeCode.exe `
  --test-backend mock `
  --test-prompt "정팀장" `
  --test-response-file tools/mock-code-response.md `
  --test-capture tools/mock-capture.png `
  --test-exit-ms 3600
```

Use real Claude/Gemini calls only for backend integration tests.

## Settings

User settings are stored at `%APPDATA%\OrangeCode\settings.json`.

Backend toggles:

```json
{
  "backend": {
    "claude": true,
    "gemini": false,
    "codex": false
  }
}
```

Gemini model selection:

```json
{
  "gemini": {
    "model": ""
  }
}
```

When `gemini.model` is empty or missing, OrangeCode does not pass `-m`, so Gemini CLI uses its own default/configured model. When non-empty, OrangeCode passes it as `-m "<model>"`.

## Key Files

- `MainWindow.cpp/.h`: app frame and prompt dispatch
- `OrangeView.h`: chat/coding output view
- `OrangeInput.h`: custom input box and IME handling
- `ClaudePrintBackend.cpp/.h`: Claude CLI backend
- `GeminiPrintBackend.cpp/.h`: Gemini CLI backend
- `BackendManager.cpp/.h`: backend selection and dispatch
- `Coordination.cpp/.h`: actor state and coordination
- `Capture.h`: screenshot verification
