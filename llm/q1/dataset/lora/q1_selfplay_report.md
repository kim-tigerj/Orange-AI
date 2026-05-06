# q1 LoRA Dataset Report

- generated_at: 2026-05-06 17:04:46
- output: llm/q1/dataset/lora/q1_selfplay.jsonl
- samples: 260
- duplicate_removed: 1031
- average_quality_score: 0.613

## Labels
- accepted_read_only: 45
- positive: 84
- rejected: 131

## Sources
- self-play-model-v1: 217
- self-play: 37
- proposal-autopilot-safe-write: 5
- self-play-model-skip-v1: 1

## Rejection Reasons
- tool_parse: missing or malformed tool_call: 47
- static_reject: replace_function validation failed: Unsafe generated content blocked: use a specific exception type instead of except Exception: 21
- <tool_result name='replace_function' status='error'>Unsafe generated content blocked: use a specific exception type instead of except Exception</tool_result>: 13
- validation failed after apply; rollback completed: 10
- static_reject: replace_function validation failed: Replacement content must contain exactly one function definition: 8
- static_reject: protected target function: run_validation_suite: 7
- static_reject: protected target function: replace: 6
- <tool_result name='replace_function' status='error'>Replacement content must contain exactly one function definition</tool_result>: 4
- static_reject: protected target function: _ensure_write_allowed: 4
- static_reject: protected target function: _resolve_path: 4
- static_reject: protected target function: execute_bash: 2
- static_reject: protected target function: replace_function: 2
- <tool_result name='replace_function' status='error'>Replacement function is not valid Python: EOL while scanning string literal at line 49</tool_result>: 1
- hallucination: added non-existent CLI flags --check-runtime-errors and --check-memory-leaks; would permanently fail validation suite: 1
- static_reject: replace_function validation failed: Replacement function is not valid Python: EOL while scanning string literal at line 49: 1

## Model Parse Reasons
- ok: 172
- no_tool_call: 40
- truncated_output: 6

## Model Call Metrics
- calls: 1159
- average_elapsed_seconds: 32.395
- average_prompt_chars: 0
- average_output_chars: 0
- average_source_chars: 0
- average_output_chars_per_second: 0
- average_seconds_per_1k_output_chars: 0
