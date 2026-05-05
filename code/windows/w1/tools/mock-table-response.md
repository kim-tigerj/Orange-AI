현재 코드 소스 분석

| 파일 | 역할 |
| --- | --- |
| Persistence.h | 로컬 세션 영속화 |
| Database.h | SQLite 래퍼 |
| Coordination.h/.cpp | 멀티 인스턴스 메시징 |
| InstanceLauncher.h/.cpp | 멤버 인스턴스 실행 |
| ManagerEnvironment.h/.cpp | manager 환경변수 주입 |

현재 상태 진단

- 빌드 산출물은 존재합니다.
- 다음 단계는 build/capture 결과 카드를 실제 값으로 채우는 것입니다.
