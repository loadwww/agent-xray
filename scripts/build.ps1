# AgentXray 一键构建（Windows / PowerShell 5.1+）
# 自动定位 Visual Studio 的 MSVC + Windows SDK，以及 VS 自带的 CMake / Ninja。
# 不需要手动初始化环境，也不修改系统环境变量。
#
# 用法:
#   powershell -ExecutionPolicy Bypass -File scripts\build.ps1          # 增量构建 + 自检
#   powershell -ExecutionPolicy Bypass -File scripts\build.ps1 -Clean   # 清空 build 后重建

param(
    [switch]$Clean,
    [string]$Config = "Release"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root

function Find-VS {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path $vswhere) {
        $p = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath 2>$null
        if ($p) { return $p.Trim() }
        $p = & $vswhere -latest -products * -property installationPath 2>$null
        if ($p) { return $p.Trim() }
    }
    # vswhere 没有登记时，退回常见安装路径（含 Build Tools）
    $candidates = @(
        "C:\Program Files\Microsoft Visual Studio\2022\Community",
        "C:\Program Files\Microsoft Visual Studio\2022\Professional",
        "C:\Program Files\Microsoft Visual Studio\2022\Enterprise",
        "C:\Program Files\Microsoft Visual Studio\2022\BuildTools",
        "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools"
    )
    foreach ($c in $candidates) {
        if (Test-Path (Join-Path $c "VC\Auxiliary\Build\vcvars64.bat")) { return $c }
    }
    return $null
}

function Find-Tool {
    param([string]$Name, [string]$Fallback)
    $cmd = Get-Command $Name -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    if ($Fallback -and (Test-Path $Fallback)) { return $Fallback }
    throw "未找到 $Name，请安装 CMake/Ninja，或改用带 C++ 工作负载的 Visual Studio 2022"
}

$vs = Find-VS
if (-not $vs) {
    throw "未找到 Visual Studio 2022（安装时请勾选「使用 C++ 的桌面开发」工作负载）"
}
$vcvars = Join-Path $vs "VC\Auxiliary\Build\vcvars64.bat"
Write-Host "[build] Visual Studio: $vs"

# 用临时 bat 导入 vcvars64 的 INCLUDE / LIB / PATH（只影响当前进程）
$tmp = Join-Path $env:TEMP ("axr-vcvars-" + [guid]::NewGuid().ToString("N") + ".bat")
Set-Content -Path $tmp -Encoding ascii -Value "@echo off`r`ncall `"$vcvars`" >nul 2>&1`r`nset"
$lines = & cmd.exe /c $tmp
Remove-Item $tmp -Force -ErrorAction SilentlyContinue
foreach ($line in $lines) {
    if ($line -notmatch "^([^=]+)=(.*)$") { continue }
    $name = $matches[1]
    $value = $matches[2]
    if ($name -notmatch "^[A-Za-z_][A-Za-z0-9_]*$") { continue }
    Set-Item -Path ("env:" + $name) -Value $value
}
if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
    throw "vcvars64 初始化失败（找不到 cl.exe）：请确认已安装 MSVC 与 Windows SDK"
}

$cmakeExe = Find-Tool "cmake" (Join-Path $vs "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe")
$ninjaExe = Find-Tool "ninja" (Join-Path $vs "Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe")
Write-Host "[build] cmake : $cmakeExe"
Write-Host "[build] ninja : $ninjaExe"

if ($Clean -and (Test-Path "build")) { Remove-Item -Recurse -Force "build" }

& $cmakeExe -S . -B build -G Ninja "-DCMAKE_BUILD_TYPE=$Config" "-DCMAKE_MAKE_PROGRAM=$ninjaExe"
if ($LASTEXITCODE -ne 0) { throw "CMake 配置失败（退出码 $LASTEXITCODE）" }

& $cmakeExe --build build
if ($LASTEXITCODE -ne 0) { throw "编译失败（退出码 $LASTEXITCODE）" }

$exe = Join-Path $root "build\axr.exe"
Write-Host ""
Write-Host "[build] 产物: $exe"
if (Test-Path "tests\fixtures") { & $exe selftest } else { & $exe version }
