# sprocess Field Guide

This guide is derived from live `public.sprocess` data profiling.

## Identity

- `id`: Orange node or managed endpoint identifier.
- `ticket`: collection/session identifier for Agent data.
- `ProcName`: process executable name, e.g. `svchost.exe`, `powershell.exe`.
- `ProcPath`: process executable path.
- `Command`: command line or launch command.
- `PProcName`: parent process name.
- `PProcPath`: parent process path.

Use these fields for questions about process names, executable paths, command
lines, and parent-child process relationships.

## Time

- `timestamp`: primary time field in milliseconds.
- `time`: Unix seconds-like bucket field.
- `hour`: hourly bucket as datetime.
- `lasttime`: last observed time, mixed string/datetime in current data.

Use `timestamp` for q2 time range matching.

## Performance

- `CounterCount`: Agent measurement sample count.
- `pscore`: Orange proprietary comprehensive process load index. It includes
  CPU, Memory, IO, Handle, and process running/active time signals.
- `CPU`: cumulative CPU value. Average unit is percent.
- `Memory`: cumulative memory value. Average unit is MB.
- `IO`: cumulative IO value. Average unit is MB/s.
- `Handle`: cumulative handle value. Average unit is count.

Average values must be computed as:

```text
sum(field) / sum(CounterCount)
```

Use `avg_by_count` for `pscore`, `CPU`, `Memory`, `IO`, and `Handle`.

When a user says "부하", "부하지수", "로드", or "load", prefer `pscore_avg`
as the primary metric. CPU/Memory/IO/Handle averages are supporting detail.

## Lifecycle And Events

- `Running`: 1 when the process is running, 0 otherwise.
- `Start`: process start count.
- `Stop`: process stop count.
- `Crash`: crash count. Most values are 0; non-zero values are important.
- `Crashed`: currently always 0 in profiled data.
- `Kill`, `Killed`: currently always 0 in profiled data.
- `Detect`: detection/symptom count. Mostly 0, but outliers exist.

Use these for questions about frequently starting/stopping processes, currently
running process distribution, crashes, and detections.

## Windows File Version Metadata

These fields come from Windows executable file version resources:

- `CompanyName`: company/vendor, e.g. Microsoft Corporation, Google LLC.
- `ProductName`: product name, e.g. Microsoft Windows Operating System.
- `ProductVersion`: product version.
- `FileDescription`: executable file description.
- `FileName`: executable file name from metadata.
- `FilePath`: executable path from metadata.
- `FileVersion`: file version.

Use these for questions about vendor/product/version/file description
distribution and for grouping executable families that have the same process
name but different versions.

## Signature Metadata

- `Signer`: signer common name or publisher label.
- `Codesign`: full certificate subject.
- `IsSystem`: 1 for system files, 0 for non-system files.

Use these for questions about signed/unsigned or system/non-system process
distribution. `Codesign` is verbose; prefer `Signer` for grouping unless the
user explicitly asks for certificate details.

## Common Operator Questions

- "서명 없는 프로세스": filter empty `Signer` or empty `Codesign`.
- "비시스템 파일": filter `IsSystem == 0`.
- "시스템 파일만": filter `IsSystem == 1`.
- "PowerShell 명령줄": filter `ProcName` with case-insensitive regex `powershell`,
  group by `ProcName`, `Command`.
- "탐지 있는 프로세스": filter `Detect > 0`.
- "크래시 발생 프로세스": filter `Crash > 0`.
- "특정 회사/제품/버전": filter or group by `CompanyName`, `ProductName`,
  `ProductVersion`, `FileVersion`.
- "노드별", "장비별": group by `id`.
- "수집 세션별", "ticket별": group by `ticket`.
