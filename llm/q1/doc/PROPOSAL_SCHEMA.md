# q1 Proposal Schema

q1 self-play must produce reviewable proposals before any file write.

## Core Rule

- q1 planner/worker/reviewer may create proposal and dataset files.
- q1 self-play must not directly apply code changes.
- Oh supervisor applies changes only through a separate gate.

## Proposal File

Location:

```text
llm/q1/proposal/pending/<proposal_id>.json
llm/q1/proposal/accepted/<proposal_id>.json
llm/q1/proposal/rejected/<proposal_id>.json
```

Required fields:

```json
{
  "id": "string",
  "schema_version": 1,
  "created_at": "YYYY-MM-DD HH:MM:SS",
  "status": "pending",
  "source": "self-play",
  "role": "planner",
  "target": "llm/q1/q1.py:function_name",
  "problem": "measurable problem statement",
  "rationale": "why this proposal exists",
  "proposed_tool_call": "DONE_NO_CHANGE or one read/write tool_call proposal",
  "expected_validation": "./q1 --validate",
  "risk": "low",
  "gate": {
    "requires_oh_supervisor": true,
    "apply_automatically": false
  }
}
```

## LoRA Export

`./q1 --export-lora-dataset` converts proposals into chat-style JSONL:

```json
{
  "messages": [
    {"role": "system", "content": "..."},
    {"role": "user", "content": "..."},
    {"role": "assistant", "content": "..."}
  ],
  "label": "proposal_pending",
  "metadata": {
    "dedupe_key": "...",
    "quality_score": 0.6,
    "source_proposal": "...",
    "validation": "./q1 --validate"
  }
}
```

## Future Gate

The apply gate must reject proposals that:

- write outside allowed q1 paths
- contain multiple write tools
- miss required target/path/function fields
- change code without passing `./q1 --validate`
- violate server/port ownership policy
- introduce destructive filesystem, subprocess, eval/exec, or network behavior without explicit approval

## Gate Commands

```bash
./q1 --gate-proposal <proposal_id>
./q1 --apply-proposal <proposal_id>
./q1 --reject-proposal <proposal_id> --reject-reason "..."
./q1 --proposal-autopilot --cycles <n>
```

`--apply-proposal` is the Oh supervisor gate. It may move proposals to:

- `accepted`: gate passed and either no writes were needed or validation passed after apply
- `rejected`: gate failed, apply failed, or validation failed after rollback

For write proposals, validation failure must restore the pre-apply file bytes before rejection.

`--proposal-autopilot` runs the complete operator loop:

1. create self-play proposal
2. gate pending proposals
3. accept/apply/rollback/reject through Oh supervisor logic
4. export the LoRA dataset
