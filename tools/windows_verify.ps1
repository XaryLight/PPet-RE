<#
.SYNOPSIS
    PPet Windows 11 真机验收（T1）：全编译 -> 试运行 -> 三类模型加载 -> 渲染截图 -> JSON 报告。

.DESCRIPTION
    面向的环境：
      Windows 11 + NVIDIA 显卡 + llvm-mingw 2026 ucrt + Qt 6.x (llvm-mingw_64)
      + Cubism SDK for Native 5.x（启用 Live2D 后端）。

    脚本做五件事：
      1. configure：C++23 / Qt6 / PPET_WITH_CUBISM=ON
      2. build：全量编译（ppet.exe 与 ppet_render_verify.exe）
      3. windeployqt：部署 Qt 运行时与平台插件
      4. 试运行：启动 ppet.exe 数秒，检查是否闪退（0xC0000135 等）与 last.log 有无 FATAL
      5. 渲染验证：对 VRM / PMX(.zip) / moc3 各跑一次 ppet_render_verify.exe，
         产出 PNG 截图 + JSON 报告，最后汇总成 summary.json

    详细手册见 docs/windows-test.md。

.EXAMPLE
    .\tools\windows_verify.ps1 `
        -QtPrefix "C:/Qt/6.7.2/llvm-mingw_64/lib/cmake" `
        -Vrm "D:/models/test.vrm" `
        -PmxZip "D:/models/Model.zip" `
        -Moc3 "D:/models/Hiyori/Hiyori.model3.json"

.NOTES
    脚本不会自动下载任何工具链 / SDK / 模型，缺什么会明确报出来。
    在 Linux 沙箱里无法运行本脚本；仓库里的 docs/verify/ 是 Linux 离屏验证的等价证据。
#>
param(
    [string]$QtPrefix = "",
    [string]$CubismRoot = "",
    [string]$Toolchain = "",
    [string]$Generator = "Ninja",
    [string]$BuildDir = "cmake-build-verify",
    [string]$Vrm = "",
    [string]$PmxZip = "",
    [string]$Moc3 = "",
    [string]$OutDir = "verify_report",
    [int]$StartupWaitSeconds = 6,
    [switch]$SkipBuild
)

$ErrorActionPreference = "Stop"
$Root = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$BuildFull = Join-Path $Root $BuildDir
$OutFull = Join-Path $Root $OutDir
New-Item -ItemType Directory -Force -Path $OutFull | Out-Null

function Write-Step([string]$text) {
    Write-Host ""
    Write-Host ("=" * 78) -ForegroundColor Cyan
    Write-Host "  $text" -ForegroundColor Cyan
    Write-Host ("=" * 78) -ForegroundColor Cyan
}
function Write-Ok([string]$text) { Write-Host "  [ OK ] $text" -ForegroundColor Green }
function Write-Warn([string]$text) { Write-Host "  [WARN] $text" -ForegroundColor Yellow }
function Write-Fail([string]$text) { Write-Host "  [FAIL] $text" -ForegroundColor Red }

$summary = [ordered]@{
    tool = "ppet_windows_verify"
    generated_at = (Get-Date).ToString("s")
    os = [System.Environment]::OSVersion.VersionString
    steps = [ordered]@{}
    models = @()
    overall = "PENDING"
}

# ---------------------------------------------------------------------------
# 0) 工具检查
# ---------------------------------------------------------------------------
Write-Step "0/5 环境检查"
foreach ($tool in @("cmake", "ninja", "clang++")) {
    $cmd = Get-Command $tool -ErrorAction SilentlyContinue
    if ($cmd) { Write-Ok "$tool -> $($cmd.Source)" }
    else { Write-Warn "$tool 不在 PATH（若用 CLion 自带工具链可忽略）" }
}
if ($QtPrefix -ne "" -and -not (Test-Path $QtPrefix)) {
    Write-Fail "QtPrefix 不存在：$QtPrefix"
    exit 1
}
if ($CubismRoot -ne "" -and
    -not (Test-Path (Join-Path $CubismRoot "include/Live2DCubismCore.h"))) {
    Write-Fail "CubismRoot 下没有 include/Live2DCubismCore.h：$CubismRoot"
    exit 1
}

# ---------------------------------------------------------------------------
# 1) configure
# ---------------------------------------------------------------------------
if (-not $SkipBuild) {
    Write-Step "1/5 CMake configure（C++23 / Qt6 / Live2D SDK 5.x）"
    $cmakeArgs = @(
        "-S", $Root,
        "-B", $BuildFull,
        "-G", $Generator,
        "-DCMAKE_BUILD_TYPE=Release",
        "-DPPET_WITH_CUBISM=ON"
    )
    if ($Toolchain -ne "") { $cmakeArgs += "-DCMAKE_TOOLCHAIN_FILE=$Toolchain" }
    if ($QtPrefix -ne "") { $cmakeArgs += "-DCMAKE_PREFIX_PATH=$QtPrefix" }
    if ($CubismRoot -ne "") { $cmakeArgs += "-DPPET_CUBISM_ROOT=$CubismRoot" }
    & cmake @cmakeArgs
    if ($LASTEXITCODE -ne 0) {
        $summary.steps.configure = "FAIL"
        Write-Fail "configure 失败（检查 CMake >= 3.20、Qt6 路径、工具链）"
        $summary.overall = "FAIL"
        $summary | ConvertTo-Json -Depth 8 | Set-Content (Join-Path $OutFull "summary.json") -Encoding UTF8
        exit 1
    }
    $summary.steps.configure = "PASS"
    Write-Ok "configure 完成"

    # -----------------------------------------------------------------------
    # 2) build
    # -----------------------------------------------------------------------
    Write-Step "2/5 全量编译"
    & cmake --build $BuildFull --parallel
    if ($LASTEXITCODE -ne 0) {
        $summary.steps.build = "FAIL"
        Write-Fail "编译失败"
        $summary.overall = "FAIL"
        $summary | ConvertTo-Json -Depth 8 | Set-Content (Join-Path $OutFull "summary.json") -Encoding UTF8
        exit 1
    }
    $summary.steps.build = "PASS"
    Write-Ok "编译完成"
} else {
    Write-Step "1-2/5 跳过编译（-SkipBuild）"
    $summary.steps.configure = "SKIPPED"
    $summary.steps.build = "SKIPPED"
}

$ppetExe = Join-Path $BuildFull "bin/ppet.exe"
$verifyExe = Join-Path $BuildFull "ppet_render_verify.exe"
if (-not (Test-Path $ppetExe)) { Write-Fail "找不到 $ppetExe"; exit 1 }
if (-not (Test-Path $verifyExe)) { Write-Fail "找不到 $verifyExe"; exit 1 }

# ---------------------------------------------------------------------------
# 3) 部署 Qt / Cubism / MinGW 运行时 DLL
# ---------------------------------------------------------------------------
Write-Step "3/5 部署运行时 DLL"
$exeDir = Split-Path $ppetExe -Parent
if ($QtPrefix -ne "") {
    $qtRoot = Split-Path (Split-Path $QtPrefix -Parent) -Parent
    $windeploy = Join-Path $qtRoot "bin/windeployqt.exe"
    if (Test-Path $windeploy) {
        & $windeploy --no-translations --no-system-d3d-compiler --no-opengl-sw $ppetExe
        if ($LASTEXITCODE -eq 0) { Write-Ok "windeployqt 完成" }
        else { Write-Warn "windeployqt 返回 $LASTEXITCODE，请手工检查 platforms/qwindows.dll" }
    } else {
        Write-Warn "未找到 $windeploy；请手工部署 Qt6Widgets/Qt6Core/Qt6Gui 与 platforms/qwindows.dll"
    }
} else {
    Write-Warn "未指定 -QtPrefix，跳过 windeployqt"
}
foreach ($dll in @("Live2DCubismCore.dll", "libgcc_s_seh-1.dll",
                   "libstdc++-6.dll", "libwinpthread-1.dll")) {
    $p = Join-Path $exeDir $dll
    if (Test-Path $p) { Write-Ok "已部署 $dll" }
    else { Write-Warn "缺 $dll（0xC0000135 常见原因；CMake POST_BUILD 通常会拷）" }
}
$summary.steps.deploy = "DONE"

# ---------------------------------------------------------------------------
# 4) 试运行
# ---------------------------------------------------------------------------
Write-Step "4/5 试运行 ppet.exe（$StartupWaitSeconds 秒）"
$logPath = Join-Path $OutFull "last.log"
$proc = Start-Process -FilePath $ppetExe -WorkingDirectory $OutFull -PassThru
Start-Sleep -Seconds $StartupWaitSeconds
$alive = -not $proc.HasExited
$exitCode = -1
if ($alive) {
    Stop-Process -Id $proc.Id -Force
    Write-Ok "进程持续运行，未闪退（已主动结束）"
    $summary.steps.startup = "PASS"
} else {
    $exitCode = $proc.ExitCode
    Write-Fail "进程已退出，代码 $exitCode（0xC0000135 = 3221225781 表示缺 DLL）"
    $summary.steps.startup = "FAIL"
    $summary.steps.startup_exit_code = $exitCode
}
if (Test-Path $logPath) {
    $fatal = Select-String -Path $logPath -Pattern "FATAL|ERROR" -ErrorAction SilentlyContinue
    if ($fatal) {
        Write-Warn "last.log 中有 $($fatal.Count) 条 ERROR/FATAL，摘录："
        $fatal | Select-Object -First 5 | ForEach-Object { Write-Host "    $($_.Line)" }
    } else {
        Write-Ok "last.log 无 ERROR/FATAL"
    }
    $summary.steps.log = $logPath
}

# ---------------------------------------------------------------------------
# 5) 三类模型的渲染验证
# ---------------------------------------------------------------------------
Write-Step "5/5 渲染验证：VRM / PMX(.zip) / moc3"
$cases = @(
    @{ Name = "vrm"; Path = $Vrm; Label = "VRM 模型" },
    @{ Name = "pmx_zip"; Path = $PmxZip; Label = "PMX 压缩包" },
    @{ Name = "moc3"; Path = $Moc3; Label = "Live2D moc3 模型" }
)
$allPass = $true
foreach ($c in $cases) {
    Write-Host ""
    Write-Host "  --- $($c.Label) ---" -ForegroundColor White
    if ([string]::IsNullOrWhiteSpace($c.Path)) {
        Write-Warn "未提供 -$($c.Name.ToUpper()) 路径，跳过"
        $summary.models += [ordered]@{ name = $c.Name; result = "SKIPPED" }
        continue
    }
    if (-not (Test-Path $c.Path)) {
        Write-Fail "模型不存在：$($c.Path)"
        $summary.models += [ordered]@{ name = $c.Name; result = "FAIL"; path = $c.Path; reason = "文件不存在" }
        $allPass = $false
        continue
    }
    $shot = Join-Path $OutFull "$($c.Name).png"
    $report = Join-Path $OutFull "$($c.Name).json"
    & $verifyExe "--model" $c.Path "--shot" $shot "--report" $report `
                 "--width" "480" "--height" "640" "--frames" "30"
    $code = $LASTEXITCODE
    $verdict = "FAIL"
    $detail = $null
    if ($code -eq 0 -and (Test-Path $report)) {
        $detail = Get-Content $report -Raw -Encoding UTF8 | ConvertFrom-Json
        $verdict = if ($detail.result) { $detail.result } else { "PASS" }
        Write-Ok "$($c.Label)：$verdict | 覆盖率 $($detail.render.coverage_percent)% | GL 错误 $($detail.render.gl_error)"
        Write-Ok "  GPU：$($detail.platform.gl_renderer)"
        Write-Ok "  截图：$shot"
        if ($detail.warnings -and $detail.warnings.Count -gt 0) {
            foreach ($w in $detail.warnings) { Write-Warn "  $w" }
        }
    } else {
        Write-Fail "$($c.Label)：渲染验证退出码 $code"
        $allPass = $false
    }
    $summary.models += [ordered]@{
        name = $c.Name
        path = $c.Path
        result = $verdict
        screenshot = $shot
        report = $report
        coverage_percent = if ($detail) { $detail.render.coverage_percent } else { $null }
        gl_error = if ($detail) { $detail.render.gl_error } else { $null }
        gl_renderer = if ($detail) { $detail.platform.gl_renderer } else { $null }
        exit_code = $code
    }
}

# ---------------------------------------------------------------------------
# 汇总
# ---------------------------------------------------------------------------
Write-Step "汇总"
$summary.overall = if ($allPass -and $summary.steps.startup -eq "PASS") { "PASS" } else { "FAIL" }
$summary | ConvertTo-Json -Depth 8 |
    Set-Content (Join-Path $OutFull "summary.json") -Encoding UTF8
Write-Host "  报告目录：$OutFull" -ForegroundColor White
Write-Host "  summary.json / *.png / *.json / last.log" -ForegroundColor White
if ($summary.overall -eq "PASS") {
    Write-Ok "总体结论：PASS（三类模型均正常渲染）"
    exit 0
} else {
    Write-Fail "总体结论：FAIL（详见 summary.json 与上面各项）"
    exit 1
}
