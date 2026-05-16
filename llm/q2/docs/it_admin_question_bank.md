# q2 IT Admin Question Bank

This bank is for autonomous q2 testing. It should contain realistic IT administrator questions, not random Korean sentence combinations.

## External Patterns Used

- Endpoint inventory: installed applications, software versions, running processes, hardware/software assets.
- Vulnerability and software visibility: installed software, versions, exposed/vulnerable devices.
- Operations dashboards: highest resource consumers, active alerts, CPU/memory/disk/process symptoms.
- Endpoint management actions: patching, software install/update/remove, command execution status.
- Security operations: unsigned files/processes, suspicious PowerShell, file change traces, threat/event distribution.

## Orange Mapping

- Asset inventory: `node`, `nodeinfo`
- Software inventory: `nodeinfo`, `filelist`, `sprocess`
- Endpoint health: `node`, `system`
- Process performance: `sprocess`
- Process/file integrity: `sprocess`, `filelist`
- Event triage: `detect`, `report`, `timeline`
- 1:N operations: `command_template`, `command`
- People/device questions: `user`, `group`, `node`, investigation endpoint

## Current Curated Categories

- `asset-inventory`
- `software-inventory`
- `endpoint-health`
- `process-performance`
- `process-integrity`
- `file-change`
- `event-triage`
- `timeline`
- `operations-command`
- `people`

Run:

```bash
./venv/bin/python llm/q2/q2_autotest.py --server http://127.0.0.1:3184
```

The runner reports live progress through the q2 API, and the API persists it in MongoDB. The web `Autotest` tab reads from MongoDB through `/api/autotest/status`.

Controlled variants are disabled by default:

```bash
./venv/bin/python llm/q2/q2_autotest.py --generated 30
```

Only promote variants into the curated bank when they represent a question an IT administrator would plausibly ask repeatedly.
