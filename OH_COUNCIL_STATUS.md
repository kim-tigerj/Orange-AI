# Oh-Council CLI Status

Current date: 2026-05-06

This is a compact status board for CLI-side Oh-Council work. It should contain
only current operating truth and recent evidence. Detailed history belongs in
task files, reports, commits, or chat records.

## Identity

- Oh-Council is the three-party supervisory system: Claude Code, Gemini, and
  Codex.
- A local CLI session may act as chair/executor, but it does not replace the
  council.
- Important direction changes use Council -> Plan -> Execution -> Review ->
  Decision.
- Small obvious fixes may use `Local-Fix` when real collaborator calls would not
  improve speed, direction, review, or recovery.

## Current Board

| Field | Status |
| --- | --- |
| Mode | Local-Fix |
| Focus | Cleanup of misleading handoff/status markdown |
| Owner | Oh-Council chair/executor |
| Collaboration | No new Claude/Gemini/Codex calls needed for this document cleanup |
| Cost note | Local file cleanup only |
| Review | Diff review of `MANAGER_HANDOFF.md` and `OH_COUNCIL_STATUS.md` |
| Evidence | Source version is `0.1.0.57`; DB-first prompt/job foundation is present in code |

## Standing Rules

- DB prompt rows and DB job briefing outrank supplemental markdown files.
- Do not auto-run stale markdown action lists after restart.
- The appointed manager provider is sourced only from app settings and
  `ORANGE_CODE_MANAGER_PROVIDER`.
- Non-appointed providers are reviewers/advisors unless explicitly delegated.
- Provider relay requires explicit command syntax such as `/relay`, `/ask`, or
  `@provider`; plain provider names are discussion text.
- When opinions repeat or enough opinions have arrived, the appointed `manager`
  closes with a decision and takes the next concrete action.
- Record durable decisions in the smallest relevant file:
  - `MANAGER_HANDOFF.md` for current state and next useful work.
  - `code/windows/w1/TOKEN_BUDGET_PROTOCOL.md` for standing loop rules.
  - `OH_COUNCIL.md` for the council protocol.
  - `doc/DECISIONS.md` for long-lived product or architecture decisions.

## Recent Evidence

- OrangeCode source version: `code/windows/w1/resource.h` -> `0.1.0.57`.
- SQLite prompt/job schema exists in `code/windows/w1/Database.h` and
  `code/windows/w1/ManagerEnvironment.cpp`.
- Provider-scoped prompt construction exists in `BuildProviderPrompt()`.
- Explicit relay parsing exists in `CCoordination::TryParseProviderRelayCommand()`.
- Self-build relaunch guard exists through `m_forceRelaunchAfterBuild`.
- Rate-limit retry countdown exists in `MainWindow.cpp`.
- Mock/UI evidence files exist under `code/windows/w1/tools/`.

## Next Council-Relevant Checks

1. Verify the running app is on the intended Release build, not only
   `ReleaseVerify`.
2. Prove one DB-first manager loop end to end.
3. Review dirty worktree contents and decide which generated diagnostics should
   be ignored, archived, or deleted.
4. Reopen Council only if product direction, provider authority, persistence,
   backend integration, crash risk, or user-visible workflow behavior changes.
