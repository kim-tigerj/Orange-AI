# Manager Handoff

Current date: 2026-05-06

This file is supplemental recovery context for the OrangeCode app workspace.
The SQLite `prompt`, `job`, and `job_detail` tables are the highest-priority
runtime source. Do not auto-execute old markdown action lists unless the latest
user request or DB job explicitly asks for them.

## Current Truth

- OrangeCode is a Windows native coding workbench for `manager` work, not a
  general chat app.
- Required role name for Jeong Team Lead is `manager`.
- The appointed manager provider is sourced only from OrangeCode app settings
  and `ORANGE_CODE_MANAGER_PROVIDER`.
- Non-appointed providers are reviewers/advisors unless explicitly delegated a
  bounded task.
- Oh-Council is the Claude Code + Gemini + Codex supervisory protocol. Use it
  for important direction, architecture, backend integration, UX, persistence,
  recovery, or review decisions. Use Local-Fix for small obvious changes.

## Current Implemented Capabilities

- DB-first prompt/job foundation:
  - SQLite tables `prompt`, `job`, and `job_detail` exist.
  - Backend prompts start with active DB prompt rows.
  - User prompts are recorded as DB jobs, with owner-style instructions given
    higher priority.
- Provider authority guard:
  - Prompts include the responding provider and appointed manager provider.
  - `ORANGE_CODE_ROLE=manager` is the app actor role, not provider execution
    authority for non-appointed reviewers.
- Provider relay:
  - Relay requires explicit syntax at line start: `/relay`, `/ask`, Korean relay
    command variants, or `@provider`.
  - Plain provider names are discussion text and must not trigger relay.
  - Relay timeout fallback keeps the work moving if a target provider is silent.
- Decision closure:
  - After selected providers give opinions, or when discussion repeats, the
    appointed `manager` closes with a decision and takes the next concrete
    action.
  - Final manager callback exists so reviewer responses do not end in an
    opinion-only loop.
- Self-build:
  - `orange.build` moves locked Release outputs aside with unique `.old` names.
  - Manager-requested successful builds can relaunch immediately.
  - `tools/build.sh` is the preferred build entry point.
- Shared memory:
  - Chat API supports list/show/attachments/import through file-output JSON.
  - Attachments, thumbnails, `/capture`, `/settings`, and `/gemini.model` exist.
- UI/workbench:
  - Direct2D sidebar uses real SQLite chat/goal/project data.
  - Large chats load recent blocks first and prepend older blocks on near-top
    scroll.
  - Rate-limit retry countdown and ESC cancellation exist.

## Verification Snapshot

- Source version currently records `0.1.0.61` in `resource.h`.
- Latest local verification:
  - `& "C:\Program Files\Git\bin\bash.exe" code/windows/w1/tools/build.sh`
  - Result: Release x64 build passed and produced
    `code/windows/w1/bin/Release/OrangeCode.exe`.
- Previous mock/UI evidence files exist under `tools/`.

## Known Risks

- The git worktree contains many generated captures, JSON diagnostics, logs,
  and backup files. Do not treat every untracked file as source.
- Some old Korean archive text is mojibake. Prefer current source code, DB
  prompt rows, and recent task specs over corrupted historical text.
- Old build-publication reminders are stale unless the current DB job or latest
  user request repeats that instruction.

## Next Useful Work

1. Prove one DB-first manager loop end to end:
   user request -> job row -> bounded action -> build/mock/capture evidence ->
   concise final report.
2. Clean repository hygiene by ignoring or archiving generated diagnostics and
   captures.
3. Normalize only the old mojibake documents that still affect the active
   manager loop.

## Preferred Verification

```powershell
cd O:\Work\OrangeLabs\Orange\Orange-AI
& "C:\Program Files\Git\bin\bash.exe" code/windows/w1/tools/build.sh
cd code/windows/w1
.\bin\Release\OrangeCode.exe --prompt-api show --chat-key <chat-key> --prompt-api-output tools\prompt-api-show.json
.\bin\Release\OrangeCode.exe --diagnose-manager-prompt tools\manager-prompt-diagnostic.txt
.\bin\Release\OrangeCode.exe --test-backend mock --test-prompt "/settings" --test-capture tools\settings-command.png
```
