# AgentXray 改进实施 — 交接说明

> 本文供下一个对话接手。第一批（P0-1 → P0-2 → P0-3 → P1-5 的 1、2 两条）与第二批
> （P1-4 → P1-5 的 3、4 两条 → P1-6）均已完成，所有改动已 push 到 `origin/master`
> （`6528cdf..9c7e73e`），CI 双平台绿。

## 一、基线与环境

- 仓库：`https://github.com/loadwww/agent-xray`，`master` 分支，HEAD = `9c7e73e`
- 语言：C++17；依赖只有 `third_party/nlohmann/json.hpp` 与 `third_party/cpp-httplib/httplib.h`（header-only）
- 基线 selftest：174 项；当前 **243 passed / 0 failed**（Windows 与 WSL Linux 均实测）
- 工作单原文：`agent-xray-improvement-plan.txt`（完整任务清单在下方第四节引用）

### 构建命令
```powershell
# Windows（增量构建 + 自动跑 selftest）
powershell -ExecutionPolicy Bypass -File scripts\build.ps1
```
```bash
# Linux/macOS（cmake 构建，selftest 需手动跑）
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/axr selftest
```

### 全局约束（每个任务都必须遵守）
- C++17，不上 C++20
- 不破坏现有 CLI 参数，新增只能是可选项
- Windows + Linux 都要编译通过；selftest 不得低于基线
- 除 P1-4 需要的 libcurl 外，不新增第三方依赖
- Web UI 只做增量修改（`src/ui/webui.cpp` 内嵌 HTML，分段 R"HTML(...)HTML"）
- 每个任务独立提交，commit 信息写“为什么”
- 不顺手做无关重构

## 二、已完成的任务

### P0-1 跨任务串链修复 — commit `bfafc0a`
- 文件：`src/chain/link.cpp`
- 改动：`prefixIsOnlySystem()` —— 命中前缀全为 system 消息时不建父子边，作为新 run 的根
- fixture：`tests/fixtures/t14_two_tasks_same_system`（两个独立任务共享同一 system prompt，间隔 5 秒）
- 验收：runs=2，各 2 步，均 complete；t2/t3/t12/t13 回归通过

### P0-2 多 run 全量输出 — commit `e631958`
- `src/chain/run.h`：新增 `findRunById()`
- `src/store/finalize.cpp`：`meta.json` 写 `primary` + `runs[]` 摘要（`runBriefJson`）；非主 run 落盘 `run-<i>.json`（i 从 0，按 runs 顺序跳主 run）
- `src/store/run_index.{h,cpp}`：`RunBrief`、`RunMeta.primary/runs`
- CLI：`axr list --all`（run 级平铺）、`axr show/report/export --run <runId>`
- `src/ui/webui.cpp`：`webUiRun(cfg,id,runId)` 三参（`cmd_selftest.cpp` 4 处调用已同步）；`resolveRunFile` 映射 runId → `run.json`/`run-<i>.json`；侧栏折叠组（`selectedRun`/`expanded`）
- **注意坑**：`resolveRunFile(runDir, runId)` 中 runId 为空时必须先返回 `run.json` 再读 meta，否则旧目录（无 meta）会 404
- replay 不落盘（内存分析）；验证多 run：`axr replay tests/fixtures/t14_two_tasks_same_system --run <id> --format json`

### P0-3 非 chat/completions 请求显式隔离 — commit `120710b`
- `src/store/events_reader.{h,cpp}`：
  - `isSupportedEndpoint(upstream)`：路径以 `/chat/completions` 结尾即白名单（兼容 `http://127.0.0.1:11434/v1/chat/completions`）
  - 非白名单请求不组帧，原始流量留 `events.jsonl`
  - `AssembleStats`：`unsupportedRequests` / `parseFailed` / `truncatedEvents`
  - `RawEvent.truncated` 字段（读 `truncated`）
- `src/chain/run.h` `ChainResult`：加三个计数字段
- `src/store/finalize.{h,cpp}`：`RunSummary` 加三字段；meta 写 `unsupported_requests`/`parse_failed`/`truncated_events`（>0 才写，参考 `dropped_events` 先例）+ `parse_degraded: true`
- `src/ui/webui.cpp`：`addDegradedCounts()`；落盘读 meta、采集中读 chain 计数；JS 顶部 banner（非 chat/completions 提示 + parse_failed/truncated 提示）
- `README.md` 边界章节如实写明支持矩阵
- fixture：`tests/fixtures/t15_mixed_endpoints`（/v1/responses + /v1/embeddings + 本机 ollama chat/completions + truncated 事件）
- 验收：runs 只由 chat/completions 组成；`meta.unsupported_requests` 计数正确；现有 fixture 不被误过滤

### P1-5 (1)(2) LICENSE + CI — commit `ac0a789`
- `LICENSE`（MIT, 2026 loadwww）
- `README.md` 末尾加许可证标注
- `.github/workflows/ci.yml`：
  - windows：`scripts/build.ps1` + 显式 selftest 退出码检查（build.ps1 内嵌 selftest 但非零退出不 throw，需单独把关）
  - linux：cmake 默认生成器 + `./build/axr selftest`
- `CMakeLists.txt`：非 WIN32 链 `Threads::Threads`（httplib 线程池需要）
- 已本地 WSL（g++ 15，原生盘）验证：编译零错误、selftest 213 全过、长链 bench 12ms

### P1-4 转发层与子进程跨平台 — commit `df4ca26`
- `src/proxy/forward.cpp` 拆为 `forward_winhttp.cpp`（WinHTTP，`#ifdef _WIN32`）+ `forward_curl.cpp`（libcurl easy handle，NOSIGNAL=1、回调只写 sink、XFERINFO abortCheck、LOW_SPEED 1/300）
- `runner.cpp` 拆出 `spawn_win.cpp`（CreateProcess）+ `spawn_posix.cpp`（fork+execve；`extern char** environ` 必须全局作用域），PATH 搜索 `resolveExecutable`
- CMake：WIN32 链 winhttp；否则 `find_package(CURL REQUIRED)` 链 `CURL::libcurl`
- 验收：WSL 原生盘 Linux 构建 + selftest 213 全过 + mock_openai/fake_agent e2e（非流式 4 steps/3 tools、流式 chunked + sse 落盘）

### P1-5 (3) e2e 参数化 — commit `b9464fa`
- `tests/e2e_proxy.ps1`：去掉硬编码 `D:\env\axr\env.ps1`（仅 `AXR_ENV` 显式设置时 dot-source）；新增 `AXR_BUILD_DIR` / `AXR_PYTHON` 覆盖
- 新增 `tests/e2e_proxy.sh`：与非 ps1 等价的非流式/流式验收（curl 探活、events/meta 断言、非零退出即失败）
- PS 5.1 坑：原生 stderr 经 `2>&1` + `$ErrorActionPreference=Stop` 会抛 NativeCommandError，捕获处临时切 Continue

### P1-5 (4) README 构建命令 — commit `040255f`
- README 增加「构建（Linux/macOS）」：cmake 三条命令 + libcurl4-openssl-dev 依赖说明 + e2e 入口

### P1-6 文档收敛 — commit `beaf6f7`
- `docs/agent-xray-design.md` 改名 `docs/design-history.md`，开头加历史参考说明；删除「简历话术」章节（§13）
- 新增 `docs/ARCHITECTURE.md`（33 行：模块边界/数据流/关键类型/线程模型/平台边界）
- README 顶部加「支持矩阵」表；「零改造」文案补「不挑框架，但需走 OpenAI 兼容 chat completions」

### CI 修复（P1-4 验收项） — commit `9c7e73e`
- 双平台 CI 从未绿过，两条根因：windows job 实为 PowerShell 5.1 执行 build.ps1，无 BOM 的 UTF-8 在 cp1252 下中文注释字节变弯引号导致解析失败 → 补 UTF-8 BOM；linux job 缺 libcurl 开发包 → ci.yml 加 apt 步骤
- 现在 windows + linux 两个 job 全绿（linux 1m0s / windows 1m47s）

### P2-7 工具失败判定分级 — commit `43ae5a0`
- `ToolExec.isError` → `errorKind = none|inferred|confirmed`；判定下沉到 `chain/superstep.cpp::classifyToolError`：
  结果 JSON `error` / 协议层 `is_error` → `confirmed`，文本词表命中 → `inferred`
- 内置词表从 webui 迁入链层；`proxy.tool_error_patterns`（行内/多行列表）非空时整体覆盖内置词表
- Web UI：confirmed 红 / inferred 琥珀（流程图、甘特、标签、详情、横幅）；mermaid 两类都标 fail；旧 run.json 的 `is_error` 回退映射为 confirmed
- 顺带修复：`upstreams:` 列表之后的 proxy 键（如 `inject_usage`）被吞进最后一个 upstream 的问题
- 验收：Windows / WSL Linux selftest 229 全过

### P2-8 inject_usage 默认开 + 自动降级 — commit `3b442f9`
- `proxy.inject_usage` 默认改为 true（config.h / config.yaml / README 同步）
- 上游流式请求若返回 400 且响应体含 `stream_options`：`injectStreamUsage` 的那次转发先压住响应，
  去掉注入后重试一次；客户端无感，`meta.json` 记 `usage_injected:false`（`rewritten` 仍记 true）
- 降级判定抽成 `inject.cpp::shouldRetryWithoutUsage`，selftest 覆盖默认开关/400/500 分支
- e2e：mock_openai 新增 `__mode/usage_reject`，Windows/Linux 各加一条降级用例
- 验收：selftest 234 全过；Windows e2e 与 WSL `e2e_proxy.sh` 全过

### P2-9 区分 0 token 与未观测 — commit `6ef55f6`
- `Usage.observed`：`parse/openai.cpp` / `parse/sse.cpp` 仅在上游真的给了 `usage` 对象时置 true，
  上游明确返回 0 token 也算观测到；旧 `run.json` 无该字段时按"数值非零"回退
- 链层 `run.total.observed` 按 Step OR 聚合；`finalize`/`meta.json`/`run_index` 增 `usage_observed`（旧 meta 回退看 total_tokens）
- 展示层未观测时统一显示 `—`：`axr list/show/run/serve/replay/diff`、`report.md`（Token 表逐 Step 也是 `—`）、`graph.mmd`、时间线、Web UI（侧栏/chips/流程图/明细表）
- selftest：`testUsageObserved` 覆盖有 usage / 无 usage / 明确 0 token 三分支 + 序列化回环
- 验收：Windows / WSL Linux selftest 240 全过；WSL `e2e_proxy.sh` 全过（312 tokens 仍正常显示）

### P2-10 run id 防撞 — commit `2b703b9`
- `src/chain/run.cpp`：`run.id` 尾部追加 4 位 16 进制序号（`YYYYMMDD-HHMMSS-rootHash-0001`）
- 用建链内 run 序号而非随机数：实时视图与 replay 反复建链时 id 稳定，同时同一链内必不重复
- 旧的不带序号的 run id 仍按字符串原样可读（代码不校验格式）
- selftest：T14 增加 id 格式、同链唯一、重复建链稳定三条断言
- 验收：Windows / WSL Linux selftest 243 全过

## 三、关键代码事实（供下一个对话快速定位）

- 转发层平台实现：`src/proxy/forward_winhttp.cpp`（`#ifdef _WIN32`）+ `src/proxy/forward_curl.cpp`（其它平台，libcurl）
- 子进程：`src/proxy/spawn_win.cpp`（CreateProcess）+ `src/proxy/spawn_posix.cpp`（fork+execve，`extern char** environ` 需全局作用域）
- Windows 头文件引用全部有 `#ifdef _WIN32` 保护（config.cpp、time_util.cpp、cmd_serve.cpp、main.cpp、run_dir.cpp 等）
- `webUiRun` 调用点：`src/proxy/server.cpp` 已传 `?run=`；`cmd_selftest.cpp` 4 处
- `buildChainFromEvents` 返回 `ChainResult`（含 runs/links/retry/fuzzy + 三个降级计数）
- `finalizeRun` 主 run 选择：步数最多，并列取最早

## 四、待做任务（按工作单顺序）

> 工作单建议的批次顺序：第一批、第二批均已完成，从第三批继续。

### 第二批（已完成，见第二节）

### 第三批（P2-7 ~ P2-12 低成本准确性）

> P2-7、P2-8、P2-9、P2-10 已完成（见第二节）。

- **P2-11 工具耗时来源标注**：`ToolExec.durationSource = inferred|mcp|otel`；UI "~" 只用于 inferred
- **P2-12 修正 OLLAMA_HOST 注入**：删除注入，或 README 明确用 `/ollama/v1`；e2e 覆盖

### 第四批（P2-13 ~ P2-16 隐私/性能/运维）
- **P2-13 采集模式与隐私**：`proxy.capture: full|summary`（summary 只存长度+sha256 前16位）；脱敏正则加 Bearer/api_key/x-api-key；run 目录 0700；非回环 bind 需 `--allow-remote`
- **P2-14 截断事件不参与解析**：`truncated=true` 的 req/resp 不 parse，计入 parse_degraded（部分已在 P0-3 计数，但仍可能进 parse——需确认并修）
- **P2-15 实时视图增量解析 + serve 自动收尾**：webui 对 (runDir, size, mtime) 缓存 + 续读；serve 空闲 N 分钟 finalize 当前 session
- **P2-16 retention 自动执行**：serve 启动自动 prune；`axr prune --auto`

### 第五批（P3，按产品方向选一个，不同时做）
- **P3-17 OTel/Langfuse 导出**：`axr export --format otlp-json`（gen_ai.* 语义）、可选 langfuse
- **P3-18 MCP 代理通道**：`McpProxySource` 复用 Source 抽象，实测工具耗时与失败
- **P3-19 协议扩展**：Responses API、Anthropic Messages 解析还原（工作量大，建议有外部用户后再做）

## 五、最终验收清单（全部勾选才算项目完成）

- [x] 两个共享 system prompt 的独立任务被切成 2 个 run（t14）
- [x] meta.json 含 runs[] 与 primary；Web UI 侧栏多 run；`axr list --all` 正常
- [x] 非 chat/completions 请求不污染 run，meta 有计数与 UI 提示
- [x] Linux/macOS 能采集（axr serve + axr run），CI 双平台绿（P1-4）
- [x] 存在 LICENSE，README 有支持矩阵与平台构建命令（顶部矩阵表 + Linux/macOS 构建一节均已就位）
- [ ] 工具失败区分 confirmed/inferred；usage 缺失显示 "—"（P2-7 / P2-9）
- [ ] inject_usage 默认开且自动降级；OLLAMA_HOST 修正（P2-8 / P2-12）
- [ ] run id 防撞；截断事件不参与解析（P2-10 / P2-14）
- [ ] summary 采集模式；retention 自动执行；UI 增量刷新（P2-13 / P2-15 / P2-16）
- [x] 设计文档降级为历史文档，架构文档与实现一致（P1-6）

## 六、已知坑与注意事项

1. **PowerShell 管道缓冲**：`.\build\axr.exe selftest` 崩溃前的输出可能被吞；调试时可在 `cmdSelftest` 开头临时加 `std::setvbuf(stdout,nullptr,_IONBF,0)`
2. **NativeCommandError**：stderr 内容会被 PowerShell 显示为错误红框，但 exit 0 属正常（selftest 日志走 stderr）
3. **selftest bench < 100ms**：在 WSL 的 `/mnt/d`（drvfs）上会因 I/O 慢超时（实测 130ms）；必须在原生 ext4 上跑（CI 不受影响）
4. **P2-14 与 P0-3 重叠**：P0-3 已对 truncated 事件计数，但 `truncated=true` 的事件目前仍会被 `parseOpenAiRequest` 尝试解析（可能产生空 pair）。P2-14 需在 events_reader 里对 truncated 的 req 跳过 parse、直接计入 parseFailed，而不是只计数
5. **CI windows job 的 selftest**：build.ps1 末尾虽调用 selftest，但非零退出不会让脚本 fail，所以 ci.yml 里单独加了一步显式检查退出码
6. **commit 推送**：GitHub token 需要 `workflow` scope 才能推 `.github/workflows/*`（`gh auth refresh -s workflow`）
