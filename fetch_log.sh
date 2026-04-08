#!/bin/bash
# fetch_log.sh — PSPからログ取得（/mnt/e/ 直接参照禁止）
# E:=PSP-1000, G:=PSP Go  両方からコピー試行
DEST="/home/kan82/pmd_psp"
WSL_PATH="\\\\wsl.localhost\\Ubuntu-24.04\\home\\kan82\\pmd_psp"
PS="/mnt/c/WINDOWS/System32/WindowsPowerShell/v1.0/powershell.exe"

for drv in E G; do
    for f in pmd_log.txt pmd_timing.txt pmd_midi_dbg.txt; do
        out="${f%.txt}_${drv}.txt"
        "$PS" -Command "if (Test-Path '${drv}:\\$f') { Copy-Item '${drv}:\\$f' '${WSL_PATH}\\${out}' -Force; Write-Host 'OK: ${drv}:/$f -> $out' } else { Write-Host 'SKIP: ${drv}:/$f' }"
    done
done
echo "=== fetch done ==="
