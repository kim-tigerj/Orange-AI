import os
from rich.console import Console

console = Console()

class ProjectContext:
    """정팀장에게 고밀도 지식을 제공하며 토큰 낭비를 방지하는 스마트 컨텍스트 매니저"""
    
    def __init__(self, project_root):
        self.project_root = project_root
        # 정팀장이 항상 머릿속에 품고 있어야 할 헌법적 문서
        self.foundational_docs = [
            "llm/q1/CLAUDE.md",        # 운영 지침
            "README.md",               # 프로젝트 개요
            "doc/STRATEGIC_GOALS.md",  # 전략적 목표
            "doc/GOVERNANCE.md"        # 거버넌스 및 위계
        ]

    def get_concise_project_index(self):
        """프로젝트의 전체 구조를 트리 형태로 요약하여 지도로 제공합니다."""
        index = "### [PROJECT MAP & INDEX]\n"
        for root, dirs, files in os.walk(self.project_root):
            dirs[:] = [d for d in dirs if d not in ['.git', 'venv', 'node_modules', '__pycache__']]
            level = root.replace(self.project_root, '').count(os.sep)
            indent = '  ' * level
            index += f"{indent}📁 {os.path.basename(root)}/\n"
            sub_indent = '  ' * (level + 1)
            for f in files:
                if f.endswith(('.md', '.py', '.log')): # 주요 파일만 표시
                    index += f"{sub_indent}📄 {f}\n"
        return index

    def get_core_knowledge(self):
        """핵심 문서의 내용만 추출하여 정팀장의 기본 상식을 형성합니다."""
        knowledge = "### [CORE CONSTITUTION & KNOWLEDGE]\n\n"
        for doc in self.foundational_docs:
            full_path = os.path.join(self.project_root, doc)
            if os.path.exists(full_path):
                try:
                    with open(full_path, "r", encoding="utf-8") as f:
                        knowledge += f"--- DOCUMENT: {doc} ---\n{f.read()}\n\n"
                except OSError as e:
                    console.print(f"[yellow]core document skipped:[/yellow] {full_path}: {e}")
        return knowledge

    def get_persona_prompt(self):
        """persona/PROMPT.md 파일을 읽어 실시간 문맥과 결합한 최종 프롬프트를 생성합니다."""
        index = self.get_concise_project_index()
        knowledge = self.get_core_knowledge()
    
        persona_path = os.path.join(self.project_root, "llm/q1/persona/PROMPT.md")
        try:
            with open(persona_path, "r", encoding="utf-8") as f:
                template = f.read()
            # .format() 대신 .replace()를 사용하여 중괄호 { } 충돌 방지
            prompt = template.replace("{knowledge}", knowledge).replace("{index}", index)
            return prompt
        except FileNotFoundError:
            return "당신은 정팀장입니다. persona/PROMPT.md 파일을 찾을 수 없습니다."
        except OSError as e:
            return f"당신은 정팀장입니다. 시스템 오류로 인해 기본 모드로 작동합니다: {e}"
