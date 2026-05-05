# q1 CLI Next Roadmap

## Purpose
초기 CLI가 준비됐으므로 다음 단계는 대표님 개입 없이 반복 실행하기 쉬운 운영 단위를 만든다. 우선 모델 호출 전 점검과 비용 절감 장치를 한 명령으로 묶는다.

## Backlog
1. `[DONE] --cycle-once`: server health, skipped archive, dry-run, auto-improve, validate를 한 번 수행한다.
2. `[DONE] --loop --cycles N --interval SEC`: cycle-once를 제한 횟수만큼 반복한다.
3. `[DONE] --seed-default-jobs`: todo가 비었을 때 안전한 함수 단위 개선 job을 생성한다.
4. `[DONE] --queue-health`: todo/done/report/log 크기와 실행 가능 상태만 빠르게 출력한다.
5. `[DONE] --failure-latest`: 최신 실패/도구 오류만 출력한다.
6. `[DONE] --improve-max-tokens N`: 함수 단위 개선 생성 한도를 조절해 잘린 tool_call 실패를 줄인다.
7. `[DONE] --tool-self-test`: `replace_function` 런타임 동작, quote 혼합 content, 메서드 들여쓰기 보존을 검증한다.
8. `[DONE] --seed-from-failures`: `CONTINUE` 보고서를 실패 유형별 복구 job으로 변환한다.
9. `[DONE] block content tool_call`: 긴 코드 교체는 `<content><![CDATA[...]]></content>` 블록 포맷으로 받을 수 있게 한다.
10. `[DONE] --seed-discovered-jobs`: 허용된 핵심 파일의 실제 함수 목록을 읽고 최근 DONE/failed/todo를 제외한 다음 후보를 자동 생성한다.

## Implementation Order
현재 두 번째 CLI 로드맵은 완료됐다. 다음 확장은 실제 loop 실행 결과가 쌓이면 실패 유형과 병목을 기준으로 정한다.

## Seed Pool
- 기본 seed 후보가 모두 최근 `DONE/NO_CHANGE`이면 루프가 멈추므로 후보 풀을 운영 핵심 함수까지 확장한다.
- 추가 후보: `run_validation_suite`, `write_improve_report`, `server_health`, `parse_tool_calls`, `execute_tools`.
- 2026-05-03 추가 후보: `queue_health`, `seed_default_jobs`, `seed_from_failures`, `print_latest_failure`, `create_function_job`, `archive_job`, `generate_response_server`.
- 2026-05-03 추가 후보 2: `list_md_files`, `extract_target_function`, `extract_job_goal`, `extract_report_field`, `snapshot_paths`, `auto_improve`, `archive_skipped`, `loop`.
- 2026-05-03 추가 후보 3: handler 도구 경로의 `_resolve_path`, `_ensure_write_allowed`, `_validate_python_with_context`, `list_functions`, `read_function`, `write_file`, `replace`, `read_file`, `execute_bash`, `list_directory`.

## Observed Failure Types
- `replace_function` content가 긴 함수에서 잘리면 닫히지 않은 `<tool_call>`이 생긴다.
- q1은 이제 malformed/truncated tool_call을 도구 파싱 실패로 기록한다.
- 긴 함수 개선은 `--improve-max-tokens 1024` 이상으로 실행하는 편이 재시도 비용을 줄인다.
- 클래스 메서드 교체 시 모델이 무들여쓰기 `def`를 반환할 수 있어 `replace_function`이 원래 들여쓰기를 보존하게 했다.
- q1은 함수 개선 시 기존 CLI 출력 형식과 파일 포맷을 유지하도록 지시받는다.
- `content='...'` 내부에 double quote나 single quote가 섞이면 단순 속성 정규식이 코드를 잘라먹을 수 있어 `content` 파싱을 별도로 처리하게 했다.
- 도구 오류가 있으면 검증 명령이 통과해도 보고서 verdict를 `CONTINUE`로 남긴다.
- 쓰기 도구가 성공하면 추가 모델 턴을 쓰지 않고 즉시 루프를 종료한다.
- 컴파일 검증만으로는 런타임 도구 결함을 못 잡으므로 `--validate`에 `--tool-self-test`를 포함했다.
- 설치되지 않은 `flake8`, `pylint`, `unittest` 경로를 검증에 추가하면 루프가 멈춘다. 검증 명령은 현재 환경에서 확인된 것만 사용한다.
- f-string 내부 dict 접근에서 같은 quote를 중첩하면 구문 오류가 난다. 모델 프롬프트에 `data.get('key')`처럼 바깥 quote와 다른 quote를 쓰도록 명시했다.
- 위험 생성물은 `replace_function` 단계에서 차단한다. 예: 미확인 린터, YAML/JSON 요약 포맷 변경.
- 실패 job은 `job/failed`로 격리하고 다음 반복이 계속 진행되게 한다.
- 수동/사이클 검증은 이전 job의 `last_tool_results` 오류에 오염되지 않도록 분리한다.
- `--seed-from-failures`는 실패 target 자체를 그대로 재시도하지 않고 `parse_tool_calls`, `_reject_unsafe_generated_content`, `improve_function` 같은 복구 target으로 변환한다.
- 위험 패턴 차단은 너무 넓으면 정상 함수명까지 막으므로 `exec(`, `eval(`처럼 호출 패턴 중심으로 제한한다.
- 긴 코드 교체를 XML 속성에 넣으면 quote 충돌이 반복되므로 block content tool_call을 지원하고 프롬프트에서 우선 사용하게 했다.
- block content 안에 `<tool_call ... />` 문자열이 들어 있어도 중첩 도구 호출로 오인하지 않도록 self-test를 강화했다.
- 프롬프트 안에 CDATA 종료 토큰 예시를 넣으면 모델이 그대로 복사해 block content를 조기 종료시킬 수 있으므로, 설명에서는 CDATA 종료 토큰을 쓰지 않는다.
- Rich 출력은 `[path]` 같은 Python 리스트를 마크업으로 오인할 수 있으므로 모델 응답과 도구 결과는 markup 없이 출력한다.
- 자동 발견 seed는 `restart`, `signal_handler`처럼 실행 제어 성격의 함수는 제외한다.
- 생성물이 `print(`, 미정의 로그 헬퍼, 생성 실패를 문자열로 숨기는 패턴을 만들면 쓰기 단계에서 차단한다.
