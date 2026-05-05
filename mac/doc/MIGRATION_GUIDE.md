# M4 Mac Migration Guide (맥북 이주 지침서)

이 문서는 윈도우 환경에서 M4 맥북으로 대표님의 개발 정체성(Key, Config)을 안전하게 전이하기 위한 지침서입니다.

## 📦 준비물
- 본 폴더에 포함된 **TransferToMac.zip**: SSH 키, Git 설정, Gemini CLI 설정이 압축되어 있습니다.

## 🚀 이주 단계 (맥북 터미널에서 수행)

### 1. 패키지 압축 해제
윈도우의 `Orange-AI/mac/TransferToMac.zip` 파일을 맥북의 적절한 곳(예: `~/Downloads`)으로 복사한 뒤 다음을 실행합니다.
\`\`\`bash
unzip TransferToMac.zip -d ~/TransferToMac
cd ~/Downloads/TransferToMac
\`\`\`

### 2. 자동 설정 스크립트 실행
\`\`\`bash
bash setup_mac.sh
\`\`\`
- 이 스크립트는 SSH 키를 `~/.ssh/`로, Git 설정을 `~/.gitconfig`로, Gemini 설정을 `~/.gemini/`로 자동 배치합니다.
- 권한 설정(chmod)까지 자동으로 완료합니다.

### 3. 환경 반영
\`\`\`bash
source ~/.zshrc
\`\`\`

## 🛠️ 확인 사항
- **Git**: \`git config --list\` 명령어로 성함과 이메일이 나오는지 확인.
- **SSH**: \`ssh -T git@github.com\` 명령어로 인증 성공 확인.
- **Gemini**: \`gemini --version\` 실행 시 정상 동작 확인.

---
상위 개념: [Orange Ecosystem Governance](../../doc/GOVERNANCE.md)
