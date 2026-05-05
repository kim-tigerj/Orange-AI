# q1 Evaluation Corpus

- generated_at: 2026-05-05 15:04:16
- discovered_target_count: 111
- seed_candidate_count: 1
- seed_rejected_recent_done_or_no_change: 110

## Guard Cases

- core_fixture_parse_function_target: target strings are split on the last colon and malformed targets raise `ValueError`.
- core_fixture_extract_job_goal: multiline job goals collapse into a stable single-line goal string.
- core_fixture_module_name_for_q1_path: q1 file paths map to importable module names and non-Python files are ignored.
- core_fixture_looks_like_direct_task: file/listing requests are direct tasks and plain chat is not.
- handler_fixture_strip_code_fence: fenced Python content is unwrapped and unfenced content is preserved.
- handler_fixture_replacement_function_node: replacement snippets must contain exactly one target function.
- handler_fixture_boundary_preserved: body-only changes are accepted and signature changes are rejected.
- handler_fixture_parse_tool_calls_block: block content preserves embedded tool-like strings.
- handler_fixture_parse_tool_calls_cdata: raw CDATA blocks unwrap into Python content.
- handler_fixture_parse_tool_calls_alias: self-closing `function=` aliases resolve to tool names.
- parse_tool_calls_roundtrip: XML attributes, block content, CDATA, and literal closing-tag strings parse correctly.
- replace_function_signature_guard: renamed functions are rejected.
- replace_function_decorator_guard: dropped decorators are rejected.
- replace_function_return_guard: replacements cannot drop existing return flow.
- replace_function_yield_guard: replacements cannot drop existing yield flow.
- replace_function_topology_guard: replacements cannot add or remove functions during splice.
- post_apply_rollback: failed validation can restore pre-change bytes.
- changed_python_import_guard: changed q1 Python modules must import in a subprocess.
- post_write_boundary_guard: target boundaries are checked again after writing.

## Validation Commands

- `./venv/bin/python -m py_compile llm/q1/q1.py llm/q1/core/handlers.py llm/q1/core/analyzer.py llm/q1/context/project_context.py llm/q1/utils/ast_utils.py llm/q1/utils/logger.py llm/q1/q1_server.py`
- `cd llm/q1 && ../../venv/bin/python -m unittest discover -s tests`
- `./venv/bin/python llm/q1/q1.py --self-check`
- `./venv/bin/python llm/q1/q1.py --tool-self-test`
- `Q1_SKIP_RESPONSE_LOG=1 ./venv/bin/python llm/q1/q1.py --task "llm/q1/core 목록 확인해라"`

## Next Corpus Gaps

- Add fixture-level behavior tests for functions that can be exercised without a model call.
- Add an import-time timeout fixture if q1 starts importing side-effect-heavy modules.
- Add a manifest for target-specific verification commands after the corpus is stable.
