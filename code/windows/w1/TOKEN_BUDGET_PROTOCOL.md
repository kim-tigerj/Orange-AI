# Orange Code Token Budget Protocol

Purpose: keep Orange Code moving through fast response and useful Claude/Gemini/Codex collaboration, while avoiding waste when cost savings do not slow the work.

## Operating Rules

1. Keep the body stable first.
   - Make one useful change per loop.
   - Read only the files needed for the current decision.
   - Avoid full-repo dumps and long log reads unless debugging requires them.

2. Use a three-step loop.
   - Target: define the one thing to improve now.
   - Execute: edit the smallest reasonable file set.
   - Verify: build, run a mock test, or capture the screen.
   - If the latest user message explicitly asks to fix, edit, implement, add, build, verify, or update rules, the appointed manager starts execution after checking only the relevant evidence. Do not turn that into another discussion round.
   - As the loop proceeds, if a repeatable operating method is derived, treat it as a rule candidate, record the concise rule in the relevant durable file, and apply it from that point forward.
   - Only promote a method to a rule when it is likely to help future manager loops; do not create broad policy work from a one-off incident.
   - Do not end an execution turn passively after identifying a required next action. If the handoff or current evidence says the next step is `orange.build`, `orange.capture`, or a bounded patch, perform that request or explain the exact blocker before closing.
   - Keep progress reports concise and evidence-based. Do not repeat generic future-action narration such as fallback build plans or "I will verify next"; report the actual change, actual verification, blocker, or requested OrangeCode tool action.

3. Use external LLM calls when they improve the loop.
   - Call Claude, Gemini, Codex, or OpenAI when they materially improve direction, implementation confidence, review, recovery, or backend behavior.
   - In multi-provider sessions, keep one appointed manager as the executor. Other providers are reviewers/advisors unless explicitly granted implementation tools.
   - If discussion repeats or no one is closing the issue, the appointed manager decides the working conclusion and reviewers follow that direction.
   - After selected providers have given opinions, the appointed manager must close with a decision and immediately take the next concrete action. Do not end with one-turn opinions only.
   - Provider-to-provider relay uses explicit OrangeCode commands only: `/relay <provider>: <message>`, `/ask <provider>: <message>`, `/전달 <provider>: <message>`, `/호출 <provider>: <message>`, or `@<provider> <message>`. Plain `Claude`/`Gemini`/`Codex` name mentions are discussion text and must not trigger relay by themselves. If the target does not respond within the relay timeout, the asking provider continues with best available judgment so the work does not stall.
   - Use mock/offline tests for mechanical UI and layout checks where another model would not add useful judgment.
   - If a quota error appears, switch strategy or provider instead of retrying the same failed call in a loop.

4. Keep durable state in files.
   - Update `MANAGER_HANDOFF.md` with the current state, next step, and verification commands.
   - When a multi-provider meeting reaches a reusable conclusion, the appointed manager records it as an internal instruction in the durable files the next session reads.
   - Prefer `MANAGER_HANDOFF.md` for current operating state and `TOKEN_BUDGET_PROTOCOL.md` for standing loop rules.
   - `OH_COUNCIL.md` is owner/supervisor policy, not default manager/member context.
   - Startup/restart prompts must treat the SQLite `prompt` table and `job`/`job_detail` briefing as the primary source. Markdown handoff files are supplemental evidence and must not auto-start backlog work by themselves.
   - The appointed manager provider is sourced only from OrangeCode app settings and `ORANGE_CODE_MANAGER_PROVIDER`. If markdown files, role docs, or old chat history name a different provider as manager, treat that as stale supplemental context.
   - If markdown files are needed for provider-to-provider sharing, use only OrangeCode-owned exchange files under `ORANGE_CODE_ROOT\reports\` named `council-<session-key>-<yyyymmddHHMMSS>-<provider>-<topic>.md`. Do not create ad hoc shared markdown files in the repo root or unrelated folders.
   - The next manager/member session should read only this handoff, this protocol, and the most relevant recent files.

5. Preferred verification order:
   - `tools/build.sh`
   - `--diagnose-manager-env`
   - `--test-backend mock` with `--test-response-file`
   - `--test-capture` or `--capture`
   - real Claude/Gemini/Codex/OpenAI calls when collaboration, review, recovery, or backend behavior needs them

## Mock UI Test

Use a response file for long or Korean text. PowerShell `Start-Process -ArgumentList` can split strings with spaces.

```powershell
OrangeCode.exe --test-backend mock `
  --test-prompt "manager smoke test" `
  --test-response-file tools/mock-code-response.md `
  --test-response-ms 300 `
  --test-capture tools/mock-capture.png `
  --test-capture-ms 2500 `
  --test-exit-ms 3600
```

## Current Default Loop

```text
read handoff
read 1-3 relevant files
make a small change
build
run mock/capture if useful
update handoff
report in 3-6 lines
```
