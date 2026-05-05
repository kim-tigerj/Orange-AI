# Orange-AI (The Brain & Body Ecosystem)

> **최선 = 반복 × 시간. 이 공식을 인간의 손에서 떼어 LLM 의 시간으로 굴리는 도구.**

이 프로젝트는 윈도우 네이티브 앱(Body)과 로컬 LLM 연구(Brain)를 결합하여, 인간의 개입을 최소화하는 자율 에이전트 시스템을 구축하는 것을 목표로 합니다.

## 🏗️ 에코시스템 구조
- **doc/**: [Conceptual] 최상위 비전, 전략, 소통 및 대표님 보고 사령부.
- **code/windows/**: [The Body] 윈도우 네이티브 앱 개발 및 플랫폼 전략.
- **llm/mac/**: [The Brain] M4 맥북 기반 로컬 LLM 연구 및 최적화 전략.

## 🏛️ 거버넌스 (Governance)
우리는 다음 4대 위계 체계로 움직입니다:
1. **Head (대표님)**: 비전 제시 및 최종 의사결정.
2. **General Manager (정팀장)**: 자원 배분 및 공정 관리.
3. **Supervisor (오감독)**: 품질 검증 및 4축 리뷰.
4. **Member (팀원)**: 단위 작업 수행.

## 🚦 가이드
- 모든 의사결정과 연구 결과는 각 계층의 `doc/` 폴더에 기록됩니다.
- 상위 문서는 하위 문서의 개념적 부모 역할을 하며, 하위는 상위의 비전을 구체화합니다.
- 새로운 인스턴스는 반드시 [GOVERNANCE.md](doc/GOVERNANCE.md)를 먼저 숙지해야 합니다.

## Oh-Council / 오감독 3자 협업체계

Claude Code, Gemini, Codex를 함께 쓰는 작업은 **Oh-Council** 프로토콜을 따릅니다.
영문명은 **Oh Supervisor 3-Way Collaboration System**입니다.
터미널에서 한국어가 깨질 수 있으므로 CLI/문서 식별자는 `Oh-Council`을 사용합니다.

시작 시 반드시 읽을 문서:

- [OH_COUNCIL.md](OH_COUNCIL.md)
- [MANAGER_HANDOFF.md](MANAGER_HANDOFF.md)
- [CLAUDE.md](CLAUDE.md)

핵심 규칙:

```text
논의는 3자 합의.
실행은 단일 책임.
검증은 3자 순환.
결정은 문서에 기록.
```
