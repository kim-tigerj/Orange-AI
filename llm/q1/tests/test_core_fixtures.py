import os
import tempfile
import unittest

import q1


class Q1CoreFixtureTests(unittest.TestCase):
    def setUp(self):
        self.engine = q1.Q1Engine(load_model=False)

    def test_parse_function_target_uses_last_colon(self):
        path, func_name = q1.parse_function_target("pkg:with:colon.py:target_func")
        self.assertEqual(path, "pkg:with:colon.py")
        self.assertEqual(func_name, "target_func")

    def test_parse_function_target_rejects_missing_parts(self):
        with self.assertRaises(ValueError):
            q1.parse_function_target("missing_colon")
        with self.assertRaises(ValueError):
            q1.parse_function_target("path.py:")

    def test_extract_job_goal_collapses_multiline_goal(self):
        job = "\n".join([
            "# q1 Function Job",
            "",
            "## Goal",
            "첫 줄 목표",
            "둘째 줄 목표",
            "## Constraints",
            "- one",
        ])
        self.assertEqual(self.engine.extract_job_goal(job), "첫 줄 목표 둘째 줄 목표")

    def test_extract_job_goal_returns_none_when_missing(self):
        self.assertIsNone(self.engine.extract_job_goal("# job without goal"))

    def test_module_name_for_q1_path(self):
        self.assertEqual(self.engine.module_name_for_q1_path("llm/q1/q1.py"), "q1")
        self.assertEqual(
            self.engine.module_name_for_q1_path("llm/q1/context/project_context.py"),
            "context.project_context",
        )
        self.assertIsNone(self.engine.module_name_for_q1_path("README.md"))

    def test_looks_like_direct_task_for_file_listing_request(self):
        self.assertTrue(q1.looks_like_direct_task("llm/q1/core 목록 확인해라"))
        self.assertTrue(q1.looks_like_direct_task("read_file llm/q1/q1.py"))

    def test_looks_like_direct_task_rejects_plain_chat(self):
        self.assertFalse(q1.looks_like_direct_task("정팀장 상태가 어떤지 설명해줘"))

    def test_response_cache_round_trip(self):
        with tempfile.TemporaryDirectory() as tmp_dir:
            self.engine.response_cache_dir = tmp_dir
            messages = [{"role": "user", "content": "same prompt"}]
            self.engine.write_response_cache(messages, 32, "cached answer")
            self.assertEqual(self.engine.read_response_cache(messages, 32), "cached answer")
            self.assertIsNone(self.engine.read_response_cache(messages, 33))

    def test_proposal_to_lora_sample_contains_assistant_response(self):
        proposal = {
            "id": "p1",
            "target": "llm/q1/q1.py:self_play",
            "problem": "제안만 생성한다",
            "rationale": "실제 파일 반영은 오감독이 한다",
            "risk": "low",
            "proposed_tool_call": "DONE_NO_CHANGE",
        }
        sample = self.engine.proposal_to_lora_sample(proposal, "proposal_pending")
        self.assertEqual(sample["label"], "proposal_pending")
        self.assertEqual(sample["messages"][-1]["role"], "assistant")
        self.assertEqual(sample["messages"][-1]["content"], "DONE_NO_CHANGE")
        self.assertTrue(sample["dedupe_key"])

    def test_proposal_to_lora_sample_includes_rejection_metadata(self):
        proposal = {
            "id": "p2",
            "target": "llm/q1/q1.py:self_play",
            "problem": "나쁜 제안을 학습 데이터로 남긴다",
            "rationale": "gate 실패 사유를 보존한다",
            "risk": "low",
            "source": "self-play-model-v1",
            "proposed_tool_call": "broken",
            "rejection": {"reason": "tool_parse: missing or malformed tool_call"},
            "model_output_raw": "broken",
        }
        sample = self.engine.proposal_to_lora_sample(proposal, "rejected")
        metadata = sample["metadata"]
        self.assertEqual(metadata["proposal_source"], "self-play-model-v1")
        self.assertEqual(metadata["rejection_reason"], "tool_parse: missing or malformed tool_call")
        self.assertEqual(metadata["model_parse_reason"], "no_tool_call")

    def test_proposal_rejection_reason_uses_gate_failure(self):
        proposal = {
            "rejection": {"reason": "gate failed"},
            "gate_result": {
                "results": [
                    {"name": "schema", "ok": True, "message": "schema ok"},
                    {"name": "static_reject", "ok": False, "message": "protected target function: replace"},
                ]
            }
        }
        self.assertEqual(
            self.engine.proposal_rejection_reason(proposal),
            "static_reject: protected target function: replace",
        )

    def test_gate_failure_reason_joins_failures(self):
        reason = self.engine.gate_failure_reason([
            {"name": "tool_parse", "ok": False, "message": "missing or malformed tool_call"},
            {"name": "static_reject", "ok": False, "message": "protected target function: replace"},
        ])
        self.assertEqual(
            reason,
            "tool_parse: missing or malformed tool_call; static_reject: protected target function: replace",
        )

    def test_proposal_tool_parse_failure_message_uses_truncated_output(self):
        proposal = {
            "proposed_tool_call": '<tool_call>\n{"name": "write_file", "arguments": {"path": "x"',
            "model_output_raw": '<tool_call>\n{"name": "write_file", "arguments": {"path": "x"',
        }
        self.assertEqual(self.engine.proposal_tool_parse_failure_message(proposal), "truncated_output")

    def test_proposal_tool_parse_failure_message_keeps_generic_for_no_tool(self):
        proposal = {"proposed_tool_call": "not a tool call", "model_output_raw": "not a tool call"}
        self.assertEqual(self.engine.proposal_tool_parse_failure_message(proposal), "missing or malformed tool_call")

    def test_autopilot_mode_validation(self):
        with self.assertRaises(SystemExit):
            self.engine.proposal_autopilot(mode="invalid")

    def test_autopilot_deterministic_kind_counts_empty(self):
        self.engine.self_play = lambda cycles: None
        self.engine.pending_proposal_ids = lambda: []
        self.engine.export_lora_dataset = lambda: None
        result = self.engine.proposal_autopilot(cycles=1, mode="deterministic")
        self.assertEqual(result["kind_counts"], {})

    def test_pick_default_self_play_target_skips_protected(self):
        target, _ = self.engine._pick_default_self_play_target(4, 0)
        self.assertNotEqual(target, "llm/q1/core/handlers.py:replace_function")

    def test_self_play_model_one_skips_long_source_without_inference(self):
        self.engine.tool_handler.read_function = lambda call: (
            "<tool_result name='read_function' status='success'>\n"
            + ("x" * (q1.MAX_SELF_PLAY_FUNCTION_CHARS + 1))
            + "\n</tool_result>"
        )
        calls = []
        self.engine.generate_response = lambda *args, **kwargs: calls.append(args) or "DONE_NO_CHANGE"
        self.engine.create_proposal = lambda **kwargs: {
            "id": "skip-test",
            **kwargs,
        }
        proposal = self.engine.self_play_model_one("llm/q1/q1.py:compact_logs", "긴 함수는 건너뛴다")
        self.assertEqual(proposal["proposed_tool_call"], "DONE_NO_CHANGE")
        self.assertEqual(proposal["source"], "self-play-model-skip-v1")
        self.assertEqual(calls, [])

    def test_parse_tool_calls_hermes_self_closing_equivalent(self):
        text = (
            '<tool_call>\n'
            '{"name": "read_function", "arguments": {"path": "llm/q1/q1.py", "func_name": "self_play"}}'
            '\n</tool_call>'
        )
        calls = self.engine.tool_handler.parse_tool_calls(text)
        self.assertEqual(len(calls), 1)
        self.assertEqual(calls[0]["name"], "read_function")
        self.assertEqual(calls[0]["path"], "llm/q1/q1.py")
        self.assertEqual(calls[0]["func_name"], "self_play")

    def test_parse_tool_calls_hermes_replace_function(self):
        content = "def sample():\n    return 1\n"
        text = (
            '<tool_call>\n'
            '{"name": "replace_function", "arguments": {"path": "llm/q1/q1.py", "func_name": "sample", '
            '"content": "def sample():\\n    return 1\\n"}}'
            '\n</tool_call>'
        )
        calls = self.engine.tool_handler.parse_tool_calls(text)
        self.assertEqual(len(calls), 1)
        self.assertEqual(calls[0]["name"], "replace_function")
        self.assertEqual(calls[0]["content"], content)

    def test_parse_tool_calls_legacy_xml_still_works(self):
        text = '<tool_call name="read_function" path="llm/q1/q1.py" func_name="self_play" />'
        calls = self.engine.tool_handler.parse_tool_calls(text)
        self.assertEqual(len(calls), 1)
        self.assertEqual(calls[0]["name"], "read_function")
        self.assertEqual(calls[0]["path"], "llm/q1/q1.py")
        self.assertEqual(calls[0]["func_name"], "self_play")

    def test_parse_tool_calls_hermes_malformed_json_skipped(self):
        text = (
            '<tool_call>{not json}</tool_call>\n'
            '<tool_call>\n'
            '{"name": "read_function", "arguments": {"path": "llm/q1/q1.py", "func_name": "self_play"}}'
            '\n</tool_call>'
        )
        calls = self.engine.tool_handler.parse_tool_calls(text)
        self.assertEqual(len(calls), 1)
        self.assertEqual(calls[0]["name"], "read_function")
        self.assertEqual(calls[0]["func_name"], "self_play")

    def test_parse_tool_calls_mixed_formats(self):
        text = (
            '<tool_call name="read_function" path="llm/q1/q1.py" func_name="self_play" />\n'
            '<tool_call>\n'
            '{"name": "list_functions", "arguments": {"path": "llm/q1/core/handlers.py"}}'
            '\n</tool_call>'
        )
        calls = self.engine.tool_handler.parse_tool_calls(text)
        self.assertEqual(len(calls), 2)
        self.assertEqual(calls[0]["name"], "read_function")
        self.assertEqual(calls[1]["name"], "list_functions")
        self.assertEqual(calls[1]["path"], "llm/q1/core/handlers.py")

    def test_parse_model_proposal_text_rejects_extra_text(self):
        text = (
            "설명입니다.\n"
            '<tool_call>\n'
            '{"name": "read_function", "arguments": {"path": "llm/q1/q1.py", "func_name": "self_play"}}'
            '\n</tool_call>'
        )
        parsed = self.engine.parse_model_proposal_text(text)
        self.assertEqual(parsed["kind"], "malformed")
        self.assertEqual(parsed["reason"], "extra_text")

    def test_parse_model_proposal_text_rejects_multiple_calls(self):
        text = (
            '<tool_call>\n'
            '{"name": "read_function", "arguments": {"path": "llm/q1/q1.py", "func_name": "self_play"}}'
            '\n</tool_call>\n'
            '<tool_call>\n'
            '{"name": "list_functions", "arguments": {"path": "llm/q1/q1.py"}}'
            '\n</tool_call>'
        )
        parsed = self.engine.parse_model_proposal_text(text)
        self.assertEqual(parsed["kind"], "malformed")
        self.assertEqual(parsed["reason"], "multiple_tool_calls")

    def test_parse_model_proposal_text_rejects_unparsed_tool_call(self):
        text = (
            '<tool_call>{not json}</tool_call>\n'
            '<tool_call>\n'
            '{"name": "read_function", "arguments": {"path": "llm/q1/q1.py", "func_name": "self_play"}}'
            '\n</tool_call>'
        )
        parsed = self.engine.parse_model_proposal_text(text)
        self.assertEqual(parsed["kind"], "malformed")
        self.assertEqual(parsed["reason"], "unparsed_tool_call")

    def test_parse_model_proposal_text_rejects_truncated_tool_call(self):
        text = (
            '<tool_call>\n'
            '{"name": "replace_function", "arguments": {"path": "llm/q1/q1.py", '
            '"func_name": "sample", "content": "def sample():\\n    return'
        )
        parsed = self.engine.parse_model_proposal_text(text)
        self.assertEqual(parsed["kind"], "malformed")
        self.assertEqual(parsed["reason"], "truncated_output")

    def test_parse_model_proposal_text_rejects_truncated_bare_json(self):
        text = (
            '{"name": "read_function", "arguments": {"path": "llm/q1/q1.py", '
            '"func_name": "self_play"'
        )
        parsed = self.engine.parse_model_proposal_text(text)
        self.assertEqual(parsed["kind"], "malformed")
        self.assertEqual(parsed["reason"], "truncated_output")

    def test_proposal_to_lora_sample_tracks_truncated_model_output(self):
        proposal = {
            "id": "p3",
            "target": "llm/q1/q1.py:self_play",
            "problem": "잘린 출력을 negative sample로 남긴다",
            "rationale": "truncation reason을 보존한다",
            "risk": "low",
            "source": "self-play-model-v1",
            "proposed_tool_call": '<tool_call>\n{"name": "write_file", "arguments": {"path": "x"',
            "rejection": {"reason": "tool_parse: missing or malformed tool_call"},
            "model_output_raw": '<tool_call>\n{"name": "write_file", "arguments": {"path": "x"',
        }
        sample = self.engine.proposal_to_lora_sample(proposal, "rejected")
        self.assertEqual(sample["metadata"]["model_parse_reason"], "truncated_output")

    def test_parse_model_proposal_text_accepts_bare_json_and_normalizes(self):
        text = (
            '{"name": "read_function", "arguments": {"path": "llm/q1/q1.py", "func_name": "self_play"}}'
        )
        parsed = self.engine.parse_model_proposal_text(text)
        self.assertEqual(parsed["kind"], "read_only")
        self.assertIn("<tool_call>", parsed["normalized_tool_call"])
        self.assertIn('"arguments"', parsed["normalized_tool_call"])

    def test_parse_model_proposal_text_accepts_single_write(self):
        text = (
            '<tool_call>\n'
            '{"name": "write_file", "arguments": {"path": "llm/q1/dataset/lora/WRITE_SAMPLES.md", "content": "ok"}}'
            '\n</tool_call>'
        )
        parsed = self.engine.parse_model_proposal_text(text)
        self.assertEqual(parsed["kind"], "write")
        self.assertEqual(parsed["reason"], "ok")
        self.assertEqual(
            parsed["normalized_tool_call"],
            '<tool_call>\n{"arguments": {"content": "ok", "path": "llm/q1/dataset/lora/WRITE_SAMPLES.md"}, "name": "write_file"}\n</tool_call>',
        )

    def test_static_reject_validates_replace_function_shape(self):
        proposal = {"target": "llm/q1/q1.py:cycle_once", "proposed_tool_call": "x"}
        calls = [{
            "name": "replace_function",
            "path": "llm/q1/q1.py",
            "func_name": "cycle_once",
            "content": (
                "def cycle_once(self, limit, max_no_change=0, compact_logs=False):\n"
                "    self.print_status()\n\n"
                "def added_function():\n"
                "    return None\n"
            ),
        }]
        reason = self.engine.static_reject_proposal(proposal, calls)
        self.assertIn("Replacement content must contain exactly one function definition", reason)


if __name__ == "__main__":
    unittest.main()
