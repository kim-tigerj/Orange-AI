# Orange Natural Language Mongo Planner

OR-1343 PoC area for converting Korean operational questions into safe MongoDB
query plans for Orange data.

The first target is Codex, not a local LLM. Local LLM support comes later after
Codex establishes a quality baseline.

## First Goal

```text
Korean natural language
  -> understand Orange `system` / `sprocess`
  -> produce a structured Mongo query plan
  -> validate the plan
  -> build a Mongo aggregation pipeline
```

The model does not execute MongoDB. It only emits a plan. Server-side code must
validate and execute the plan.

## Run

From the `Orange-AI` root:

```bash
./q2_api.sh
```

The API and test web page listen on `0.0.0.0:3184`.

- Web UI: `http://127.0.0.1:3184/`
- Health: `http://127.0.0.1:3184/health`
- Plan API: `POST /api/mongo-plan`
- Query API: `POST /api/mongo-query`

CLI test client:

```bash
./q2_cli.sh health
./q2_cli.sh plan "우리 회사에서 제일 부하 많은 프로세스를 최근 일주일간 요일별로 뽑아줘"
./q2_cli.sh query "최근 일주일간 메모리 사용률 추이를 보여줘"
./venv/bin/python llm/q2/q2_eval.py
```

Mongo execution uses environment variables:

```bash
ORANGE_MONGO_URI="mongodb://..." ./q2_api.sh
```

Codex fallback is enabled by default in `q2_api.sh`; template and cache hits
return before Codex is called. Override it when needed:

```bash
Q2_CODEX_ENABLED=0 ./q2_api.sh
Q2_CODEX_ENABLED=1 Q2_CODEX_WORKERS=2 Q2_CODEX_TIMEOUT=180 ./q2_api.sh
```

The planner uses an exact normalized cache persisted at
`llm/q2/output/plan_cache.json` by default. The cache key includes question,
schema version, planner version, and timezone.

## Collections

### `system`

Fleet or node-group performance aggregate over time.

Important fields:

- `time`: Unix seconds bucket time.
- `CPU.open`, `CPU.high`, `CPU.low`, `CPU.close`, `CPU.rate`, `CPU.unit`
- `Memory.open`, `Memory.high`, `Memory.low`, `Memory.close`, `Memory.rate`,
  `Memory.total`, `Memory.value`, `Memory.unit`
- `IO.open`, `IO.high`, `IO.low`, `IO.close`, `IO.value`, `IO.unit`
- `Handle.open`, `Handle.high`, `Handle.low`, `Handle.close`, `Handle.value`,
  `Handle.unit`
- `Process.crash`, `Process.crash2`, `Process.running`, `Process.start`,
  `Process.stop`
- `Stat.detect`, `Stat.detect2`, `Stat.install`, `Stat.sprocess`,
  `Stat.timeline`
- `health`, `h0`, `h1`, `h2`, `h3`, `total`, `online`, `offline`

Use this collection for fleet-wide questions:

- 전체 CPU/Memory/IO/Handle 추이
- 최근 며칠 대비 성능 변화
- health/online/offline 변화
- crash/detect 증가

### `sprocess`

Per-process performance data collected by the Agent.

Important fields:

- `time`, `timestamp`, `hour`, `lasttime`
- `id`, `ticket`
- `ProcName`, `ProcPath`, `Command`
- `PProcName`, `PProcPath`
- `CPU`, `Memory`, `IO`, `Handle`, `CounterCount`, `pscore`
- `Crash`, `Crashed`, `Kill`, `Killed`, `Detect`
- `Running`, `Start`, `Stop`
- `CompanyName`, `ProductName`, `ProductVersion`
- `FileDescription`, `FileName`, `FilePath`, `FileVersion`
- `Signer`, `Codesign`, `IsSystem`

Use this collection for process-level questions:

- 부하 많은 프로세스
- 요일/시간대별 프로세스 부하
- 제품/회사별 리소스 사용량
- 회사명/제품명/제품버전/파일설명/파일버전별 실행 파일 분포
- 실행/종료/충돌/탐지 많은 프로세스

## Query Plan Contract

The model must output only JSON:

```json
{
  "plan_type": "mongo_query",
  "collection": "sprocess",
  "time_range": {
    "type": "relative",
    "days": 7,
    "timezone": "Asia/Seoul"
  },
  "group_by": ["weekday", "ProcName"],
  "metrics": [
    {"name": "cpu_sum", "op": "sum", "field": "CPU"},
    {"name": "pscore_sum", "op": "sum", "field": "pscore"}
  ],
  "sort": [{"field": "pscore_sum", "direction": "desc"}],
  "limit": 100,
  "limit_per_group": 10,
  "output": "table",
  "notes": []
}
```

## Codex Planner Prompt

The Codex backend prompt lives in:

```text
llm/q2/prompts/system_sprocess_planner.md
```

Update that file when q2 learns Orange-specific domain meaning such as
`CounterCount`, `pscore`, or Windows file version metadata.

The current `sprocess` field guide lives in:

```text
llm/q2/docs/sprocess_field_guide.md
```

The current `nodeinfo` field guide lives in:

```text
llm/q2/docs/nodeinfo_field_guide.md
```

## Korean Term Mapping

- 부하: prefer `pscore`; if unclear, include CPU/Memory/IO/Handle together.
- `sprocess.CounterCount`: Agent가 해당 프로세스를 측정한 샘플 수.
- `sprocess.CPU`: 100% 최대 기준 평균 CPU 사용률을 구하기 위한 누적값.
- `sprocess.Memory`: MB 단위 평균 메모리 값을 구하기 위한 누적값.
- `sprocess.IO`: MB/s 단위 평균 IO 값을 구하기 위한 누적값.
- `sprocess.Handle`: 핸들 개수 평균을 구하기 위한 누적값.
- `sprocess.pscore`: CPU, Memory, IO, Handle, 동작시간을 포함한 Orange 고유
  포괄 부하지수의 누적값.
- `sprocess`의 pscore/CPU/Memory/IO/Handle 평균: `sum(field) / sum(CounterCount)`.
- `CompanyName`, `ProductName`, `ProductVersion`, `FileDescription`, `FileVersion`:
  Windows 실행 파일 리소스의 버전 정보. 회사/벤더, 제품, 제품 버전, 파일 설명,
  파일 버전 질문에 사용한다.
- CPU: `CPU`
- 메모리: `Memory` in `sprocess`, `Memory.rate` in `system`
- IO, 디스크, 네트워크 사용량: `IO`
- 핸들: `Handle`
- 충돌, 크래시: `Crash`, `Crashed`, or `Process.crash`
- 탐지, 증상 수: `Detect` or `Stat.detect`
- 실행: `Start`, `Running`
- 종료: `Stop`
- 최근 일주일: relative 7 days
- 요일별: derive weekday from `time` or `timestamp`
- 시간대별: derive hour from `time` or `timestamp`

## Safety Rules

- Only read-only Mongo queries are allowed.
- Only allowlisted collections and fields may be referenced.
- Always include a bounded time range unless the user explicitly asks for a
  static schema summary.
- Always include a limit.
- Prefer query plans over raw aggregation JSON. Aggregation is built by server
  code after validation.

## `avg_by_count`

For `sprocess`, the Agent stores sampled cumulative values with `CounterCount`.
Use `avg_by_count` for process-level average values:

```json
{"name": "cpu_avg", "op": "avg_by_count", "field": "CPU"}
{"name": "pscore_avg", "op": "avg_by_count", "field": "pscore"}
```

The server builds this as:

```text
sum(CPU) / sum(CounterCount)
```
