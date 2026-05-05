import ast

def validate_python_code(code):
    """파이썬 코드 문법 검증"""
    try:
        ast.parse(code)
        return True, "OK"
    except SyntaxError as e:
        return False, f"구문 오류 발생: '{e.msg}' (위치: {e.lineno}행, {e.offset}열)"
    except Exception as e:
        return False, f"알 수 없는 코드 검증 에러: {str(e)}"

def get_function_source(source, func_name):
    """소스 코드에서 특정 함수의 본체를 추출"""
    tree = ast.parse(source)
    for node in ast.walk(tree):
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef)) and node.name == func_name:
            lines = source.splitlines(keepends=True)
            start_idx = node.lineno - 1
            end_idx = getattr(node, 'end_lineno', len(lines))
            return "".join(lines[start_idx:end_idx])
    return None

def list_functions_in_source(source):
    """소스 코드 내의 모든 함수 목록 반환"""
    tree = ast.parse(source)
    return [node.name for node in ast.walk(tree) if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef))]
