# M2 离线验收：mock 上游 -> axr run 录制 -> events/run.json/graph.mmd -> replay
# 用法: powershell -ExecutionPolicy Bypass -File tests\e2e_proxy.ps1
# 环境覆盖：AXR_BUILD_DIR 指定构建目录，AXR_PYTHON 指定 python 解释器，
#          AXR_ENV 指定需 dot-source 的环境脚本（如激活虚拟环境）；未设置用默认值
$ErrorActionPreference = "Stop"

if ($env:AXR_ENV) { . $env:AXR_ENV | Out-Null }

$root = Split-Path -Parent $PSScriptRoot
$build = if ($env:AXR_BUILD_DIR) { $env:AXR_BUILD_DIR } else { Join-Path $root "build" }
$axr = Join-Path $build "axr.exe"
$mockExe = Join-Path $build "mock_openai.exe"
$python = if ($env:AXR_PYTHON) { $env:AXR_PYTHON } else { "python" }
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
    $ErrorActionPreference = "Continue"
    $run1Out = & $axr run --config $cfg --root $runsRoot -- $python $agent 2>&1 | Out-String
    $ErrorActionPreference = "Stop"
    Check ($LASTEXITCODE -eq 0) "axr run 退出码 0"
    Check ($run1Out -match "ollama_host = \(unset\)") "P2-12 不再注入 OLLAMA_HOST"

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
    $ErrorActionPreference = "Continue"   # PS 5.1：原生 stderr 经 2>&1 会变 ErrorRecord，Stop 模式会抛 NativeCommandError
    $replayOut = & $axr replay $run1.FullName --format text 2>&1
    $ErrorActionPreference = "Stop"
    Check ($LASTEXITCODE -eq 0) "replay 退出码 0"
    Check (($replayOut -join "`n") -match "Run: ") "replay 打印出 run"

    Write-Host "[3] axr run：流式（S2 边收边发 + 逐 chunk 录制）"
    $ErrorActionPreference = "Continue"
    $streamOut = & $axr run --config $cfg --root $runsRoot -- $python $agent --stream 2>&1 | Out-String
    $ErrorActionPreference = "Stop"
    Check ($LASTEXITCODE -eq 0) "流式 axr run 退出码 0"
    Check ($streamOut -match "transfer-encoding = chunked") "S2 流式响应为 chunked（不是缓冲后一次性返回）"
    Check ($streamOut -match "sse bytes = (\d+)") "客户端收到 SSE 数据"
    $run2 = NewestRun
    $events2 = Join-Path $run2.FullName "events.jsonl"
    Check ((CountKind $events2 "sse_chunk") -ge 1) "流式录制包含 sse_chunk"
    Check ((CountKind $events2 "sse_end") -ge 1) "流式录制包含 sse_end"
    $streamRun = Get-Content (Join-Path $run2.FullName "run.json") -Raw | ConvertFrom-Json
    Check ($streamRun.steps.Count -ge 1) "流式 run 还原出步骤"

    Write-Host "[3.5] P2-8 上游拒绝 stream_options 时自动降级重试"
    Invoke-WebRequest -Uri "http://127.0.0.1:9099/__mode/usage_reject" -TimeoutSec 5 -UseBasicParsing | Out-Null
    $ErrorActionPreference = "Continue"
    $degradeOut = & $axr run --config $cfg --root $runsRoot -- $python $agent --stream 2>&1 | Out-String
    $ErrorActionPreference = "Stop"
    Check ($LASTEXITCODE -eq 0) "降级重试后流式 axr run 退出码 0"
    Check ($degradeOut -match "sse bytes = (\d+)") "降级重试后客户端仍收到 SSE 数据"
    $run3 = NewestRun
    $meta3 = Get-Content (Join-Path $run3.FullName "meta.json") -Raw | ConvertFrom-Json
    Check ($meta3.usage_injected -eq $false) "meta.usage_injected = false"
    Check ($meta3.rewritten -eq $true) "meta.rewritten = true（记录注入改写）"
    Invoke-WebRequest -Uri "http://127.0.0.1:9099/__mode/ok" -TimeoutSec 5 -UseBasicParsing | Out-Null

    Write-Host "[3.7] P2-12 本地 ollama 走 /ollama/v1 前缀仍能被采集"
    $env:AXR_FAKE_PATH = "/ollama/v1"
    $ErrorActionPreference = "Continue"
    $ollamaOut = & $axr run --config $cfg --root $runsRoot -- $python $agent 2>&1 | Out-String
    $ErrorActionPreference = "Stop"
    Remove-Item Env:\AXR_FAKE_PATH -ErrorAction SilentlyContinue
    Check ($LASTEXITCODE -eq 0) "ollama 前缀 axr run 退出码 0"
    Check ($ollamaOut -match "/ollama/v1") "客户端请求发到 /ollama/v1"
    $runOllama = NewestRun
    $ollamaRun = Get-Content (Join-Path $runOllama.FullName "run.json") -Raw | ConvertFrom-Json
    Check ($ollamaRun.steps.Count -ge 3) "ollama 前缀 run 还原步数 >= 3（实际 $($ollamaRun.steps.Count)）"

    Write-Host "[3.9] WP4 流式 sse_chunk 落盘脱敏（客户端仍收原文）"
    Invoke-WebRequest -Uri "http://127.0.0.1:9099/__case/secret" -TimeoutSec 5 -UseBasicParsing | Out-Null
    $ErrorActionPreference = "Continue"
    $secretOut = & $axr run --config $cfg --root $runsRoot -- $python $agent --stream 2>&1 | Out-String
    $ErrorActionPreference = "Stop"
    Check ($LASTEXITCODE -eq 0) "secret 流式 axr run 退出码 0"
    Check ($secretOut -match "sse has secret = True") "客户端收到的 SSE 仍是原文（含密钥）"
    $runSecret = NewestRun
    $eventsSecret = Join-Path $runSecret.FullName "events.jsonl"
    $secretHits = (Select-String -Path $eventsSecret -Pattern "sk-SECRETKEY0123456789ABCDEFGHIJ" -AllMatches).Count
    Check (($null -eq $secretHits) -or ($secretHits -eq 0)) "full 模式 sse_chunk 已脱敏"
    Invoke-WebRequest -Uri "http://127.0.0.1:9099/__case/deep" -TimeoutSec 5 -UseBasicParsing | Out-Null

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

        # T10：超过 max_body_bytes（4096）→ 413，拒绝且不 OOM
        $pad = 'x' * 8000
        $big = '{"model":"deepseek-chat","messages":[{"role":"user","content":"' + ($pad -join '') + '"}]}'
        try {
            Invoke-WebRequest -Uri "http://127.0.0.1:8082/v1/chat/completions" -Method POST `
                -Body $big -ContentType "application/json" -TimeoutSec 20 -UseBasicParsing | Out-Null
            $status = 200
        } catch { $status = [int]$_.Exception.Response.StatusCode }
        Check ($status -eq 413) "T10 超大 body 被拒绝（413，实际 $status）"

        # T11：上游 500 → 代理透传 500（事件流里另记 err）
        Invoke-WebRequest -Uri "http://127.0.0.1:9099/__mode/fail" -TimeoutSec 5 -UseBasicParsing | Out-Null
        try {
            Invoke-WebRequest -Uri "http://127.0.0.1:8082/v1/chat/completions" -Method POST `
                -Body '{"model":"deepseek-chat","messages":[{"role":"user","content":"hi"}]}' `
                -ContentType "application/json" -TimeoutSec 10 -UseBasicParsing | Out-Null
            $status = 200
        } catch { $status = [int]$_.Exception.Response.StatusCode }
        Check ($status -eq 500) "T11 上游 500 原样透传（实际 $status）"
        Invoke-WebRequest -Uri "http://127.0.0.1:9099/__mode/ok" -TimeoutSec 5 -UseBasicParsing | Out-Null

        try {
            Invoke-WebRequest -Uri "http://127.0.0.1:8082/nope" -Method POST -Body "{}" -TimeoutSec 5 -UseBasicParsing | Out-Null
            $status = 200
        } catch { $status = [int]$_.Exception.Response.StatusCode }
        Check ($status -eq 404) "未匹配路径返回 404（实际 $status）"

        # 内置 Web UI：页面 + runs 接口 + 浏览器根路径
        $ui = Invoke-WebRequest -Uri "http://127.0.0.1:8082/__xray" -TimeoutSec 5 -UseBasicParsing
        Check ($ui.StatusCode -eq 200 -and $ui.Content -match "AgentXray") "Web UI 页面可访问（/__xray）"
        $uiApi = Invoke-WebRequest -Uri "http://127.0.0.1:8082/__xray/api/runs" -TimeoutSec 5 -UseBasicParsing
        Check ($uiApi.StatusCode -eq 200 -and $uiApi.Content -match "\[") "Web UI runs 接口返回 JSON 数组"
        $uiRoot = Invoke-WebRequest -Uri "http://127.0.0.1:8082/" -Headers @{ Accept = "text/html" } -TimeoutSec 5 -UseBasicParsing
        Check ($uiRoot.StatusCode -eq 200 -and $uiRoot.Content -match "AgentXray") "浏览器访问根路径返回页面"
        try {
            Invoke-WebRequest -Uri "http://127.0.0.1:8082/nope" -Headers @{ Accept = "text/html" } -TimeoutSec 5 -UseBasicParsing | Out-Null
            $status = 200
        } catch { $status = [int]$_.Exception.Response.StatusCode }
        Check ($status -eq 404) "带 text/html 的其他路径不走 UI（404）"
    }
    if (!$serve.HasExited) { Stop-Process -Id $serve.Id -Force }
    Check (Test-Path (Join-Path $serveRoot "runs")) "serve 也创建了会话目录"

    Write-Host "[4.1] WP6 serve 无流量不创建会话目录（懒建）"
    $quietRoot = Join-Path $build "e2e-serve-quiet"
    if (Test-Path $quietRoot) { Remove-Item -Recurse -Force $quietRoot }
    $quiet = Start-Process -FilePath $axr -PassThru -WindowStyle Hidden `
        -ArgumentList "serve", "--config", $cfg, "--root", $quietRoot, "--listen", "8085" `
        -RedirectStandardOutput (Join-Path $build "e2e-serve-quiet.log")
    $quietUp = $false
    for ($i = 0; $i -lt 60; $i++) {
        try {
            $c = New-Object System.Net.Sockets.TcpClient
            $c.Connect("127.0.0.1", 8085)
            $c.Close()
            $quietUp = $true
            break
        } catch { Start-Sleep -Milliseconds 100 }
    }
    Check $quietUp "serve 开始监听 8085（无流量）"
    if (!$quiet.HasExited) { Stop-Process -Id $quiet.Id -Force }
    Start-Sleep -Milliseconds 300
    Check (-not (Test-Path (Join-Path $quietRoot "runs"))) "无流量 serve 未创建 run 目录"

    Start-Sleep -Milliseconds 400
    $serveRun = Get-ChildItem (Join-Path $serveRoot "runs") -Directory | Sort-Object LastWriteTime -Descending |
        Select-Object -First 1
    if ($serveRun) {
        $serveEvents = Get-Content (Join-Path $serveRun.FullName "events.jsonl") -Raw
        Check ($serveEvents -match '"kind":"err"') "T11 事件流里记录了 err"
        Check ($serveEvents -match '"status":500') "T11 事件里保留了上游 500"
    } else {
        Check $false "serve 未生成 run 目录"
    }
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
