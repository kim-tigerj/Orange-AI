# OR-1305 Orange Code Strategy

Orange Code exists to move repeated coding work from human time into LLM-managed time.

## Product Thesis

Claude Code is useful, but its session model is not enough for long-running, inspectable, multi-actor development. Orange Code turns the workflow itself into the product:

- persistent tasks,
- visible progress,
- manager/member/supervisor roles,
- build and capture verification,
- native lightweight UI,
- controlled token and quota usage.

## Role Model

- `manager`: 정팀장, the main in-app actor.
- `member`: bounded worker for one task.
- `supervisor`: 오감독/reviewer/recovery actor.

`manager` is the required role name for the Jeong Team Lead app.

## Near-Term Priorities

1. Manager awakening
   - backend child processes must receive `ORANGE_CODE_ROOT`, `ORANGE_CODE_ROLE=manager`, `ORANGE_CODE_PID`, and `ORANGE_CODE_SESSION_KEY`.

2. Reliable coding loop
   - user request,
   - manager response,
   - file edit,
   - build,
   - capture or test,
   - concise report.

3. Cost control
   - default to mock/offline tests,
   - avoid repeated external LLM calls,
   - keep durable context in files.

4. Coding-oriented UI
   - chat is acceptable, but code blocks, logs, diffs, and work summaries must be the primary optimized surface.

## Related Issues

- OR-1305: core Orange Code vision
- OR-1317: work hierarchy and progress dashboard
- OR-1318: coordination mechanism
- OR-1325: Gemini backend
- OR-1326: backend selection
- OR-1328: SQLite coordination system
