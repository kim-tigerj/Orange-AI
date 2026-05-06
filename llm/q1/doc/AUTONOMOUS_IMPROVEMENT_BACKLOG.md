# q1 Autonomous Improvement Backlog

Generated: 2026-05-06

This backlog defines what Codex should keep proposing while q1 runs model-backed self-play overnight. The purpose is not to let q1 blindly edit itself. The purpose is to turn each observed model behavior into safer gates, better prompts, better datasets, and measurable progress toward a usable local coding agent.

## Current Goal

Make q1 useful as a local coding agent by improving these loops:

1. Model-backed proposal generation uses the q1 server, not deterministic templates.
2. Supervisor gate catches bad proposals before file writes.
3. Accepted and rejected proposals become LoRA-ready behavior data.
4. Runtime metrics show whether q1 is getting faster, safer, and more useful.

## Observed Facts

- q1 server is running on `127.0.0.1:8765`.
- Real model-backed self-play works and increments server generate count.
- q1 often outputs bare JSON instead of wrapped Hermes `<tool_call>`.
- q1 sometimes emits incomplete long function replacements.
- q1 sometimes proposes structurally invalid `replace_function` content.
- Gate validation now catches `replace_function` content that contains more than one function.
- Dataset balance is improving, but rejected samples are rising quickly.

## Next Proposal Themes

### P0: Preserve Safety While Loop Runs

- Do not let model proposals modify server process ownership, ports, or launch scripts without explicit gate checks.
- Gate `replace_function` by validating target function shape before apply.
- Reject incomplete streamed JSON or truncated tool calls as negative samples.
- Keep every failed proposal as rejected data, with reason preserved.

### P1: Improve Model Output Format

- Add stronger examples to `SELF_PLAY_SYSTEM_PROMPT`.
- Prefer shorter function replacements by asking q1 to use `replace` for small local edits.
- Add explicit anti-patterns:
  - no extra helper functions in `replace_function`
  - no placeholder functions such as `pass`
  - no comments claiming unimplemented optimization
  - no English operational text unless source already uses it
- Track malformed reasons separately in dataset metadata.

### P2: Improve Gate Quality

- Add gate result labels:
  - `malformed_tool_call`
  - `truncated_output`
  - `replace_function_shape_error`
  - `unsafe_pattern`
  - `validation_failure`
- Add dry-run apply check for `replace` and `write_file`, not only `replace_function`.
- Add gate check that `replace_function` content does not add undefined method calls.
- Add gate check that generated content does not introduce TODO/pass placeholder behavior.

### P3: Improve Dataset Utility

- Split dataset report by `source`:
  - deterministic
  - self-play-model-v1
  - proposal-autopilot-safe-write
- Split rejected samples by rejection reason.
- Prefer high-quality positives:
  - model generated
  - gate passed
  - validation passed
  - actual changed files
- Keep malformed outputs, but label them as format correction data, not behavior success data.

### P4: Improve Throughput

- Record per-proposal prompt size and output size.
- Add max-token tuning experiments:
  - 512 for read-only proposals
  - 768 for small function edits
  - 1024 only for larger replacements
- Add output truncation detection to avoid wasting apply time.
- Measure average generate time per target function.

## Immediate Next Slices

1. Add dataset metadata for model output parse reason.
2. Add gate rejection reason export to LoRA report.
3. Add prompt examples showing accepted bare JSON and canonical Hermes output.
4. Add truncation detection for outputs ending mid-token or mid-JSON.
5. Add nightly summary parser that reads the loop log and reports:
   - iterations
   - generate count
   - average generate time
   - accepted/rejected deltas
   - top rejection reasons

## Operating Rule

Codex should keep generating improvement proposals from actual loop observations. Each proposed code change must be small, validated, and compatible with the running q1 server. q1 may propose changes, but Codex/OH supervision decides what is applied.
