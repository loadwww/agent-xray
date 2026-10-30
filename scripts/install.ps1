# AgentXray 一键安装（Windows，无需编译）
# 从 GitHub 最新 Release 下载 axr.exe 到用户目录并加入 PATH。
#
# 用法（在 PowerShell 里执行）:
#   irm https://raw.githubusercontent.com/loadwww/agent-xray/master/scripts/install.ps1 | iex

$ErrorActionPreference = "Stop"
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

$repo = "loadwww/agent-xray"
$headers = @{ "User-Agent" = "axr-installer" }

Write-Host "[install] 查询最新版本..."
$rel = Invoke-RestMethod -Uri "https://api.github.com/repos/$repo/releases/latest" -Headers $headers
$asset = $rel.assets | Where-Object { $_.name -eq "axr.exe" } | Select-Object -First 1
if (-not $asset) { throw "最新 Release（$($rel.tag_name)）里没有 axr.exe 资产" }

$dir = Join-Path $env:LOCALAPPDATA "Programs\axr"
$dst = Join-Path $dir "axr.exe"
New-Item -ItemType Directory -Force -Path $dir | Out-Null

Write-Host "[install] 下载 $($rel.tag_name) -> $dst"
Invoke-WebRequest -Uri $asset.browser_download_url -OutFile $dst -UseBasicParsing -Headers $headers

$userPath = [Environment]::GetEnvironmentVariable("Path", "User")
if ($userPath -notlike "*$dir*") {
    [Environment]::SetEnvironmentVariable("Path", "$userPath;$dir", "User")
    $env:Path = "$env:Path;$dir"
    Write-Host "[install] 已把 $dir 加入用户 PATH（新开的终端生效）"
}

& $dst version
Write-Host ""
Write-Host "安装完成。用法示例:"
Write-Host "  `$env:OPENAI_API_KEY = '<你的 key>'"
Write-Host "  axr run -- python your_agent.py"
