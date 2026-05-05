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


if __name__ == "__main__":
    unittest.main()
