# M2 离线验收：mock 上游 -> axr run 录制 -> events/run.json/graph.mmd -> replay
# 用法: powershell -ExecutionPolicy Bypass -File tests\e2e_proxy.ps1
$ErrorActionPreference = "Stop"

$envScript = if ($env:AXR_ENV) { $env:AXR_ENV } else { "D:\env\axr\env.ps1" }
if (Test-Path $envScript) { . $envScript | Out-Null }

$root = Split-Path -Parent $PSScriptRoot
$build = Join-Path $root "build"
$axr = Join-Path $build "axr.exe"
$mockExe = Join-Path $build "mock_openai.exe"
$cfg = Join-Path $root "tests\proxy_test.yaml"
$agent = Join-Path $root "tests\fake_agent.py"
$runsRoot = Join-Path $build "e2e-runs"

if (-not (Test-Path $axr)) { throw "缺少 $axr，请先运行 tests\run_tests.ps1" }
if (Test-Path $runsRoot) { Remove-Item -Recurse -Force $runsRoot }

$script:failures = @()
function Check($cond, $msg) {
    if ($cond) { Write-Host "  PASS  $msg" } else { Write-Host "  FAIL  $msg"; $script:failures += $msg }
}
function NewestRun {
    Get-ChildItem (Join-Path $runsRoot "runs") -Directory | Sort-Object LastWriteTime -Descending | Select-Object -First 1
}
function CountKind($eventsPath, $kind) {
    $raw = Get-Content $eventsPath -Raw
    return ([regex]::Matches($raw, '"kind":"' + $kind + '"')).Count
}

$mock = $null
try {
    $mock = Start-Process -FilePath $mockExe -ArgumentList "--port", "9099" -PassThru -WindowStyle Hidden
    $ready = $false
    for ($i = 0; $i -lt 50; $i++) {
        try {
            Invoke-WebRequest -Uri "http://127.0.0.1:9099/__case/deep" -TimeoutSec 1 -UseBasicParsing | Out-Null
            $ready = $true
            break
        } catch { Start-Sleep -Milliseconds 100 }
    }
    if (-not $ready) { throw "mock_openai 未就绪" }

    Write-Host "[1] axr run：非流式，3 轮工具调用 + 最终回答"
    & $axr run --config $cfg --root $runsRoot -- python $agent
    Check ($LASTEXITCODE -eq 0) "axr run 退出码 0"

    $run1 = NewestRun
    Check ($null -ne $run1) "生成 run 目录"
    $events1 = Join-Path $run1.FullName "events.jsonl"
    $runJson1 = Join-Path $run1.FullName "run.json"
    $graph1 = Join-Path $run1.FullName "graph.mmd"
    $meta1 = Join-Path $run1.FullName "meta.json"

    Check (Test-Path $events1) "events.jsonl 已落盘"
    Check (Test-Path $runJson1) "run.json 已生成"
    Check (Test-Path $graph1) "graph.mmd 已生成"
    Check (Test-Path $meta1) "meta.json 已生成"

    Check ((CountKind $events1 "req") -ge 4) "req 事件 >= 4"
    Check ((CountKind $events1 "resp") -ge 4) "resp 事件 >= 4"

    $rawEvents = Get-Content $events1 -Raw
    Check ($rawEvents -notmatch "sk-0123456789") "body 中的 sk- 密钥已脱敏"
    Check ($rawEvents -match '"authorization":\s*"\*\*\*"') "authorization 头已脱敏"

    $run = Get-Content $runJson1 -Raw | ConvertFrom-Json
    Check ($run.steps.Count -ge 3) "还原步数 >= 3（实际 $($run.steps.Count)）"
    Check ($null -ne $run.total) "token 统计存在"

    $meta = Get-Content $meta1 -Raw | ConvertFrom-Json
    Check ($meta.status -eq "complete") "meta.status = complete"
    Check ($meta.step_count -ge 3) "meta.step_count >= 3"
    Check ($meta.models -contains "deepseek-chat") "meta.models 含 deepseek-chat"

    $graphText = Get-Content $graph1 -Raw
    Check ($graphText -match "flowchart TD") "graph.mmd 是 Mermaid flowchart"
    Check ($graphText -match "S2") "graph.mmd 至少 3 个步骤节点"

    Write-Host "[2] axr replay：从 events.jsonl 重算"
    $replayOut = & $axr replay $run1.FullName --format text 2>&1
    Check ($LASTEXITCODE -eq 0) "replay 退出码 0"
    Check (($replayOut -join "`n") -match "Run: ") "replay 打印出 run"

    Write-Host "[3] axr run：流式（SSE 逐 chunk 录制）"
    & $axr run --config $cfg --root $runsRoot -- python $agent --stream
    Check ($LASTEXITCODE -eq 0) "流式 axr run 退出码 0"
    $run2 = NewestRun
    $events2 = Join-Path $run2.FullName "events.jsonl"
    Check ((CountKind $events2 "sse_chunk") -ge 1) "流式录制包含 sse_chunk"
    Check ((CountKind $events2 "sse_end") -ge 1) "流式录制包含 sse_end"
    $streamRun = Get-Content (Join-Path $run2.FullName "run.json") -Raw | ConvertFrom-Json
    Check ($streamRun.steps.Count -ge 1) "流式 run 还原出步骤"

    Write-Host "[4] axr serve：前台代理 + 路由转发 + 未匹配 404"
    $serveRoot = Join-Path $build "e2e-serve"
    if (Test-Path $serveRoot) { Remove-Item -Recurse -Force $serveRoot }
    $serve = Start-Process -FilePath $axr -PassThru -WindowStyle Hidden `
        -ArgumentList "serve", "--config", $cfg, "--root", $serveRoot, "--listen", "8082" `
        -RedirectStandardOutput (Join-Path $build "e2e-serve.log")
    $up = $false
    for ($i = 0; $i -lt 60; $i++) {
        try {
            $c = New-Object System.Net.Sockets.TcpClient
            $c.Connect("127.0.0.1", 8082)
            $c.Close()
            $up = $true
            break
        } catch { Start-Sleep -Milliseconds 100 }
    }
    Check $up "serve 开始监听 8082"
    if ($up) {
        $resp = Invoke-WebRequest -Uri "http://127.0.0.1:8082/v1/chat/completions" -Method POST `
            -Body '{"model":"deepseek-chat","messages":[{"role":"user","content":"hi"}]}' `
            -ContentType "application/json" -TimeoutSec 10 -UseBasicParsing
        Check ($resp.StatusCode -eq 200) "serve 转发到上游返回 200（实际 $($resp.StatusCode)）"

        try {
            Invoke-WebRequest -Uri "http://127.0.0.1:8082/nope" -Method POST -Body "{}" -TimeoutSec 5 -UseBasicParsing | Out-Null
            $status = 200
        } catch { $status = [int]$_.Exception.Response.StatusCode }
        Check ($status -eq 404) "未匹配路径返回 404（实际 $status）"
    }
    if (!$serve.HasExited) { Stop-Process -Id $serve.Id -Force }
    Check (Test-Path (Join-Path $serveRoot "runs")) "serve 也创建了会话目录"
}
finally {
    if ($mock -and !$mock.HasExited) { Stop-Process -Id $mock.Id -Force }
}

if ($script:failures.Count -gt 0) {
    Write-Host "`n结果: $($script:failures.Count) 项失败"
    exit 1
}
Write-Host "`n结果: 全部通过"
exit 0
