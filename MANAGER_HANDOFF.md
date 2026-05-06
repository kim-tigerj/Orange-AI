# Manager Handoff

Current date: 2026-05-06

This file is supplemental recovery context for OrangeCode. The SQLite `prompt`,
`job`, and `job_detail` tables are the highest-priority runtime source. Do not
auto-execute old markdown action lists unless the latest user request or DB job
explicitly asks for them.

## Current Truth

- Product direction: OrangeCode is a Windows native coding workbench for
  `manager` work, not a general chat app.
- Required role name for Jeong Team Lead is `manager`.
- Oh-Council is the Claude Code + Gemini + Codex supervisory protocol. It is
  used for important direction, architecture, backend integration, UX, recovery,
  or review decisions. Small obvious fixes may be handled as `Local-Fix`.
- One provider is the appointed execution owner. Other providers are reviewers
  or advisors unless explicitly delegated a bounded task.
- The appointed manager provider comes only from OrangeCode app settings and
  `ORANGE_CODE_MANAGER_PROVIDER`. Markdown files, role docs, and older chat
  history naming another provider are stale supplemental context.

## Current Implemented Capabilities

- SQLite prompt/job foundation:
  - `prompt`, `job`, and `job_detail` tables exist.
  - Generated backend prompts start with active DB prompt rows.
  - User prompts are recorded as DB jobs, with owner-style instructions given
    higher priority.
- Provider authority guard:
  - Backend prompts include the responding provider and appointed manager
    provider.
  - Non-appointed providers are told that `ORANGE_CODE_ROLE=manager` is the app
    actor role, not file-edit/build authority.
- Provider relay command parser:
  - Relay requires explicit syntax at line start: `/relay`, `/ask`, Korean relay
    command variants, or `@provider`.
  - Plain mentions of Claude/Gemini/Codex must not trigger provider relay.
  - Relay has a timeout fallback so work continues if a target provider is
    silent.
- Decision-to-action rules:
  - When provider opinions repeat or enough opinions have arrived, the appointed
    `manager` must decide and take the next concrete action.
  - Progress reports should state actual change, actual verification, blocker,
    or requested OrangeCode tool action. Avoid repeated generic future-action
    narration.
- Self-build/relaunch:
  - `orange.build` can move locked Release outputs aside with unique `.old`
    names.
  - Manager-requested successful builds can relaunch immediately instead of
    waiting for unrelated pending reviewers.
- Shared chat memory:
  - Chat API supports list/show/attachments/import through file-output JSON.
  - Backends are told to inspect shared SQLite chat history when needed instead
    of assuming unavailable context.
- Workbench body:
  - Direct2D sidebar is bound to real SQLite chat/goal/project data.
  - Large chat sessions load recent blocks first and can prepend older blocks on
    near-top scroll.
  - Attachments, image thumbnails, `/capture`, `/settings`, and `/gemini.model`
    are implemented.
  - Rate-limit retry countdown and ESC cancellation are implemented.

## Verification Snapshot

- Source version currently records `0.1.0.57` in
  `code/windows/w1/resource.h`.
- Multiple Release x64 builds passed during the 2026-05-06 work. Some builds
  used `bin\ReleaseVerify\OrangeCode.exe` when the running app locked
  `bin\Release\OrangeCode.exe`.
- Mock captures exist for UI/settings/retry/sidebar checks under
  `code/windows/w1/tools/`.

## Known Risks

- The git worktree is very dirty, with many untracked generated files and
  captures. Do not assume every changed file is intentional product source.
- Some older Korean text in markdown files is mojibake. Prefer source code,
  DB prompt rows, and recent verified task files over corrupted archive text.
- Old build-publication reminders are stale unless the current DB job or latest
  user message repeats that request.

## Next Useful Work

1. Confirm the running app is actually using the latest intended Release build.
2. Run one end-to-end DB-first manager loop: user request -> job row -> bounded
   implementation or local command -> build/mock/capture evidence -> concise
   final report.
3. Clean the repository working tree by separating source changes from generated
   diagnostics, captures, and temporary JSON files.
4. Normalize or remove mojibake task/archive documents only when they affect the
   current manager loop.

## Preferred Verification

```powershell
cd O:\Work\OrangeLabs\Orange\Orange-AI\code\windows\w1
& "C:\Program Files\Git\bin\bash.exe" tools/build.sh
.\bin\Release\OrangeCode.exe --prompt-api show --chat-key <chat-key> --prompt-api-output tools\prompt-api-show.json
.\bin\Release\OrangeCode.exe --diagnose-manager-prompt tools\manager-prompt-diagnostic.txt
.\bin\Release\OrangeCode.exe --test-backend mock --test-prompt "/settings" --test-capture tools\settings-command.png
```
