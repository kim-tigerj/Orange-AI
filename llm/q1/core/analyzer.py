import os
import ast

def get_project_stats(project_root):
    stats = {}
    for root, _, files in os.walk(project_root):
        for file in files:
            if file.endswith(".py"):
                path = os.path.join(root, file)
                try:
                    with open(path, 'r', encoding='utf-8') as f:
                        tree = ast.parse(f.read())
                        funcs = [n.name for n in ast.walk(tree) if isinstance(n, (ast.FunctionDef, ast.AsyncFunctionDef))]
                        stats[path] = len(funcs)
                except (OSError, SyntaxError, UnicodeDecodeError):
                    continue
    return stats
