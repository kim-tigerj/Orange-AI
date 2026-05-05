# Orange Code Token Budget Protocol

Purpose: keep Orange Code development moving while protecting Codex, Claude, Gemini, and OpenAI quota.

## Operating Rules

1. Keep the body stable first.
   - Make one useful change per loop.
   - Read only the files needed for the current decision.
   - Avoid full-repo dumps and long log reads unless debugging requires them.

2. Use a three-step loop.
   - Target: define the one thing to improve now.
   - Execute: edit the smallest reasonable file set.
   - Verify: build, run a mock test, or capture the screen.

3. External LLM calls are last.
   - Use mock/offline tests for UI and layout work.
   - Call Claude, Gemini, or OpenAI only when backend integration or model behavior itself is under test.
   - If a quota error appears, do not retry the same provider in a loop.

4. Keep durable state in files.
   - Update `MANAGER_HANDOFF.md` with the current state, next step, and verification commands.
   - The next session should read only this handoff, this protocol, and the most relevant recent files.

5. Preferred verification order:
   - `tools/build.sh`
   - `--diagnose-manager-env`
   - `--test-backend mock` with `--test-response-file`
   - `--test-capture` or `--capture`
   - one real Claude/Gemini/OpenAI call only when needed

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
