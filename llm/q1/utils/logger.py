import os
from datetime import datetime

def log_blackbox(log_dir, reasoning=None, action=None, result=None, elapsed=None):
    """로그 로테이션 기능이 포함된 블랙박스 로깅"""
    log_date = datetime.now().strftime('%Y%m%d')
    base_log_path = os.path.join(log_dir, f"BLACKBOX_{log_date}.md")
    
    log_path = base_log_path
    if os.path.exists(base_log_path) and os.path.getsize(base_log_path) > 1024 * 1024:
        count = 1
        while os.path.exists(os.path.join(log_dir, f"BLACKBOX_{log_date}_{count}.md")):
            count += 1
        log_path = os.path.join(log_dir, f"BLACKBOX_{log_date}_{count}.md")

    timestamp = datetime.now().strftime('%H:%M:%S')
    log_entry = [
        f"\n## [{timestamp}]",
        f"**🧠 THOUGHT:** {reasoning.strip()}\n\n" if reasoning else "",
        f"**🏗️  ACTION:** {action}\n\n" if action else "",
        f"**⏱️  ELAPSED:** {elapsed:.4f}s\n\n" if elapsed is not None else "",
        f"**✅ RESULT:**\n{result.strip()}\n\n---\n" if result else ""
    ]
    with open(log_path, "a", encoding="utf-8") as f:
        f.write("".join(log_entry))
