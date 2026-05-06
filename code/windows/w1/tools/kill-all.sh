#!/usr/bin/env bash
# tools/kill-all.sh ???좎엳??紐⑤뱺 OrangeCode ?몄뒪?댁뒪 媛뺤젣 醫낅즺.
#
# 媛쒕컻 以??먭린-援먯껜 / spawn ?ш퀬濡??몄뒪?댁뒪媛 ?꾩쟻?????ъ슜.
# ?덉쟾 ??`OrangeLabs` 寃쎈줈 ???꾨줈?몄뒪留??≪쓬 (?ㅻⅨ Code.exe ? 異⑸룎 X).

set -eu

powershell.exe -NoProfile -Command "
Get-Process -Name OrangeCode,Code -ErrorAction SilentlyContinue |
    Where-Object { \$_.Path -like '*OrangeLabs*' } |
    ForEach-Object {
        Write-Host (\"kill: PID=\" + \$_.Id + \" Title='\" + \$_.MainWindowTitle + \"'\")
        Stop-Process -Id \$_.Id -Force
    }
Start-Sleep -Milliseconds 500
\$remain = Get-Process -Name OrangeCode,Code -ErrorAction SilentlyContinue |
    Where-Object { \$_.Path -like '*OrangeLabs*' }
if (\$remain) {
    Write-Host '???뺣━ ?꾩뿉???⑥? ?꾨줈?몄뒪:'
    \$remain | Select-Object Id,MainWindowTitle | Format-Table -AutoSize
} else {
    Write-Host '??紐⑤몢 ?뺣━??
}
"
