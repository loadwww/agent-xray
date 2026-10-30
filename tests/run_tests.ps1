# 一键回归：构建 + 自检（工具链定位交给 scripts/build.ps1）
# 用法: powershell -ExecutionPolicy Bypass -File tests\run_tests.ps1
$ErrorActionPreference = "Stop"

$root = Split-Path -Parent $PSScriptRoot
& (Join-Path $root "scripts\build.ps1")
exit $LASTEXITCODE
