# nodeinfo Field Guide

`nodeinfo` contains per-node inventory and WMI data. Unlike `sprocess`, the
actual inventory rows are stored in the `data[]` array, and the top-level `name`
field tells which inventory type the document contains.

## Top-Level Fields

- `id`: Orange node or managed endpoint identifier. This can be joined with
  `sprocess.id`.
- `name`: inventory category such as `OS`, `CPU`, `MEMORY`, `UNINSTALL`,
  `VACCINE`, `NETWORK`, `AGENT`.
- `label`: display label.
- `command`: source command type.
- `count`: collection count.
- `firsttime`, `lasttime`: first/last observed time.

## Common Categories

- `OS`: Windows OS information.
  - `data.CSName`: computer name.
  - `data.Caption`: OS caption, e.g. Microsoft Windows 10 Pro.
  - `data.BuildNumber`: Windows build number.
  - `data.OSArchitecture`: OS architecture.
- `CPU`: CPU information.
  - `data.Name`: CPU model.
  - `data.NumberOfCores`: physical/logical core count reported by WMI.
  - `data.NumberOfLogicalProcessors`: logical processors.
  - `data.MaxClockSpeed`: max clock MHz.
- `MEMORY`: memory module information.
  - `data.Capacity`: module capacity string such as `4G`, `8G`.
  - `data.BankLabel`: memory bank/slot.
  - `data.Speed`: memory speed when available.
- `UNINSTALL`: installed program inventory.
  - `data.name`, `data.version`, `data.publisher`, `data.installLocation`,
    `data.installKey`, `data.installedTime`, `data.installSource`,
    `data.uninstallString`, `data.Path`.
- `VACCINE`: security/antivirus product inventory.
  - `data.displayName`, `data.Signature`, `data.Status`,
    `data.FileCountersPath`, `data.FileCounters.ProcPath`.
- `AGENT`: Orange Agent installation and Agent-side counters.
  - `data.version`, `data.revision`, `data.installTime`,
    `data.TotalCounters`, `data.FileCounters`.

## Natural Language Mapping

- "윈도 10/11 장비": filter `name == OS` and `data.Caption regex Windows 10/11`.
- "CPU 코어 낮은 장비": filter `name == CPU`, group by `id`, `data.Name`,
  sort by `max(data.NumberOfCores)` ascending.
- "메모리 용량 구성": filter `name == MEMORY`, group by `id`, `data.Capacity`.
- "설치된 프로그램": filter `name == UNINSTALL`, group by
  `data.name`, `data.version`, `data.publisher`.
- "백신 제품": filter `name == VACCINE`, group by `id`, `data.displayName`,
  `data.Signature`, `data.Status`.

## Joining With sprocess

Use virtual collection `sprocess_nodeinfo` when process metrics must be filtered
or grouped by node inventory:

- "윈도 10 장비에서 부하 높은 프로세스": filter `node.OSCaption regex Windows 10`,
  group by `id`, `node.CSName`, `node.OSCaption`, `ProcName`.
- "CPU 코어 2 이하 장비에서 부하 높은 프로세스": filter `node.Cores <= 2`.
- "윈도 10 장비에서 PowerShell 명령줄": filter `node.OSCaption regex Windows 10`
  and `ProcName regex powershell`, group by `Command`.
