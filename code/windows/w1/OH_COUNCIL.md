# Oh-Council Protocol

Oh-Council is the OrangeCode three-provider collaboration system.

## Roles

- `manager`: the appointed execution owner for the current chat.
- `member`: a bounded worker for one assigned task.
- `supervisor`: a reviewer, recovery actor, or external review voice.

Use `manager` as the role name in code, prompts, docs, and issue text.

## Authority

1. User instruction.
2. Appointed manager execution.
3. Reviewer advice.

When the user asks to fix, edit, implement, add, build, verify, or update rules, the appointed `manager` stops discussion and executes after checking only relevant evidence.

## Collaboration Loop

1. The `manager` identifies one concrete goal.
2. Reviewers provide bounded advice only when asked or when delegation is explicit.
3. One owner edits files.
4. The owner verifies with build, mock test, capture, or a targeted backend call.
5. The owner records reusable conclusions in durable files.
6. When a repeatable operating method emerges during any step, the `manager` evaluates it as a rule candidate, records the concise rule in the relevant durable file, and applies it from that point forward.

## Durable Conclusions

Conclusions reached through council discussion are not just chat text. The `manager` records reusable conclusions in:

- `MANAGER_HANDOFF.md` for current state and next action.
- `TOKEN_BUDGET_PROTOCOL.md` for standing execution-loop rules.
- `OH_COUNCIL.md` for three-provider collaboration protocol.

Rules should stay scoped to the evidence that produced them. Do not turn a single incident into broad backlog work unless the user explicitly asks for that expansion.
