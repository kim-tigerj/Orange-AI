#!/bin/bash
# Orange Code Release x64 build helper.
#
# Move existing Release outputs to unique .old names before MSBuild runs.
# This avoids repeated self-build failures when a previous fixed .old target
# already exists.

set -e
cd "$(dirname "$0")/.."

backup_release_output() {
    local src="$1"
    [ -e "$src" ] || return 0

    local stamp
    stamp="$(date +%Y%m%d-%H%M%S)-pid$$"
    local i=0
    local dst
    while [ "$i" -lt 100 ]; do
        dst="${src}.${stamp}"
        [ "$i" -eq 0 ] || dst="${dst}-${i}"
        dst="${dst}.old"
        if [ ! -e "$dst" ]; then
            mv "$src" "$dst"
            echo "[build prep] moved $src -> $dst"
            return 0
        fi
        i=$((i + 1))
    done

    echo "[build prep] no unique backup name for $src" >&2
    return 1
}

backup_release_output bin/Release/OrangeCode.exe
backup_release_output bin/Release/OrangeCode.pdb
backup_release_output bin/Release/Code.exe
backup_release_output bin/Release/Code.pdb

MSBUILD="/c/Program Files/Microsoft Visual Studio/2022/Professional/MSBuild/Current/Bin/MSBuild.exe"
if [ ! -x "$MSBUILD" ]; then
    for ed in Community Enterprise BuildTools; do
        cand="/c/Program Files/Microsoft Visual Studio/2022/$ed/MSBuild/Current/Bin/MSBuild.exe"
        if [ -x "$cand" ]; then MSBUILD="$cand"; break; fi
    done
fi

"$MSBUILD" Code.sln -p:Configuration=Release -p:Platform=x64 -m -v:minimal "$@"
