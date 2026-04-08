#!/usr/bin/env bash
# PSP USBデプロイ — PowerShell経由 + バックアップ
set -e

BACKUP_DIR="/home/kan82/pmd_psp/backups"
mkdir -p "$BACKUP_DIR"

# バックアップ: タイムスタンプ付きEBOOT + main.c
TS=$(date +%Y%m%d_%H%M%S)
cp EBOOT.PBP "$BACKUP_DIR/EBOOT_${TS}.PBP"
cp main.c "$BACKUP_DIR/main_${TS}.c"

# 古いバックアップ削除 (10世代保持)
ls -t "$BACKUP_DIR"/EBOOT_*.PBP 2>/dev/null | tail -n +11 | xargs -r rm -f
ls -t "$BACKUP_DIR"/main_*.c 2>/dev/null | tail -n +11 | xargs -r rm -f

echo "Backup: EBOOT_${TS}.PBP + main_${TS}.c"

# デプロイ (E:=PSP-1000, G:=PSP Go 両方試行)
/mnt/c/WINDOWS/System32/WindowsPowerShell/v1.0/powershell.exe -Command "
\$src = '\\\\wsl.localhost\\Ubuntu-24.04\\home\\kan82\\pmd_psp\\EBOOT.PBP'
\$ok = 0
foreach (\$d in @('E','G')) {
    for (\$i=0; \$i -lt 5; \$i++) {
        if (Test-Path \"\$(\$d):\\\") { break }
        Start-Sleep -Seconds 1
    }
    if (Test-Path \"\$(\$d):\\\") {
        Copy-Item \$src \"\$(\$d):\\PSP\\GAME\\PMDVIS\\EBOOT.PBP\" -Force
        Write-Host \"Deploy OK: \$(\$d):\"
        \$ok++
    }
}
if (\$ok -eq 0) { Write-Host 'Deploy FAILED: no drive found'; exit 1 }
"
echo "Deploy done."
