# q1 Council Invocation

Automated council calls must avoid heavy Claude Code project sessions.

## Claude

Use Claude only for short review opinions, not repo exploration, unless explicitly needed.

Default automated invocation:

```bash
claude --model haiku --tools '' --no-session-persistence --max-budget-usd 0.05 -p '<short review prompt>'
```

Rules:

- run from `/tmp` when no repository file access is needed
- pass concise summaries instead of full diffs
- set a timeout in the caller and terminate hung processes
- do not use `--bare` with the current OAuth/keychain login; it requires `ANTHROPIC_API_KEY`
- do not use the default model for routine council checks; it can select an expensive/high-latency model

Observed on 2026-05-05:

- default `claude -p 'OK'` took about 5-8 seconds
- default calls created about 20k-28k system/cache tokens
- `--bare` failed with `Not logged in` because it bypasses keychain/OAuth auth
- `--model haiku --tools '' --no-session-persistence` reduced cost but still has about 5-6 seconds process/API overhead

## Gemini

Default automated invocation:

```bash
GEMINI_CLI_TRUST_WORKSPACE=true gemini -p '<short review prompt>'
```

Rules:

- use `GEMINI_CLI_TRUST_WORKSPACE=true` for headless trusted-directory execution
- terminate hung processes instead of blocking q1 autopilot

## Codex

Codex owns final integration, validation, rollback, server/port cleanup, and final reporting.
