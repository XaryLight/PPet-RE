<#
.SYNOPSIS
PPet 本地代码与远程仓库一致性诊断（Windows / PowerShell）。

.DESCRIPTION
典型症状：CLion configure 时报一串
    CMake Error at CMakeLists.txt:195 (add_library): Cannot find source file: src/core/FileIO.cpp
    CMake Error: No SOURCES given to target: ppet_model
根因通常是本地只更新了部分文件 —— CMakeLists.txt 停在旧提交（引用
src/core/... 旧路径），而 src/ 目录已换成 34da636 起的新结构
（src/{ui,tools} + api/），两者对不上。

.EXAMPLE
powershell -ExecutionPolicy Bypass -File tools\check_sync.ps1
只诊断，输出问题与建议，不修改任何文件。

.EXAMPLE
powershell -ExecutionPolicy Bypass -File tools\check_sync.ps1 -Fix
先 stash 备份本地改动，再对齐 origin/main，并清理 CLion 构建缓存。
#>
[CmdletBinding()]
param(
    [switch]$Fix,
    [string]$BuildDir = "cmake-build-debug"
)

# 不用 Stop：PowerShell 5.1 在 ErrorActionPreference=Stop 下对 native 命令的
# stderr 重定向会抛 NativeCommandError，反而让诊断脚本自己中断。
$ErrorActionPreference = "Continue"
try {
    [Console]::OutputEncoding = [System.Text.Encoding]::UTF8
    $OutputEncoding = [System.Text.Encoding]::UTF8
} catch { }

function Write-Head($t) { Write-Host ""; Write-Host "=== $t ===" -ForegroundColor Cyan }
function Write-Ok($t)   { Write-Host "  [OK]   $t" -ForegroundColor Green }
function Write-Bad($t)  { Write-Host "  [问题] $t" -ForegroundColor Red }
function Write-Tip($t)  { Write-Host "  [提示] $t" -ForegroundColor Yellow }

$repo = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
Set-Location $repo
Write-Host "仓库: $repo"

if (-not (Get-Command git -ErrorAction SilentlyContinue)) {
    Write-Bad "未找到 git，请安装 Git for Windows 并加入 PATH。"
    exit 1
}
if (-not (Test-Path (Join-Path $repo ".git"))) {
    Write-Bad "当前目录不是 git 仓库（没有 .git）。若源码是 zip 解压得到的，请改用 git clone，或重新解压完整的新版源码包（所有文件一起覆盖）。"
    exit 1
}

Write-Head "1. 拉取远程信息"
git fetch origin main --quiet 2>$null
if ($LASTEXITCODE -ne 0) { Write-Tip "git fetch 失败（离线或权限问题），将用已有的本地远程引用继续诊断。" }
$remoteRev = (git rev-parse --short origin/main 2>$null)
$localRev  = (git rev-parse --short HEAD 2>$null)
if (-not $remoteRev) {
    Write-Bad "取不到 origin/main。请确认 remote 配置：git remote -v"
    exit 1
}
Write-Host "  本地 HEAD   : $localRev"
Write-Host "  origin/main : $remoteRev"
if ($localRev -eq $remoteRev) { Write-Ok "本地 HEAD 已与远程一致" }
else { Write-Bad "本地 HEAD 落后/偏离 origin/main" }

Write-Head "2. 关键文件与目录"
$issues = @()

# 直接命中「旧版 CMakeLists」这一症状：本地文件里是否还引用旧源码路径
$cmLocal = Join-Path $repo "CMakeLists.txt"
$oldRefs = @(Select-String -Path $cmLocal -Pattern 'src/(core|model|platform|render|app)/[A-Za-z0-9_]+\.(cpp|hpp)' -AllMatches -ErrorAction SilentlyContinue)
if ($oldRefs.Count -gt 0) {
    Write-Bad "本地 CMakeLists.txt 仍引用旧路径 src/core|model|platform|render|app/（$($oldRefs.Count) 处）—— 这是 34da636 之前的旧版文件"
    $oldRefs | Select-Object -First 3 | ForEach-Object { Write-Host "        L$($_.LineNumber): $($_.Line.Trim())" }
    $issues += "oldrefs"
} else { Write-Ok "本地 CMakeLists.txt 未引用旧源码路径" }

git diff --quiet origin/main -- CMakeLists.txt 2>$null
if ($LASTEXITCODE -eq 0) { Write-Ok "CMakeLists.txt 与 origin/main 一致" }
else { Write-Bad "CMakeLists.txt 与 origin/main 不一致（本地改过或版本落后）"; $issues += "cmake" }

$hasNew = Test-Path (Join-Path $repo "src\tools\core\Log.cpp")
$hasOld = (Test-Path (Join-Path $repo "src\core")) -or (Test-Path (Join-Path $repo "src\model")) -or (Test-Path (Join-Path $repo "src\platform"))
if ($hasNew) { Write-Ok "源码是新版结构 src/tools/（34da636+）" }
else { Write-Bad "缺少 src/tools/core/Log.cpp，源码目录不是新版结构"; $issues += "layout" }
if ($hasOld) { Write-Tip "存在旧目录 src/core|model|platform（残留，不参与构建；确认无本地文件后可删）" }

$diffFiles = @(git diff --name-only HEAD origin/main 2>$null)
if ($diffFiles.Count -gt 0) {
    Write-Bad "共 $($diffFiles.Count) 个文件与 origin/main 不同（前 12 个）："
    $diffFiles | Select-Object -First 12 | ForEach-Object { Write-Host "        $_" }
    if ($diffFiles.Count -gt 12) { Write-Host "        ... 其余 $($diffFiles.Count - 12) 个" }
    $issues += "behind"
} else { Write-Ok "HEAD 文件树与 origin/main 完全一致" }

$dirty = @(git status --porcelain 2>$null)
if ($dirty.Count -gt 0) {
    Write-Tip "工作区有 $($dirty.Count) 个未提交改动（可能包含你手改的 CMakeLists.txt，这也是 git pull 没生效的常见原因）："
    $dirty | Select-Object -First 8 | ForEach-Object { Write-Host "        $_" }
}

Write-Head "3. 结论"
if ($issues.Count -eq 0) {
    Write-Ok "未发现版本不同步。若 configure 仍报 Cannot find source file，删除 $BuildDir 后在 CLion 里 Reload CMake Project 即可。"
    exit 0
}
Write-Bad "本地代码与仓库不一致 —— 这就是 configure 失败的原因。"
if ($issues -contains "oldrefs") {
    Write-Tip "旧版 CMakeLists.txt 引用 src/core/... 等旧路径，而 src/ 已是 src/tools/... 新结构，两者版本对不上（本次日志正是这种）。"
} else {
    Write-Tip "CMakeLists.txt 或源码目录与 origin/main 不一致，请对齐后重新 configure。"
}

Write-Head "4. 修复"
if (-not $Fix) {
    Write-Host "  最小修复（只同步构建脚本 + 清缓存，保留其他本地改动）：" -ForegroundColor Yellow
    Write-Host "    git checkout origin/main -- CMakeLists.txt"
    Write-Host "    Remove-Item -Recurse -Force $BuildDir"
    Write-Host ""
    Write-Host "  彻底修复（先 stash 备份所有本地改动，再整体对齐远程）：" -ForegroundColor Yellow
    Write-Host "    powershell -ExecutionPolicy Bypass -File tools\check_sync.ps1 -Fix"
    Write-Host "  等价于：git fetch origin; git stash push -m ppet-backup; git reset --hard origin/main; Remove-Item -Recurse -Force $BuildDir"
    Write-Host ""
    Write-Tip "完成后在 CLion 中 Reload CMake Project（或重新 configure）。"
    Write-Tip "新版已支持 -DPPET_WITH_CUBISM=OFF 真正生效，无需再手改 CMakeLists.txt。"
    exit 0
}

$stamp = Get-Date -Format "yyyyMMdd-HHmmss"
Write-Host "  [1/4] 备份本地改动到 stash ..."
git stash push -m "ppet-check-sync-$stamp" 2>&1 | ForEach-Object { Write-Host "        $_" }
Write-Ok "可用 git stash list 查看；需要时用 git stash pop 恢复。"

Write-Host "  [2/4] 对齐 origin/main ..."
git reset --hard origin/main
if ($LASTEXITCODE -ne 0) { Write-Bad "reset 失败，请用 git status 检查仓库状态。"; exit 1 }
Write-Ok "已对齐：$(git log --oneline -1)"

Write-Host "  [3/4] 清理 CLion 构建缓存 ..."
$bd = Join-Path $repo $BuildDir
if (Test-Path $bd) { Remove-Item -Recurse -Force $bd; Write-Ok "已删除 $bd" }
else { Write-Ok "无 $bd，无需清理" }

Write-Host "  [4/4] 复核 ..."
git diff --quiet origin/main -- CMakeLists.txt 2>$null
if ($LASTEXITCODE -eq 0) { Write-Ok "CMakeLists.txt 已与 origin/main 一致" }
else { Write-Bad "CMakeLists.txt 仍不一致，请手动检查。" }
if (Test-Path (Join-Path $repo "src\tools\core\Log.cpp")) { Write-Ok "新版源码结构就位" }
else { Write-Bad "src/tools/core/Log.cpp 仍缺失，请确认源码包完整。" }

Write-Host ""
Write-Ok "完成。请在 CLion 中 Reload CMake Project（或删除 $BuildDir 后重新 configure）。"
Write-Tip "注意：新版已支持 -DPPET_WITH_CUBISM=OFF 真正生效，无需再手改 CMakeLists.txt。"
