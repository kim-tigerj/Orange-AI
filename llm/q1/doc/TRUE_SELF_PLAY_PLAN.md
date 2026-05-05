# q1 True Self-Play Plan

## Current Finding

`proposal-autopilot` currently does not represent true q1 self-play.

The present flow is mostly deterministic:

1. `proposal_autopilot()` calls `self_play(1)`.
2. `self_play()` selects a known seed target from `DEFAULT_SEED_FUNCTION_JOBS` or `next_manual_job_command()`.
3. It creates a proposal JSON with a fixed `read_function` tool call.
4. Every tenth proposal, it creates a safe write proposal for `llm/q1/dataset/lora/WRITE_SAMPLES.md`.
5. `gate_proposal()`, `apply_proposal()`, and `export_lora_dataset()` run locally.

This means proposal files and LoRA samples can grow without q1 server inference. The generated dataset is therefore mostly supervisor-generated operational data, not q1 model behavior data.

## Why This Matters

The goal of LoRA tuning is to reinforce q1's useful behavior:

- problem definition
- investigation choice
- safe tool-call proposal
- write/no-write judgment
- rejection boundary awareness

If samples are produced deterministically by Python code, they do not capture q1's actual reasoning habits. Training on them may teach formatting and policy shape, but it will not strongly improve autonomous code-improvement behavior.

## Current Dataset Shape

As of 2026-05-05:

- samples: 41
- accepted_read_only: 36
- positive: 4
- rejected: 1

The dataset is heavily skewed toward read-only proposals. Positive write samples and rejected boundary samples are too sparse for useful tuning.

## Target Architecture

True self-play should use the q1 model server to generate proposals.

```text
seed context
  -> q1 /generate or /generate_stream
  -> proposal JSON or single tool_call
  -> Oh supervisor gate
  -> apply / accept / reject
  -> validation
  -> LoRA sample export
```

The supervisor remains responsible for safety. q1 is responsible for proposing.

## Required Design Change

Add a model-backed proposal generation path:

- `self_play_mode = deterministic | model | mixed`
- default should become `mixed` after the model path is stable
- deterministic mode remains as fallback and seed source
- model mode must call `generate_response()`, which routes to the q1 server in normal backend mode

## MVP

1. Add `create_model_proposal()`.
2. Select a target and goal from the existing seed candidate system.
3. Build a strict proposal-generation prompt.
4. Call `generate_response(messages, max_tokens=...)`.
5. Parse the result into either:
   - `DONE_NO_CHANGE`
   - one allowed read tool call
   - one allowed write tool call
6. Wrap the result in the existing proposal schema.
7. Store malformed model output as a rejected proposal sample rather than discarding it.

## Prompt Contract

The model-backed proposal prompt should require:

- no direct file mutation
- exactly one proposal
- either `DONE_NO_CHANGE` or one tool call
- allowed tools only
- explicit target/problem/rationale/risk
- no multiple write tools
- no server process or port manipulation

## Safety Gate

Keep the current supervisor gate:

- schema validation
- tool-call parsing
- allowed path check
- dangerous pattern rejection
- exactly one write tool maximum
- validation after apply
- rollback on validation failure

Rejected model outputs should be preserved as negative data when they are useful boundary examples.

## Data Collection Targets

Before LoRA training, aim for a more balanced dataset:

- accepted_read_only: useful investigation examples only
- positive: at least 15-25% of samples
- rejected: at least 10-20% of samples

Positive samples must include successful validation. Rejected samples must include the concrete gate or validation failure reason.

## Implementation Plan

1. Document the current deterministic limitation.
2. Add CLI flags:
   - `--self-play-mode {deterministic,model,mixed}`
   - `--model-self-play-ratio`
3. Implement model-backed proposal creation.
4. Add rejected-sample capture for malformed or unsafe model output.
5. Update `proposal_autopilot()` to use the configured mode.
6. Export dataset metadata with `source=model-self-play` vs `source=deterministic`.
7. Add tests for:
   - deterministic fallback
   - malformed model proposal rejection
   - valid read-only model proposal
   - valid safe write model proposal

## Operational Rule

Do not treat deterministic proposal-autopilot growth as evidence of q1 self-improvement. True self-play progress must be measured by q1 server generate count, accepted model proposals, positive write samples, rejected boundary samples, and validation success rate.
