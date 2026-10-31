# AgentXray

**不改一行 Agent 代码，就能看清它每一步在想什么、调了哪个工具、在哪一步卡住。**

`axr` 是一个本地反向代理 + 可视化工具。你照常启动自己的 Agent，它把每一轮请求 / 响应（含流式分片）
录下来，还原成「主模型思考链路 + 工具调用扇出」，并在浏览器里**边跑边渲染**。

- 零改造：不接 SDK、不改代码、不重打包，只改一个 `OPENAI_BASE_URL`
- 看得懂：失败节点直接标红，顶部告诉你「最早在第几步受挫」
- 留得下：`events.jsonl` / `run.json` / `graph.mmd` / `report.md` 全部落盘，可回放、可比对
- 单文件：一个 `axr.exe`，静态链接，UI 内嵌，离线可用

## 实际长什么样

![AgentXray Web UI](docs/webui.png)

> 真实 AutoGPT 任务（14 步 / 24 次工具调用）。Step 8 的 `read_webpage ×3`、Step 9 的 `http_get ×8`
> 全部失败或被拒，自动标红，顶部提示「最早出现在 Step 8」；甘特图给出总耗时与工具的并行关系。
> 一键折叠后只看主模型链路：[折叠视图](docs/webui-collapsed.png)（`?collapse=1`）。

## 部署（Windows）

```powershell
irm https://raw.githubusercontent.com/loadwww/agent-xray/master/scripts/install.ps1 | iex
```

装到 `%LOCALAPPDATA%\Programs\axr` 并加入用户 PATH（**新开**的终端生效）。然后：

```powershell
$env:OPENAI_API_KEY  = "<你的 key>"
$env:OPENAI_BASE_URL = "http://127.0.0.1:8080/v1"   # 让 agent 走 axr 代理
axr serve --listen 8080                             # 起代理，浏览器打开 127.0.0.1:8080/__xray
axr show <runID>                                    # 终端看步骤 / token / 工具扇出
```

`axr serve` 是前台程序，不会自动弹浏览器，窗口关掉或 `Ctrl+C` 就结束并落盘。想让它不占用 agent
所在的终端，就在独立窗口里起：

```powershell
Start-Process axr -ArgumentList 'serve','--listen','8080' -WorkingDirectory D:\agent-xray
```

`config.yaml` 默认从**当前目录**读取；不在仓库目录时用 `--config D:\agent-xray\config.yaml`
（否则会静默使用内置默认上游，你的 `model_map` 等配置不生效）。

## 构建（Linux/macOS）

Python 侧无依赖，只需要 C++17 编译器与 CMake；Ubuntu/Debian 先 `sudo apt install cmake g++ libcurl4-openssl-dev`
（macOS 装 Xcode Command Line Tools 即可）。然后三条命令：

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/axr selftest          # selftest 不会随 cmake 自动跑，手动执行，全部通过即构建完整
```

产物是单个 `./build/axr` 与 `./build/mock_openai`；`bash tests/e2e_proxy.sh` 可离线跑一遍采集验收。

## 为什么实用

- **一眼定位失败**：工具结果带 `Error` / `403` / `Forbidden` / `Failed to` / `too much output` 等特征时
  自动标红（协议层没有 `is_error` 时按文本判断）；受挫的 Step 边框变红并标注「受阻」，顶部横幅给出
  失败总数与最早出现的 Step，侧栏该 run 带「N 失败」徽标。
- **跑的时候就能看**：采集中的 run 每 3 秒刷新，直接读 `events.jsonl` 现建链，长任务不用等结束。
- **两种视角**：默认看完整图（工具分支全展开）；一键折叠只看主模型思考链路；顶部甘特图给出总耗时、
  每步 LLM 时间片、同名工具合并后的并行条与步骤间隙，并行 / 串行 / 卡点一目了然。
- **证据链完整**：每次 run 留下 `events.jsonl`、`run.json`、`graph.mmd`、`meta.json`、`report.md`，
  下次可以直接拿两份 run 做结构 diff，看是哪一步开始跑偏。
- **数据只说真话**：token 直接取上游响应的 `usage`（含 DeepSeek 缓存命中数），没有就显示 `-`，不估算；
  上游不返回账单，所以不展示金额，需要金额请用服务商后台或自己乘单价。
- **不侵入你的环境**：只监听本机回环，落盘副本自动脱敏（`authorization` / `x-api-key` / `sk-...`），
  原始请求照常转发，鉴权与流式体验都不受影响。

## 能拿它做什么

| 场景 | 你怎么用 | 得到什么 |
|---|---|---|
| 工具报错排查 | `axr show <runID>`，或浏览器打开该 run | 失败的 Step 与工具、错误原文、失败发生在第几步 |
| 长任务复盘 | 收尾后看 `report.md` | 概览 / 执行流程 / 时间线 / 工具表 / Token 用量 |
| 两次运行对比 | `axr diff <runA> <runB>` | 新增或消失的 Step、参数 / 结果变化、token 差值 |
| 框架横向对比 | 同一任务分别接 AutoGPT、smolagents、手写脚本 | 步数、token、工具并行结构 |
| 教学 / 演示 | 浏览器打开实时页面 | 一张随时间推进的「Agent 内部发生了什么」 |

## 常用命令

```powershell
axr serve --listen 8080                        # 常驻代理（Ctrl+C 收尾）
axr show <目录|runID>                          # 终端摘要：状态 / token / 模型 / 时间线 / 工具扇出
axr report <目录|runID> [--out report.md]      # 生成 Markdown 报告
axr list --root <root> [--json]                # 列出所有 run（steps / tools / fail_tools / tokens）
axr export --root <root> <runID> --format mermaid  # 导出流程图（json 则导出完整 Run）
axr diff <runA> <runB>                         # 按 Step 对齐比较两次运行
axr prune --days 7 --root <root> [--apply]     # 清理旧 run（默认 dry-run）
```

`<root>` 默认是 `%USERPROFILE%\.xray`，每个 run 一个目录：

```
<root>/runs/<sessionId>/
  events.jsonl   # req / sse_chunk / sse_end / resp / err
  run.json       # 还原出的步骤与工具
  graph.mmd      # Mermaid 流程图
  meta.json      # 摘要：步数、工具数、失败数、token、完整性、模型
  report.md      # Markdown 报告
```

## Web UI

页面内嵌在 exe 里，无 CDN、无需联网。起代理后直接访问：

```
http://127.0.0.1:8080/__xray                        # 页面
http://127.0.0.1:8080/__xray?id=<runID>             # 直接定位某次 run
http://127.0.0.1:8080/__xray?id=<runID>&collapse=1  # 默认折叠工具调用
```

也可以直接打开 `http://127.0.0.1:8080/`（仅当 `Accept: text/html`；API 客户端的 `GET /` 仍照常转发）。
给脚本消费的接口有两个：

```
GET /__xray/api/runs         # run 列表（steps / tools / failTools / tokens / live）
GET /__xray/api/run?id=<id>  # 单个 run：steps / token / failTools / 甘特图时间字段 / mermaid 源码
```

## 配置

上游按**最长前缀匹配 + 剥离 match 前缀**路由；写多条即可同时接入不同服务：

```yaml
proxy:
  upstreams:
    - match: "/v1"        # /v1/chat/completions → https://api.deepseek.com/v1/chat/completions
      url: "https://api.deepseek.com/v1"
      name: "deepseek"
      model_map:            # 客户端只认 gpt-4o 之类的名字时，转发前替换 model 字段
        gpt-4o: deepseek-chat
        gpt-4-turbo: deepseek-chat
    - match: "/ollama"    # /ollama/chat/completions → http://127.0.0.1:11434/v1/chat/completions
      url: "http://127.0.0.1:11434/v1"
      name: "ollama"

  inject_usage: true      # 给流式请求补 stream_options.include_usage，否则拿不到 token
```

`model_map` 命中后日志会打印 `模型名改写: gpt-4o -> deepseek-chat`，落盘记录的是真实发给上游的请求体。

## 边界（如实说明）

- 只还原 OpenAI 兼容协议的 chat completions（路径以 `/chat/completions` 结尾，含流式 SSE，
  本地上游如 `http://127.0.0.1:11434/v1/chat/completions` 同样命中）。其它端点
  （Responses API、Anthropic `/v1/messages`、embeddings、models 列表等）**不还原、也不进统计**，
  但原始流量不丢：仍完整保存在 `events.jsonl`，并在 `meta.json` 计入 `unsupported_requests`
  （同时置 `parse_degraded: true`），Web UI 顶部会提示未还原的条数。
- 工具失败是**文本启发式**判断（协议层没有 `is_error`），个别措辞可能漏标；详情页能看到原始结果自行确认。
- 用 `Observation:` 文本回填工具结果的框架（如 smolagents），能还原工具调用与扇出，但**结果内容**落在
  下一条消息正文里，工具表的结果会显示 `-`。
- 并行子 agent 交错写同一 messages 链时，分支归属可能误判；建议把可疑分支拆成独立 run。
- 只监听本机回环，无鉴权；请勿暴露到公网。
- `proxy.max_body_bytes` 超限的请求由 HTTP 层直接回 413，代理拿不到完整请求体，因此不产生事件（宁可少记也不占内存）。

## 待办

- **本地模型部署支持**（Ollama / llama.cpp / vLLM / LM Studio 等 OpenAI 兼容服务）：目标是
  `axr serve --local ollama` 自动把 `/v1` 指向 `http://127.0.0.1:11434/v1` 并跳过 API key 校验；
  本地服务不回 `usage` 时先补 `stream_options.include_usage`，仍拿不到就把 token 标为「未观测」，不估算。

## 许可证

[MIT](LICENSE) © 2026 loadwww
