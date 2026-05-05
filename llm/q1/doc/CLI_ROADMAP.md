# q1 CLI Roadmap

## Purpose
q1 CLI는 대표님 개입 없이 정팀장이 반복 개선을 수행하도록 돕는 운영 도구다. 기능은 작은 단위로 추가하고, 각 기능은 서버/모델 없이 검증 가능한 경로를 우선한다.

## Backlog
1. `[DONE] --status`: todo/done/report/latest summary 상태를 즉시 출력한다.
2. `[DONE] --list-jobs`: todo/done/report 파일을 정렬해 보여준다.
3. `[DONE] --new-function-job <path:function> --goal <text>`: 함수 단위 job 파일을 생성한다.
4. `[DONE] --archive-skipped`: 오래된 비함수 job을 skip 보고서와 함께 done으로 이동한다.
5. `[DONE] --validate`: compile, self-check, direct task를 한 번에 실행한다.
6. `[DONE] --server-health`: q1 서버 health와 응답 시간을 확인한다.
7. `[DONE] --auto-improve --limit N --max-no-change M`: NO_CHANGE 반복 중단 기준을 CLI에서 지정한다.
8. `[DONE] --report-latest`: latest_summary.md만 출력한다.
9. `[DONE] --dry-run-auto`: auto-improve가 처리할 job 순서와 skip 이유를 실행 없이 보여준다.
10. `[DONE] --compact-logs`: 긴 RESPONSE_HISTORY/BLACKBOX를 최신 요약 중심으로 압축한다.

## Implementation Order
현재 초기 CLI 로드맵은 완료됐다. 다음 확장은 새 실행 데이터가 쌓인 뒤 반복 루프의 병목을 보고 정한다.
