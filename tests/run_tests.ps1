# 一键回归：初始化环境 -> 构建 -> 自检
# 用法: powershell -ExecutionPolicy Bypass -File tests\run_tests.ps1
$ErrorActionPreference = "Stop"

# 环境区与代码区隔离：环境脚本放在 D 盘固定位置，可用 AXR_ENV 覆盖
$envScript = if ($env:AXR_ENV) { $env:AXR_ENV } else { "D:\env\axr\env.ps1" }
if (Test-Path $envScript) { . $envScript } else { Write-Warning "未找到 $envScript，假定工具链已在 PATH 中" }

$root = Split-Path -Parent $PSScriptRoot
$buildDir = Join-Path $root "build"

cmake -S $root -B $buildDir -G Ninja -DCMAKE_BUILD_TYPE=Release
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

cmake --build $buildDir
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

& (Join-Path $buildDir "axr.exe") selftest
exit $LASTEXITCODE
