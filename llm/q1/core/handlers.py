import re
import html
import time
import os
import subprocess
import sys
import ast
import json
import textwrap
from utils.logger import log_blackbox
from utils.ast_utils import list_functions_in_source, get_function_source, validate_python_code
from rich.console import Console

console = Console()

class ToolHandler:
    def __init__(self, log_dir, project_root, allowed_write_paths=None):
        self.log_dir = log_dir
        self.project_root = project_root
        self.allowed_write_paths = None
        if allowed_write_paths is not None:
            self.allowed_write_paths = [self._resolve_path(path) for path in allowed_write_paths]
        self.tool_map = {
            "list_functions": self.list_functions,
            "read_function": self.read_function,
            "replace_function": self.replace_function,
            "write_file": self.write_file,
            "replace": self.replace,
            "read_file": self.read_file,
            "execute_bash": self.execute_bash,
            "list_directory": self.list_directory,
            "list_files": self.list_directory, 
            "restart_system": self.restart
        }

    def _resolve_path(self, path):
        if not path:
            path = "."
        clean_path = path.lstrip('/')
        if clean_path.startswith('llm/llm/'):
            clean_path = clean_path.replace('llm/llm/', 'llm/', 1)
    
        resolved_path = os.path.abspath(os.path.join(self.project_root, clean_path))
        normalized_path = os.path.normpath(resolved_path)
        project_root = os.path.abspath(self.project_root)

        if os.path.commonpath([project_root, normalized_path]) != project_root:
            raise ValueError(f"Attempted path traversal outside project root: {path} resolved to {normalized_path}")
    
        return normalized_path
    def _ensure_write_allowed(self, full_path):
        if self.allowed_write_paths is None:
            return
        normalized = os.path.normpath(os.path.abspath(full_path))
        for allowed in self.allowed_write_paths:
            if normalized == allowed or normalized.startswith(allowed + os.sep):
                return
        raise PermissionError(f"Write operation is not allowed for the path: {normalized}. Allowed paths are: {', '.join(self.allowed_write_paths)}")
    def _validate_python_with_context(self, source):
        try:
            ast.parse(source)
            return True, "OK"
        except SyntaxError as e:
            source_lines = source.splitlines()
            line_no = e.lineno or 0
            start = max(line_no - 2, 1)
            end = min(line_no + 2, len(source_lines))
            context = []
            for idx in range(start, end + 1):
                marker = ">" if idx == line_no else " "
                context.append(f"{marker} {idx}: {source_lines[idx - 1]}")
            context_str = "\n".join(context)
            return False, (
                f"구문 오류 발생: '{e.msg}' (위치: {e.lineno}행, {e.offset}열)\n"
                f"주변 문맥:\n{context_str}"
            )
        except Exception as e:
            return False, f"알 수 없는 코드 검증 에러: {str(e)}"
    def _reject_unsafe_generated_content(self, path, content, func_name=None):
        if not path or not path.endswith(".py"):
            return
        if func_name == "_reject_unsafe_generated_content":
            return
        blocked_patterns = [
            ("./venv/bin/flake8", "unavailable validation tool"),
            ("./venv/bin/pylint", "unavailable validation tool"),
            ("python -m unittest discover -s llm/q1/tests", "missing test directory validation"),
            ("json.dumps(summary", "do not change markdown summary format"),
            ("yaml.dump", "do not introduce yaml output format"),
            ("os.system", "avoid using os.system for security reasons"),
            ("subprocess.call", "avoid using subprocess.call for security reasons"),
            ("exec(", "avoid using exec for security reasons"),
            ("eval(", "avoid using eval for security reasons"),
            ("self.log_error", "do not call undefined logging helpers"),
            ("handle_model_load_failure", "do not introduce undefined model failure helpers"),
            ("Error generating response", "do not hide generation failures as model text"),
            ("No jobs", "do not include English operational messages"),
            ("Consider creating", "do not include English operational messages"),
            ("--create-function-job", "do not include non-existent CLI commands"),
        ]
        for pattern, reason in blocked_patterns:
            if pattern in content:
                raise Exception(f"Unsafe generated content blocked: {reason}: {pattern}")
        if re.search(r"(^|\n)\s*except\s+Exception\b", content):
            raise Exception("Unsafe generated content blocked: use a specific exception type instead of except Exception")
        if path.endswith("q1.py") and re.search(r"console\.print\(f?[\"\'](?:Error|File not found|Permission denied|An error occurred)", content):
            raise Exception("Unsafe generated content blocked: keep q1 CLI diagnostics in Korean and project style")
        if path.endswith("q1_server.py") and "console.print" in content:
            raise Exception("Unsafe generated content blocked: q1_server.py must use plain print logging")
        if not (path.endswith("q1_server.py") and func_name == "log") and re.search(r"(^|\n)\s*print\s*\(", content):
            raise Exception("Unsafe generated content blocked: use console.print for q1 CLI output: print(")

    def _strip_code_fence(self, content):
        stripped = content.strip()
        match = re.fullmatch(r"```(?:python)?\s*\n(.*?)\n```", stripped, re.DOTALL)
        if match:
            return match.group(1)
        lines = stripped.splitlines()
        if len(lines) >= 2 and lines[0].strip().startswith("```"):
            for index in range(len(lines) - 1, 0, -1):
                if lines[index].strip().startswith("```"):
                    return "\n".join(lines[1:index])
        return content

    def _replacement_function_node(self, content, func_name):
        try:
            tree = ast.parse(textwrap.dedent(content).strip() + "\n")
        except SyntaxError as e:
            raise ValueError(f"Replacement function is not valid Python: {e.msg} at line {e.lineno}") from e
        function_nodes = [
            node for node in tree.body
            if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef))
        ]
        if len(function_nodes) != 1 or len(tree.body) != 1:
            raise ValueError("Replacement content must contain exactly one function definition")
        node = function_nodes[0]
        if node.name != func_name:
            raise ValueError(f"Replacement function name mismatch: expected {func_name}, got {node.name}")
        return node

    def _assert_function_boundary_preserved(self, original_node, replacement_node):
        if type(original_node) is not type(replacement_node):
            raise ValueError("Replacement must preserve async/sync function kind")
        def comparable(value):
            if value is None:
                return None
            if isinstance(value, list):
                return [comparable(item) for item in value]
            return ast.dump(value, include_attributes=False)
        comparisons = [
            ("arguments", original_node.args, replacement_node.args),
            ("return annotation", original_node.returns, replacement_node.returns),
            ("decorators", original_node.decorator_list, replacement_node.decorator_list),
        ]
        for label, original_value, replacement_value in comparisons:
            if comparable(original_value) != comparable(replacement_value):
                raise ValueError(f"Replacement must preserve function {label}")
        control_nodes = [
            ("return", ast.Return),
            ("yield", (ast.Yield, ast.YieldFrom)),
        ]
        for label, node_type in control_nodes:
            if self._function_body_has_node(original_node, node_type) and not self._function_body_has_node(replacement_node, node_type):
                raise ValueError(f"Replacement must preserve function {label} flow")

    def _function_body_has_node(self, function_node, node_type):
        stack = list(function_node.body)
        while stack:
            node = stack.pop()
            if isinstance(node, node_type):
                return True
            if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef, ast.Lambda, ast.ClassDef)):
                continue
            stack.extend(ast.iter_child_nodes(node))
        return False

    def parse_tool_calls(self, text):
        calls = []
        block_pattern = re.compile(
            r'<tool_call\s+([^>]*)>\s*<content>\s*(?:<!\[CDATA\[)?(.*?)(?:\]\]>)?\s*</content>\s*</tool_call>',
            re.DOTALL,
        )
        tag_items = []
        masked_text_parts = []
        cursor = 0
        for match in block_pattern.finditer(text):
            tag_items.append((match.group(1), match.group(2)))
            masked_text_parts.append(text[cursor:match.start()])
            cursor = match.end()
        masked_text_parts.append(text[cursor:])
        masked_text = "".join(masked_text_parts)
        generic_block_pattern = re.compile(r'<tool_call\s+([^>]*)>\s*(.*?)^\s*</tool_call>', re.DOTALL | re.MULTILINE)
        generic_mask_parts = []
        cursor = 0
        for match in generic_block_pattern.finditer(masked_text):
            generic_content = match.group(2).strip()
            if generic_content.startswith("<![CDATA[") and generic_content.endswith("]]>"):
                generic_content = generic_content[len("<![CDATA["):-len("]]>")]
            tag_items.append((match.group(1), generic_content.strip()))
            generic_mask_parts.append(masked_text[cursor:match.start()])
            cursor = match.end()
        generic_mask_parts.append(masked_text[cursor:])
        masked_text = "".join(generic_mask_parts)
        tag_items.extend((tag_content, None) for tag_content in re.findall(r'<tool_call\s+(.*?)\s*/>', masked_text, re.DOTALL))
        for tag_content, block_content in tag_items:
            call = {}
            content_match = re.search(r'\bcontent=(["\'])', tag_content, re.DOTALL)
            attr_text = tag_content
            if block_content is not None:
                call["content"] = html.unescape(block_content)
            elif content_match:
                quote = content_match.group(1)
                content_start = content_match.end()
                content_end = tag_content.rfind(quote)
                if content_end >= content_start:
                    call["content"] = html.unescape(tag_content[content_start:content_end])
                    attr_text = tag_content[:content_match.start()]
            attrs = re.findall(r'(\w+)=(["\'])(.*?)\2', attr_text, re.DOTALL)
            for attr_name, _, attr_val in attrs:
                call[attr_name] = html.unescape(attr_val)
            if 'name' not in call:
                call['name'] = call.get('function') or call.get('type')
            if 'name' in call: calls.append(call)
        hermes_pattern = re.compile(r'<tool_call>\s*(\{.*?\})\s*</tool_call>', re.DOTALL)
        for match in hermes_pattern.finditer(text):
            try:
                parsed = json.loads(match.group(1))
            except json.JSONDecodeError:
                continue
            if not isinstance(parsed, dict):
                continue
            name = parsed.get("name")
            arguments = parsed.get("arguments")
            if not isinstance(name, str) or not isinstance(arguments, dict):
                continue
            call = dict(arguments)
            call["name"] = name
            calls.append(call)
        if not calls:
            candidate = text.strip()
            fence_match = re.match(r'^```(?:json)?\s*(.*?)\s*```$', candidate, re.DOTALL)
            if fence_match:
                candidate = fence_match.group(1).strip()
            if candidate.startswith('{') and candidate.endswith('}'):
                try:
                    parsed = json.loads(candidate)
                except json.JSONDecodeError:
                    parsed = None
                if isinstance(parsed, dict):
                    name = parsed.get("name")
                    arguments = parsed.get("arguments")
                    if isinstance(name, str) and isinstance(arguments, dict):
                        call = dict(arguments)
                        call["name"] = name
                        calls.append(call)
        return calls

    def execute_tools(self, response):
        calls = self.parse_tool_calls(response)
        if not calls: return None
        results = []
        for call in calls:
            name = call.get('name')
            start_t = time.time()
            try:
                if name in self.tool_map:
                    result_text = self.tool_map[name](call)
                    results.append(result_text)
                    log_blackbox(self.log_dir, action=name, result="SUCCESS", elapsed=time.time()-start_t)
                else: raise Exception(f'Unknown tool: {name}')
            except Exception as e:
                results.append(f"<tool_result name='{name}' status='error'>{str(e)}</tool_result>")
                log_blackbox(self.log_dir, action=name, result=f"ERROR: {e}", elapsed=time.time()-start_t)
        return "\n".join(results)

    def list_functions(self, call):
        try:
            with open(self._resolve_path(call.get('path')), 'r', encoding='utf-8') as f:
                source = f.read()
            functions = list_functions_in_source(source)
            if not functions:
                return "<tool_result name='list_functions' status='success'>No functions found.</tool_result>"
            return f"<tool_result name='list_functions' status='success'>\n{chr(10).join(functions)}\n</tool_result>"
        except FileNotFoundError:
            return "<tool_result name='list_functions' status='error'>File not found.</tool_result>"
        except IOError as e:
            return f"<tool_result name='list_functions' status='error'>Failed to read file: {e.strerror}</tool_result>"
    def read_function(self, call):
        try:
            with open(self._resolve_path(call.get('path')), 'r', encoding='utf-8') as f:
                source = f.read()
        except FileNotFoundError:
            raise Exception(f"File not found: {call.get('path')}")
        except Exception as e:
            raise Exception(f"Error reading file: {call.get('path')} - {str(e)}")

        func_source = get_function_source(source, call.get('func_name'))
        if func_source is None:
            raise Exception(f"Function not found: {call.get('func_name')} in file: {call.get('path')}")

        return f"<tool_result name='read_function' status='success'>\n{func_source}\n</tool_result>"
    def replace_function(self, call):
        path, func_name, content = call.get('path'), call.get('func_name'), call.get('content')
        if not path or not func_name or content is None:
            raise Exception("path, func_name, and content are required")
        content = self._strip_code_fence(content)
        full_path = self._resolve_path(path)
        self._ensure_write_allowed(full_path)
        self._reject_unsafe_generated_content(path, content, func_name=func_name)
        with open(full_path, 'r', encoding='utf-8') as f: source = f.read()
        tree = ast.parse(source)
        target = next((n for n in ast.walk(tree) if isinstance(n, (ast.FunctionDef, ast.AsyncFunctionDef)) and n.name == func_name), None)
        if not target: raise Exception(f"Function {func_name} not found in {path}")
        replacement_node = self._replacement_function_node(content, func_name)
        self._assert_function_boundary_preserved(target, replacement_node)
        lines = source.splitlines(keepends=True)
        original_line = lines[target.lineno - 1]
        original_indent = original_line[:len(original_line) - len(original_line.lstrip())]
        content_lines = content.splitlines(keepends=True)
        if content_lines and not content_lines[-1].endswith(("\n", "\r")):
            content_lines[-1] += "\n"
        first_line = next((line for line in content_lines if line.strip()), "")
        if original_indent and first_line.startswith(("def ", "async def ")):
            content_lines = [
                original_indent + line if line.strip() else line
                for line in content_lines
            ]
        lines[target.lineno-1:getattr(target, 'end_lineno', len(lines))] = content_lines
        updated = "".join(lines)
        if path.endswith(".py"):
            valid, err = self._validate_python_with_context(updated)
            if not valid: raise Exception(f"Python validation failed: {err}")
            updated_tree = ast.parse(updated)
            updated_target = next((n for n in ast.walk(updated_tree) if isinstance(n, (ast.FunctionDef, ast.AsyncFunctionDef)) and n.name == func_name), None)
            if not updated_target:
                raise ValueError(f"Post-splice verification failed: target function {func_name} disappeared")
            self._assert_function_boundary_preserved(target, updated_target)
            original_functions = sorted(n.name for n in ast.walk(tree) if isinstance(n, (ast.FunctionDef, ast.AsyncFunctionDef)))
            updated_functions = sorted(n.name for n in ast.walk(updated_tree) if isinstance(n, (ast.FunctionDef, ast.AsyncFunctionDef)))
            if original_functions != updated_functions:
                raise ValueError("Post-splice verification failed: function topology changed")
        with open(full_path, 'w', encoding='utf-8') as f: f.write(updated)
        return "<tool_result name='replace_function' status='success'>교체 완료</tool_result>"
    def write_file(self, call):
        full_path = self._resolve_path(call.get('path'))
        self._ensure_write_allowed(full_path)
        start, end, content = call.get('start_line'), call.get('end_line'), call.get('content')
        if content is None:
            raise Exception("content is required")
        content = self._strip_code_fence(content)
        self._reject_unsafe_generated_content(call.get('path'), content)
        if start:
            with open(full_path, 'r', encoding='utf-8') as f:
                lines = f.readlines()
            lines[int(start)-1:int(end) if end else int(start)] = content.splitlines(keepends=True)
            updated = "".join(lines)
        else:
            updated = content
        if call.get('path').endswith(".py"):
            valid, err = self._validate_python_with_context(updated)
            if not valid:
                raise Exception(f"Python validation failed: {err}")
        with open(full_path, 'w', encoding='utf-8') as f:
            f.write(updated)
        return "<tool_result name='write_file' status='success'>기록 완료</tool_result>"
    def replace(self, call):
        full_path = self._resolve_path(call.get('path'))
        self._ensure_write_allowed(full_path)
        with open(full_path, 'r', encoding='utf-8') as f: lines = f.readlines()
        old, new = call.get('old_string'), call.get('new_string')
        if old is None or new is None:
            raise Exception("old_string and new_string are required")
        self._reject_unsafe_generated_content(call.get('path'), new)
        hits = [i for i, line in enumerate(lines) if old in line]
        if len(hits) == 1:
            lines[hits[0]] = lines[hits[0]].replace(old, new)
            updated = "".join(lines)
            if full_path.endswith(".py"):
                valid, err = self._validate_python_with_context(updated)
                if not valid: raise Exception(err)
            with open(full_path, 'w', encoding='utf-8') as f: f.write(updated)
            return "<tool_result name='replace' status='success'>수정 완료</tool_result>"
        raise Exception(f"Unique match failed (count: {len(hits)}). Expected 1 match but found {len(hits)} matches. Matches found in lines: {', '.join(map(str, hits))}")
    def read_file(self, call):
        path = self._resolve_path(call.get('path'))
        start_line = int(call.get('start_line', 1))
        end_value = call.get('end_line')
        end_line = int(end_value) if end_value is not None else None

        try:
            with open(path, 'r', encoding='utf-8') as f:
                lines = f.readlines()
        except FileNotFoundError:
            return f"<tool_result name='read_file' status='error'>File not found: {path}</tool_result>"
        except PermissionError:
            return f"<tool_result name='read_file' status='error'>Permission denied: {path}</tool_result>"
        except Exception as e:
            return f"<tool_result name='read_file' status='error'>An error occurred: {str(e)}</tool_result>"

        if start_line < 1 or (end_line is not None and end_line < start_line):
            return f"<tool_result name='read_file' status='error'>Invalid line range: start_line={start_line}, end_line={end_line}</tool_result>"

        selected = lines[start_line-1:end_line] if end_line else lines[start_line-1:]
        return f"<tool_result name='read_file' status='success'>\n{''.join(selected)}\n</tool_result>"
    def execute_bash(self, call):
        command = call.get('command')
        if not command:
            raise Exception("command is required")
        try:
            res = subprocess.run(
                command, 
                shell=True, 
                capture_output=True, 
                text=True, 
                cwd=self.project_root, 
                timeout=60  # Set timeout to 60 seconds
            )
            status = "success" if res.returncode == 0 else "error"
            return (
                f"<tool_result name='execute_bash' status='{status}'>\n"
                f"RETURN_CODE: {res.returncode}\n"
                f"STDOUT: {res.stdout[:1000]}\n"  # Limit output to 1000 characters
                f"STDERR: {res.stderr[:1000]}\n"  # Limit output to 1000 characters
                f"OUTPUT_LENGTH: {len(res.stdout)}\n"
                f"ERROR_LENGTH: {len(res.stderr)}\n"
                "</tool_result>"
            )
        except subprocess.TimeoutExpired:
            return (
                "<tool_result name='execute_bash' status='error'>\n"
                "RETURN_CODE: -1\n"
                "STDOUT: \n"
                "STDERR: Command timed out\n"
                "OUTPUT_LENGTH: 0\n"
                "ERROR_LENGTH: 0\n"
                "</tool_result>"
            )
    def list_directory(self, call):
        target = self._resolve_path(call.get('path', '.'))
        if not os.path.exists(target):
            raise Exception(f"Path not found: {target}")
        res = []
        for f in os.listdir(target):
            p = os.path.join(target, f)
            is_file = os.path.isfile(p)
            size = os.path.getsize(p) if is_file else 0
            type_ = 'file' if is_file else 'directory'
            res.append({"name": f, "size": size, "type": type_})
        return f"<tool_result name='list_directory' status='success'>\n{json.dumps(res)}\n</tool_result>"
    def restart(self, call): sys.exit(0)
