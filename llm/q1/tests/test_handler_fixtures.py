import ast
import os
import unittest

from core.handlers import ToolHandler


PROJECT_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))


class ToolHandlerFixtureTests(unittest.TestCase):
    def setUp(self):
        self.handler = ToolHandler(
            log_dir=os.path.join(PROJECT_ROOT, "llm", "log"),
            project_root=PROJECT_ROOT,
            allowed_write_paths=[],
        )

    def test_strip_code_fence_handles_python_fence(self):
        content = "```python\nprint('x')\n```"
        self.assertEqual(self.handler._strip_code_fence(content), "print('x')")

    def test_strip_code_fence_preserves_unfenced_content(self):
        content = "def target():\n    return 'ok'\n"
        self.assertEqual(self.handler._strip_code_fence(content), content)

    def test_replacement_function_node_requires_single_function(self):
        with self.assertRaises(ValueError):
            self.handler._replacement_function_node("x = 1\n", "target")
        with self.assertRaises(ValueError):
            self.handler._replacement_function_node("def other():\n    pass\n", "target")

    def test_assert_function_boundary_preserved_accepts_body_only_change(self):
        original = ast.parse("def target(value: int) -> int:\n    return value\n").body[0]
        replacement = ast.parse("def target(value: int) -> int:\n    return value + 1\n").body[0]
        self.handler._assert_function_boundary_preserved(original, replacement)

    def test_assert_function_boundary_preserved_rejects_signature_change(self):
        original = ast.parse("def target(value):\n    return value\n").body[0]
        replacement = ast.parse("def target(value, extra=None):\n    return value\n").body[0]
        with self.assertRaises(ValueError):
            self.handler._assert_function_boundary_preserved(original, replacement)

    def test_parse_tool_calls_block_content_preserves_embedded_tool_tag(self):
        response = (
            '<tool_call name="replace_function" path="llm/q1/tmp/sample.py" func_name="target">\n'
            "def target():\n"
            "    marker = '<tool_call name=\"noop\" />'\n"
            "    return marker\n"
            "</tool_call>"
        )
        calls = self.handler.parse_tool_calls(response)
        self.assertEqual(len(calls), 1)
        self.assertEqual(calls[0]["name"], "replace_function")
        self.assertIn("marker = '<tool_call", calls[0]["content"])

    def test_parse_tool_calls_raw_cdata_block(self):
        response = (
            '<tool_call name="replace_function" path="llm/q1/tmp/sample.py" func_name="target">\n'
            "<![CDATA[\n"
            "def target():\n"
            "    return 'ok'\n"
            "]]>\n"
            "</tool_call>"
        )
        calls = self.handler.parse_tool_calls(response)
        self.assertEqual(len(calls), 1)
        self.assertIn("return 'ok'", calls[0]["content"])
        self.assertNotIn("<![CDATA[", calls[0]["content"])

    def test_parse_tool_calls_supports_function_alias(self):
        response = '<tool_call function="list_directory" path="llm/q1" />'
        calls = self.handler.parse_tool_calls(response)
        self.assertEqual(len(calls), 1)
        self.assertEqual(calls[0]["name"], "list_directory")
        self.assertEqual(calls[0]["path"], "llm/q1")


if __name__ == "__main__":
    unittest.main()
