#!/usr/bin/env bash
# PSP USBデプロイ — PowerShell経由 + バックアップ
# パスは全てスクリプト自身の位置から解決 (ハードコード禁止)
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BACKUP_DIR="${SCRIPT_DIR}/backups"
EBOOT="${SCRIPT_DIR}/EBOOT.PBP"
MAIN_C="${SCRIPT_DIR}/main.c"
SONGS_DIR="${SCRIPT_DIR}/songs"

# WSLパス→UNCパス変換
WSL_DISTRO="Ubuntu-24.04"
EBOOT_UNC="\\\\wsl.localhost\\${WSL_DISTRO}${EBOOT}"
SONGS_UNC="\\\\wsl.localhost\\${WSL_DISTRO}${SONGS_DIR}"

echo "=== AUTO DEPLOY ==="
echo "Source: ${EBOOT}"

# EBOOTが存在しなければ即失敗
if [ ! -f "$EBOOT" ]; then
    echo "ERROR: ${EBOOT} not found. Build first."
    exit 1
fi

# EBOOTのサイズとタイムスタンプ表示 (デプロイ内容の検証用)
ls -la "$EBOOT"

mkdir -p "$BACKUP_DIR"

# バックアップ: タイムスタンプ付きEBOOT + main.c
TS=$(date +%Y%m%d_%H%M%S)
cp "$EBOOT" "$BACKUP_DIR/EBOOT_${TS}.PBP"
[ -f "$MAIN_C" ] && cp "$MAIN_C" "$BACKUP_DIR/main_${TS}.c"

# 古いバックアップ削除 (10世代保持)
ls -t "$BACKUP_DIR"/EBOOT_*.PBP 2>/dev/null | tail -n +11 | xargs -r rm -f
ls -t "$BACKUP_DIR"/main_*.c 2>/dev/null | tail -n +11 | xargs -r rm -f

echo "Backup: EBOOT_${TS}.PBP + main_${TS}.c"

# デプロイ (E:=PSP-1000, G:=PSP Go 両方試行)
/mnt/c/WINDOWS/System32/WindowsPowerShell/v1.0/powershell.exe -Command "
\$src = '${EBOOT_UNC}'
\$ok = 0
foreach (\$d in @('E','G')) {
    for (\$i=0; \$i -lt 5; \$i++) {
        if (Test-Path \"\$(\$d):\\\") { break }
        Start-Sleep -Seconds 1
    }
    if (Test-Path \"\$(\$d):\\\") {
        Copy-Item \$src \"\$(\$d):\\PSP\\GAME\\PMDVIS\\EBOOT.PBP\" -Force
        \$songsDir = \"\$(\$d):\\PSP\\GAME\\PMDVIS\\songs\"
        if (!(Test-Path \$songsDir)) { New-Item -ItemType Directory -Path \$songsDir -Force | Out-Null }
        \$songsSrc = '${SONGS_UNC}'
        Get-ChildItem \"\$songsSrc\\*.M\" -ErrorAction SilentlyContinue | ForEach-Object {
            Copy-Item \$_.FullName \"\$songsDir\\\$(\$_.Name)\" -Force
            Write-Host \"  Song: \$(\$_.Name)\"
        }
        Write-Host \"Deploy OK: \$(\$d):\"
        \$ok++
    }
}
if (\$ok -eq 0) { Write-Host 'Deploy FAILED: no drive found'; exit 1 }
"
echo "Deploy done."
