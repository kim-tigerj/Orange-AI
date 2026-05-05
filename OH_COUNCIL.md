# Oh-Council: 오감독 3자 협업체계

English alias: Oh Supervisor 3-Way Collaboration System.

Use `Oh-Council` as the stable CLI/document identifier when Korean text may be
misdecoded by a terminal.

## Purpose

Oh-Council, also called **오감독 3자 협업체계** or **Oh Supervisor 3-Way
Collaboration System**, is the project protocol for using Claude Code, Gemini,
and Codex together.

The goal is not simple parallel work. The goal is better decisions, better work
direction, and stronger verification by making the three systems expose and
check each other's blind spots.

## Participants

| Participant | Primary Strength | Default Role |
| --- | --- | --- |
| Claude Code | Codebase implementation, large edits, existing-flow continuity | Implementer |
| Gemini | Alternative plans, product/UX judgment, broad risk discovery | Strategy advisor |
| Codex | Integration, build/test verification, code review, documentation | Quality gate and synthesizer |

These are defaults, not permanent assignments. At the start of important work,
all three must be invited into the planning discussion before execution roles
are fixed.

## Core Rule

```text
논의는 3자 합의.
실행은 단일 책임.
검증은 3자 순환.
결정은 문서에 기록.
```

Short form:

```text
회의는 다 같이. 실행은 한 명이. 검증은 돌아가며. 결정은 기록한다.
```

## Phases

1. Council Phase
   - Claude Code, Gemini, and Codex all read the same current context.
   - No one edits files during this phase.
   - Each participant gives goal interpretation, plan, risks, and role proposal.

2. Plan Phase
   - A single operator synthesizes the three opinions into one plan.
   - Record the plan in the active task, handoff, or a dedicated plan file.
   - Include alternatives considered and why the final direction was selected.

3. Execution Phase
   - One owner performs the implementation.
   - File ownership must be explicit.
   - Other participants do not edit the same files in parallel unless the plan
     explicitly splits write ownership.

4. Tri-Review Phase
   - All three participants review the result from different angles.
   - Claude Code checks implementation fit and code-flow continuity.
   - Gemini checks goal fit, product direction, UX, and better alternatives.
   - Codex checks bugs, regressions, build/test evidence, scope, and docs.

5. Decision Phase
   - Merge review results into one verdict:
     approve, approve-with-notes, request-changes, or reopen-council.
   - Record the decision and reasoning in durable documents.

## Review Verdict Rules

| Review Result | Action |
| --- | --- |
| All approve | Mark complete and record evidence. |
| One approve-with-notes | Complete is allowed; record notes as follow-up. |
| One request-changes | Send back to execution owner, then re-review the changed area. |
| Two or more request-changes | Return to Council Phase. |
| Any reopen-council | Return to Council Phase if the issue affects goal, design, or direction. |

## Required Context For A Council Call

Each participant should receive:

- user goal or problem statement
- relevant excerpt from `MANAGER_HANDOFF.md`
- relevant task or issue text
- known constraints, owned files, and verification expectations
- explicit request to avoid editing during Council Phase

## Recommended Prompt Shape

```text
You are participating in Oh-Council, the 오감독 3자 협업체계.
Do not edit files in this phase.

Goal:
<goal>

Current context:
<handoff/task summary>

Give:
1. Your interpretation of the goal
2. Best approach
3. Main risks
4. Suggested execution owner and reviewer focus
5. Stop conditions that should reopen council
```

## Current CLI Status

As of 2026-05-05, OrangeCode can call all three backends:

- `claude.cmd --version` -> `2.1.123 (Claude Code)`
- `gemini.cmd --version` -> `0.40.1`
- `codex.cmd --version` -> `codex-cli 0.128.0`

Verified OrangeCode smoke calls:

```powershell
cd O:\Work\OrangeLabs\Orange\Orange-AI\code\windows\w1
.\bin\Release\OrangeCode.exe --test-backend claude --test-prompt "Reply exactly: CLAUDE_OK"
.\bin\Release\OrangeCode.exe --test-backend gemini --test-prompt "Reply exactly: GEMINI_OK"
.\bin\Release\OrangeCode.exe --test-backend codex --test-prompt "Reply exactly: CODEX_OK"
.\bin\Release\OrangeCode.exe --test-backend all --test-prompt "3-party call smoke test. Reply with your provider name followed by _COUNCIL_OK."
```

The `all` test produced:

- `Codex_COUNCIL_OK`
- `Claude_COUNCIL_OK`
- `Gemini_COUNCIL_OK`

## Durable Records

Use these files so other CLI sessions can recover the collaboration state:

- `OH_COUNCIL.md`: this protocol
- `MANAGER_HANDOFF.md`: current project state and recent verification
- `doc/DECISIONS.md`: final decisions and reasoning
- task specs or reports under `code/windows/w1/tasks/` and `code/windows/w1/reports/`

When Oh-Council changes direction, update `MANAGER_HANDOFF.md` and, if the choice
is architectural or long-lived, update `doc/DECISIONS.md`.
