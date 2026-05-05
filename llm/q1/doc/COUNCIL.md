# q1 Council Notes

## 2026-05-05 Round 1

### Topic

What should the next q1 improvement round focus on so q1 becomes substantially more reliable, not just busier?

### Current State

- `./q1 --validate`: PASS
- q1 server: OK on `127.0.0.1:8765`
- queue: `todo_count=0`, `failed_count=0`, `seed_candidate_count=0`
- recent no-change model calls: about 5-10 seconds
- exact repeated no-change skip: about 0.18 seconds
- malformed/free-text model outputs are now failure reports instead of silent success
- reports include `model_elapsed_ms`

### Claude View

Top concern: q1 can look stable because it parses model output and has an empty queue, not because applied changes are behaviorally correct.

Ranked themes:

1. Outcome verification after applying a change.
2. Determinism and idempotency beyond exact source hashes.
3. Better observability for why a round did nothing.
4. Candidate/seed starvation visibility.
5. Regression guard around `replace_function`.

Suggested first experiment: add post-apply verification on a fixture function, fail and roll back when tests fail.

### Gemini View

Top concern: function-level improvement can break boundaries while still producing syntactically valid Python.

Ranked themes:

1. Context-enriched function prompts.
2. AST-level signature preservation.
3. Closed-loop failure recovery with traceback context.
4. Static blocking of unsafe side effects.
5. Shadow execution or focused runtime checks.

Suggested first experiment: reject `replace_function` output when the new function changes the original function name/signature/decorators.

### Codex Synthesis

The common issue is that q1's definition of success is still too shallow. The smallest high-signal next step is to harden `replace_function` before writing to disk:

- parse the replacement snippet as exactly one Python function
- require the function name to match the requested target
- preserve async/sync kind, arguments, return annotation, and decorators
- keep focused self-tests for adversarial model outputs

This does not replace post-apply behavioral tests. It gives q1 a stronger boundary check first, then the next round can add fixture-level post-apply verification.

### Decision

Round 1 implementation target: AST boundary preservation in `replace_function`, with tool self-test cases for renamed functions and missing decorators.

## 2026-05-05 Round 2

### Topic

How should q1 reject syntactically valid and boundary-preserving changes when they break behavior?

### Claude View

Recommended direction:

1. Add a per-target verification command manifest later.
2. Always run cheap import/compile verification where possible.
3. On verifier failure, restore the pre-change file bytes and report the failure.

Claude's strongest implementation detail was atomicity: q1 should capture a pre-image before applying model output, and failed verification must leave the repo byte-equivalent to the pre-change state.

### Gemini View

Recommended direction:

1. Reuse existing q1 validation/self-test surface first.
2. Avoid asking the model to invent verification commands.
3. Roll back after a timeout or non-zero verification result.

Gemini suggested putting this directly inside `replace_function`, but that would make `--tool-self-test` recursively call itself because the self-test uses `replace_function`.

### Codex Synthesis

q1 already runs `run_validation_suite` after `improve_once` and function-level `improve_function` calls. The missing invariant is rollback: validation failure currently creates a failure report but can leave the edited file in place.

Round 2 implementation target:

- capture byte-level file contents before model/tool execution
- after validation or runtime failure, restore changed files from the pre-image
- record `rolled_back_files` in the improve report
- add a no-model self-test for snapshot/restore mechanics

Future round:

- add targeted verification manifest support once q1 has stable rollback semantics
- consider import-only checks for changed modules, but keep them outside `replace_function` to avoid recursive self-test behavior

## 2026-05-05 Round 3

### Topic

What is the smallest useful verifier to add once rollback exists?

### Claude View

Claude recommended a post-write AST-equivalence verifier. The main point was that Round 1 checks the replacement snippet before writing, but a future writer bug or merge issue could still leave a bad function on disk. Claude ranked import-time smoke below AST verification because import checks catch module initialization failures but not function-level boundary drift.

### Gemini View

Gemini recommended a static control-flow preservation check:

- if the original function has a `return`, the replacement must keep a `return`
- if the original function has a `yield`, the replacement must keep a `yield`

Gemini explicitly scoped this as a cheap guard against LLM truncation, not a full semantic verifier.

### Codex Synthesis

Round 3 adds two small verifier layers:

1. Import-time smoke checks for changed q1 Python modules inside `run_validation_suite`, outside `replace_function`, so validation failure can use the Round 2 rollback path without recursive self-test behavior.
2. Static `return`/`yield` preservation inside the existing AST boundary check, because it is cheap and catches common LLM body truncation.

This still does not prove semantic correctness. It catches more broken states before q1 calls a change successful.

### Decision

Round 3 implementation target:

- add `verify_changed_python_imports` to validation
- add self-tests for passing and failing import checks
- reject replacements that drop existing `return` or `yield` flow
- add self-tests for return/yield drop rejection

## 2026-05-05 Round 4

### Topic

Should q1 add a post-splice/post-write target boundary verifier?

### Claude View

Claude recommended doing it now because Round 2 rollback is already available and the cost is only another AST parse. The key risk is writer/splice bugs: the snippet can be valid before writing, but the resulting file can still have the target function in the wrong shape.

### Gemini View

Gemini also recommended doing it now, with an additional topology check so a splice cannot swallow or create adjacent functions while still leaving syntactically valid Python.

### Codex Synthesis

Round 4 adds two related guards:

1. `improve_function` verifies the target function boundary after the model/tool call by reading the function back from disk. If it fails, the Round 2 rollback path restores the pre-image.
2. `replace_function` verifies the full post-splice module AST before writing:
   - target function still exists
   - target boundary still matches
   - function topology is unchanged

This keeps `replace_function` strict: it replaces one function body and must not add/drop neighboring or nested functions.

### Decision

Round 4 implementation target:

- add `verify_target_boundary_after_write`
- call it from `improve_function` before global validation
- add post-splice AST topology verification inside `replace_function`
- add self-tests for post-write boundary drift and topology changes

## 2026-05-05 Round 5

### Topic

Improve q1 observability around candidate/seed selection so idle status is distinguishable from broken discovery or over-filtering.

### Claude View

Claude identified the main gap as a collapsed funnel: q1 exposed only the final `seed_candidate_count`, so healthy idle, broken discovery, and over-filtering could look the same. Claude suggested a fuller `DiscoveryReport` with state, source counts, rejection reasons, samples, and freshness.

### Gemini View

Gemini did not return a result in a reasonable time for this round. The hung CLI process was terminated and the delay is treated as a council participant timeout, not a blocker.

### Codex Synthesis

The smallest useful version is a shared seed candidate summary helper rather than a new report type. q1 already discovers targets and filters them through three practical gates:

- already present in todo
- failed target
- recent `DONE` or `NO_CHANGE`

Round 5 exposes those gates consistently in `--status`, `--queue-health`, and `latest_summary`.

### Decision

Round 5 implementation target:

- add `seed_candidate_summary`
- exclude existing todo targets from status candidate counts
- print seed rejection counts in status and queue health
- include seed rejection counts and sample candidates in latest summary

## 2026-05-05 Round 6

### Topic

Should `--report-latest` regenerate `latest_summary.md` before printing?

### Claude View

Claude recommended regenerating before printing. The summary is a derived artifact, regeneration is cheap and model-free, and stale output from a command named `--report-latest` is more surprising than rewriting the derived file.

### Gemini View

Gemini did not return a result in a reasonable time for this round. The hung CLI process was terminated and the delay is treated as a council participant timeout.

### Codex Synthesis

Round 5 added new seed funnel fields to summary generation, but `--report-latest` only printed the existing file. That made the command show stale `seed_candidate_count` and omit the new rejection fields until another workflow happened to regenerate the summary.

### Decision

Round 6 implementation target:

- call `write_latest_summary()` at the start of `print_latest_report`
- verify `--report-latest` output contains fresh seed funnel fields

## 2026-05-05 Round 7

### Topic

What should q1 do when discovered seed candidates are exhausted?

### Claude View

Claude recommended not expanding discovery first. With all 106 discovered targets recently `DONE` or `NO_CHANGE`, the system is signal-starved rather than work-starved. Claude's ranking was:

1. Make `next_action` concrete instead of returning `--status`.
2. Build a small regression/evaluation corpus.
3. Revisit old `NO_CHANGE` targets only after there is an evaluation signal.
4. Defer discovery expansion until q1 can measure useful deltas.

### Gemini View

Gemini did not return a result in a reasonable time for this round. The hung CLI process was terminated and the delay is treated as a council participant timeout.

### Codex Synthesis

When fallback jobs and discovered candidates are exhausted, `--status` is an accurate state check but a weak next recommendation. q1 needs a model-free next step that improves future signal. The smallest useful step is to create a corpus document from existing guards and validation commands.

### Decision

Round 7 implementation target:

- add `--build-corpus`
- generate `llm/q1/doc/EVALUATION_CORPUS.md`
- make exhausted `next_manual_job_command` return `--build-corpus` instead of `--status`

## 2026-05-05 Round 8

### Topic

How should q1 avoid recommending `--build-corpus` forever after the corpus exists?

### Claude View

Claude recommended stateful freshness: once the corpus is fresh and seeds are exhausted, q1 should not fabricate work. It should report an honest idle state, preferably with a reason.

### Gemini View

Gemini did not return a result in a reasonable time for this round. The hung CLI process was terminated and the delay is treated as a council participant timeout.

### Codex Synthesis

A strict mtime freshness rule is fragile here because running q1 against the new `build_corpus` function creates a newer `NO_CHANGE` report after the corpus file is written. That can make a fresh-enough corpus look stale forever. The smallest reliable rule is:

- if the corpus is missing, recommend `--build-corpus`
- if the corpus exists and candidates are exhausted, return the honest idle command `--status`

### Decision

Round 8 implementation target:

- make `next_manual_job_command` recommend `--build-corpus` only when `EVALUATION_CORPUS.md` is missing
- otherwise return `--status` for the fully exhausted state

## 2026-05-05 Round 9

### Topic

How should q1 move from a safe self-modifier to a genuinely useful self-improver?

### Claude View

Claude argued that q1 is currently signal-starved. The proposed sequence was deterministic seed generation, then A/B scorecards, then acceptance gates based on measurable improvement.

### Gemini View

Gemini argued that the first step should be model-free fixture tests, so q1 has objective behavior checks before it expands candidate generation.

### Codex Synthesis

The practical order is:

1. Add model-free fixture tests to `--validate`.
2. Add deterministic static seeding so q1 can create work without loosening safety rules.
3. Add scorecards and acceptance gates after there is enough deterministic signal.

### Decision

Round 9 implementation target:

- add `llm/q1/tests` fixture tests for core parsing/path behaviors
- include unittest discovery in `run_validation_suite`
- add `--seed-static-jobs` for deterministic AST-based candidates

## 2026-05-05 Round 10

### Topic

Make council participation and work ownership visible in the q1 CLI.

### Decision

Round 10 implementation target:

- add `--council-status`
- print recent council rounds, participant status, Codex ownership, and decisions

## 2026-05-05 Round 11

### Topic

Derive and implement the first q1 server reliability improvements.

### Claude View

Claude prioritized request/response observability: structured request IDs, ready/health separation, single-flight generation, graceful shutdown, server-side timeouts, metrics, and startup config banners.

### Gemini View

Gemini prioritized operational reliability during long generations: `ThreadingHTTPServer`, generation speed logging, progress heartbeat, strict payload validation, graceful shutdown, tunable generation parameters, and enriched health output.

### Codex Synthesis

The first safe server patch should be additive and preserve the existing CLI JSON contract. The lowest-risk combined slice is:

- threaded HTTP server so `/health` can respond during long generation
- per-request IDs in logs and `X-Q1-Request-Id`
- enriched `/health` with uptime and request count
- 400 responses for malformed generation payloads
- generation progress and chunks-per-second logs

### Decision

Round 11 implementation target:

- update `q1_server.py` with the additive reliability/observability patch
- run q1 validation without restarting the currently running server

## 2026-05-05 Round 12

### Topic

Make q1 server restart automatic so the operator does not manually kill and relaunch the model server.

### Decision

Round 12 implementation target:

- add `--restart-server`
- find and stop existing local `q1_server.py` listener on the configured port
- launch the server detached with the project venv Python
- wait for `/health`
- print the new PID, model, uptime, and `X-Q1-Request-Id`

## 2026-05-05 Round 13

### Topic

Automatically restart the model server when q1 changes `q1_server.py`.

### Decision

Round 13 implementation target:

- after validation succeeds, detect `llm/q1/q1_server.py` in `changed_files`
- run the existing restart flow automatically
- record `server_restarted: true` in the improve report
- if restart fails, convert the run to a failure report so rollback can restore the server file

## 2026-05-05 Round 14

### Topic

Fix direct q1 server execution when port 8765 is already occupied by an existing q1 server.

### Decision

Round 14 implementation target:

- on `EADDRINUSE`, retry binding instead of exiting immediately
- if `/health` confirms the listener is q1, terminate the existing listener and retry
- tolerate transient port/reset states during restart
- verify both direct `q1_server.py` execution and detached `--restart-server`

## 2026-05-05 Round 15

### Topic

Improve perceived q1 response speed without truncating or changing responses.

### Claude View

Claude CLI did not return a review response in this round and was terminated to avoid blocking the automated workflow.

### Gemini View

Gemini agreed with adding NDJSON streaming while preserving the existing `/generate` contract. The main review finding was missing HTTP-level coverage for the streaming protocol.

### Codex Synthesis

Do not reduce `max_tokens`. Preserve final response text and existing tool parsing. Add `/generate_stream` as an additive endpoint, stream chunks to the CLI immediately, keep the accumulated full response for `parse_tool_calls` and logging, retain `/generate` compatibility, and add HTTP-level NDJSON tests.

### Decision

Round 15 implementation target:

- add `/generate_stream` with `accepted`, `start`, `chunk`, `done`, and `error` NDJSON events
- keep single MLX worker queue and existing cache behavior
- make q1 CLI consume stream chunks immediately while returning the complete text unchanged
- fall back to `/generate` if a server does not support `/generate_stream`
- add fixture coverage for streaming job events and cached NDJSON HTTP output

## 2026-05-05 Round 16

### Topic

Split q1 self-play work into proposal generation and supervised application so LoRA training data can accumulate safely.

### Claude View

Claude proposed a propose-first architecture: one self-play round should write no code, produce one proposal JSON, and defer all file application to an Oh supervisor gate. The recommended gate checks include schema validation, target anchor/path constraints, AST safety, dangerous pattern rejection, validation commands, reviewer verdict, and final approval policy.

### Gemini View

Gemini proposed a chat-style JSONL dataset with labels such as `positive`, `no_change`, `repair`, and `rejected`, plus dedupe keys, quality scores, validation metadata, and a dataset report.

### Codex Synthesis

Implement the first safe slice: proposal directories, deterministic self-play dry-run, proposal listing/showing, and LoRA dataset export. Do not implement automatic file application in this round.

### Decision

Round 16 implementation target:

- add `llm/q1/proposal/{pending,accepted,rejected}`
- add `--self-play`, `--list-proposals`, `--show-proposal`, and `--export-lora-dataset`
- ensure self-play writes proposal JSON only, not code changes
- add `llm/q1/doc/PROPOSAL_SCHEMA.md`
- export proposal samples to `llm/q1/dataset/lora/q1_selfplay.jsonl`

## 2026-05-05 Round 17

### Topic

Complete the proposal workflow so the operator is not left with half-finished manual steps.

### Decision

Round 17 implementation target:

- add `--gate-proposal`, `--apply-proposal`, and `--reject-proposal`
- validate proposal schema, parse tool calls, enforce allowed paths, and run static rejection checks
- apply write proposals only after gate pass
- run `./q1 --validate` after write application
- rollback changed files on validation/apply failure and move failed proposals to rejected
- accept DONE_NO_CHANGE/read-only proposals without file writes

## 2026-05-05 Round 18

### Topic

Avoid handing the operator a sequence of manual proposal commands.

### Decision

Round 18 implementation target:

- add `--proposal-autopilot`
- run self-play proposal generation, gate/apply handling, and LoRA dataset export in one command
- keep Oh supervisor rollback/validation behavior inside the command
- leave q1 server port free after verification

## 2026-05-05 Round 19

### Topic

Fix Claude Code council invocation so automated reviews do not hang or use heavyweight sessions by accident.

### Decision

Round 19 findings:

- default `claude -p` works but adds several seconds of process/API overhead
- default Claude Code calls can create about 20k+ system/cache tokens even for tiny prompts
- `--bare` is not usable with the current login because it bypasses keychain/OAuth and expects `ANTHROPIC_API_KEY`
- routine council review should use `claude --model haiku --tools '' --no-session-persistence --max-budget-usd 0.05 -p ...`
- calls that do not need repo access should run from `/tmp`
- hung Claude/Gemini processes must be terminated and treated as participant timeouts

Implementation target:

- add `llm/q1/doc/COUNCIL_INVOCATION.md`
- use short summaries for Claude/Gemini review prompts instead of full repo-context prompts
