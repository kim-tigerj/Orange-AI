# bump_build.ps1 — Release Pre-Build 시 APP_VER_BUILD 를 1 증가시킨다.
# 사용: powershell -File tools\bump_build.ps1 <path\to\resource.h>
param([string]$ResourceFile)

if (-not $ResourceFile) { $ResourceFile = "$PSScriptRoot\..\resource.h" }
$ResourceFile = [IO.Path]::GetFullPath($ResourceFile)

$lines = [IO.File]::ReadAllLines($ResourceFile, [Text.Encoding]::UTF8)
$build = 0

for ($i = 0; $i -lt $lines.Length; $i++) {
    if ($lines[$i] -match '^#define APP_VER_BUILD\s+(\d+)') {
        $build = [int]$Matches[1] + 1
        $lines[$i] = "#define APP_VER_BUILD  $build"
    }
}
for ($i = 0; $i -lt $lines.Length; $i++) {
    if ($lines[$i] -match '^#define APP_VER_STR') {
        $lines[$i] = '#define APP_VER_STR   "0.1.0.' + $build + '"'
    } elseif ($lines[$i] -match '^#define APP_VER_WSTR') {
        $lines[$i] = '#define APP_VER_WSTR  L"0.1.0.' + $build + '"'
    }
}

[IO.File]::WriteAllLines($ResourceFile, $lines, [Text.Encoding]::UTF8)
Write-Host "OrangeCode build number bumped to $build"
