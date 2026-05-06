@echo off
setlocal enableextensions enabledelayedexpansion

rem ============================================================
rem  Orange Code 자기-격발 빌드 스크립트
rem
rem  사용:
rem    rebuild.bat <session_id> <bootstrap_message> <parent_pid>
rem
rem  흐름:
rem    1) MSBuild Release x64 빌드
rem    2) bin\Release\Code.exe 를 새 인자로 spawn
rem    3) 새 인스턴스가 부팅 시 --replace-pid 로 부모 종료
rem
rem  안전: 빌드 실패 → 새 인스턴스 안 뜸 → 루프 자연 정지.
rem ============================================================

set "ROOT=%~dp0"
cd /d "%ROOT%"

rem ── MSBuild 찾기 (PATH 에 없으면 vswhere) ──
set "MSBUILD="
where /q msbuild
if !errorlevel! equ 0 (
    set "MSBUILD=msbuild"
) else (
    set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
    if exist "!VSWHERE!" (
        for /f "usebackq tokens=*" %%i in (`"!VSWHERE!" -latest -requires Microsoft.Component.MSBuild -find MSBuild\**\Bin\MSBuild.exe`) do (
            set "MSBUILD=%%i"
        )
    )
)

if "!MSBUILD!"=="" (
    echo [rebuild] msbuild not found.
    exit /b 1
)

echo [rebuild] msbuild: !MSBUILD!

rem ── APP_VER_BUILD 자동 증가는 Code.vcxproj PreBuildEvent(tools\bump_build.ps1)가 담당 ──

"!MSBUILD!" Code.sln /p:Configuration=Release /p:Platform=x64 /m /nologo /verbosity:minimal
if !errorlevel! neq 0 (
    echo [rebuild] build failed.
    exit /b 1
)

set "EXE=!ROOT!bin\Release\OrangeCode.exe"
if not exist "!EXE!" (
    echo [rebuild] !EXE! not found.
    exit /b 1
)

set "SESSION=%~1"
set "BOOTSTRAP=%~2"
set "PARENT_PID=%~3"

echo [rebuild] spawn (session=!SESSION!, parent=!PARENT_PID!)
start "" "!EXE!" --session "!SESSION!" --bootstrap "!BOOTSTRAP!" --replace-pid "!PARENT_PID!"
exit /b 0
