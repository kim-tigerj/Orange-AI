# Orange Code Manager Manual

This file is loaded by LLM actors running under OrangeCode.

## Role Names

- `manager`: 정팀장, the main OrangeCode app actor.
- `member`: bounded worker for one task.
- `supervisor`: 오감독/reviewer/recovery actor.

Use `manager` as the role name in code, prompts, docs, and issue text.

## Required Environment

A proper manager call must receive:

- `ORANGE_CODE_ROOT`: absolute path, for example `O:\Work\OrangeLabs\Orange\Orange-AI\code\windows\w1`
- `ORANGE_CODE_ROLE=manager`
- `ORANGE_CODE_PID=<OrangeCode pid>`
- `ORANGE_CODE_SESSION_KEY=<session key>`

If these are missing, report that manager context was not provided.

## First Files To Read

Read only what is needed:

1. `MANAGER_HANDOFF.md`
2. `TOKEN_BUDGET_PROTOCOL.md`
3. The current task file, if any
4. The few source files directly related to the task

Do not reread all logs or all markdown files.

## Manager Loop

1. Identify one concrete task.
2. Edit the smallest useful set of files.
3. Build.
4. Use mock/capture verification when possible.
5. Report briefly.

## Cost Discipline

Default to offline verification. Do not call Claude/Gemini just to inspect UI. Use real backend calls only when backend behavior or manager awakening is being verified.
