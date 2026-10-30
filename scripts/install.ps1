# AgentXray 一键安装（Windows，无需编译）
# 从 GitHub 最新 Release 下载 axr.exe 到用户目录并加入 PATH。
#
# 用法（在 PowerShell 里执行）:
#   irm https://raw.githubusercontent.com/loadwww/agent-xray/master/scripts/install.ps1 | iex

$ErrorActionPreference = "Stop"
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

# 直连 releases/latest/download，不经过 api.github.com（避免匿名 API 限流）
$url = "https://github.com/loadwww/agent-xray/releases/latest/download/axr.exe"

$dir = Join-Path $env:LOCALAPPDATA "Programs\axr"
$dst = Join-Path $dir "axr.exe"
New-Item -ItemType Directory -Force -Path $dir | Out-Null

Write-Host "[install] 下载 -> $dst"
try {
    Invoke-WebRequest -Uri $url -OutFile $dst -UseBasicParsing
} catch {
    throw "下载失败（可能还没有 Release 或网络不可达）：$url`n$($_.Exception.Message)"
}

$head = [System.IO.File]::ReadAllBytes($dst)[0..1]
if (-not ($head[0] -eq 0x4D -and $head[1] -eq 0x5A)) { throw "下载内容不是可执行文件（可能被拦截或重定向到 HTML）" }

$userPath = [Environment]::GetEnvironmentVariable("Path", "User")
if ($userPath -notlike "*$dir*") {
    [Environment]::SetEnvironmentVariable("Path", "$userPath;$dir", "User")
    $env:Path = "$env:Path;$dir"
    Write-Host "[install] 已把 $dir 加入用户 PATH（新开的终端生效）"
}

& $dst version
Write-Host ""
Write-Host "安装完成。用法示例:"
Write-Host "  `$env:OPENAI_API_KEY  = '<你的 key>'"
Write-Host "  `$env:OPENAI_BASE_URL = 'http://127.0.0.1:8080/v1'"
Write-Host "  axr serve --listen 8080"
Write-Host "  axr show <runID>"
