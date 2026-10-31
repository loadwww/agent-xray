# AgentXray 架构说明（与当前实现一致）

> 面向维护者。施工期设计规格书（含历史决策）已归档为 [design-history.md](design-history.md)。

## 模块边界

| 目录 | 职责 |
|---|---|
| `src/proxy` | httplib 服务、路由（config 最长前缀匹配剥离）、上游转发（Windows WinHTTP / 其它平台 libcurl）、流式桥接、录制、脱敏、`axr run` 子进程启动 |
| `src/parse` | OpenAI chat completions 请求/响应解析、SSE 状态机，还原 `Step` / `ToolExec` |
| `src/chain` | 滚动前缀哈希建链、run 切分、superstep 归并、模糊匹配、完整性校验 |
| `src/store` | `events.jsonl` 读取与组装、finalize（run.json / meta.json / report.md）、run 目录与索引 |
| `src/graph` | Mermaid 流程图与甘特时间线 |
| `src/ui` | 内嵌 Web UI（HTML/JS）、markdown 报告 |
| `src/cli` | serve / run / show / report / list / export / diff / prune / replay / selftest |
| `src/common` `src/config` `src/diff` | 日志、哈希、时间、配置解析、两次 run 结构对比 |

## 数据流

```
Agent → serve|run（httplib）→ route → 每请求一个桥接线程 → forward（原样转上游，流式边收边发）
                                      └→ recorder 队列（脱敏后）→ events.jsonl（append-only）

会话收尾 finalize：events_reader → parse → chain → run.json + meta.json + graph.mmd + report.md
Web UI（/__xray）直接读 run 目录；采集中的 run 每 3s 从 events.jsonl 增量重建。
```

## 关键类型

- `RawEvent`（store/events_reader.h）：req / resp / sse_chunk / sse_end / err，带 hop / seq
- `Run` / `Step` / `ToolExec`（parse/model.h）：执行图与工具调用（耗时推断、失败判定）
- `ChainResult`（chain/run.h）：建链产物（runs / links + 解析降级计数）
- `RunMeta` / `RunBrief`（store/）：meta.json 与 run 列表摘要
- 中间态尽量用 nlohmann::json；落盘格式以 README 描述的 events.jsonl / run.json 为准

## 线程模型

- httplib 线程池接管连接；每个请求再起一个桥接线程做转发与 SSE 桥接，转发路径不串行
- `Recorder` 每会话一条写线程（条件变量队列）异步落盘，转发路径不等待磁盘
- `axr run`：代理监听线程与子进程（CreateProcess / fork+execve）并发，主线程等待退出后收尾

## 平台边界

- 转发层：Windows → `forward_winhttp.cpp`；Linux/macOS → `forward_curl.cpp`（libcurl）
- 子进程：`spawn_win.cpp` / `spawn_posix.cpp`；平台头文件引用均有 `#ifdef` 保护