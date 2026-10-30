# AgentXray

零改造 Agent 执行流程逆向与可视化系统。CLI 名：`axr`。实现语言：C++17，采集模式：Mode A（反向代理）。

> 完整施工规格见 [docs/agent-xray-design.md](docs/agent-xray-design.md)。

## 构建（Windows / MSVC）

环境隔离在 `D:\env\axr`，代码区不存放任何工具链。

```powershell
. D:\env\axr\env.ps1          # 初始化 MSVC + WinSDK + CMake + Ninja
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

产物：`build\axr.exe`

一键回归（环境初始化 + 构建 + 自检）：

```powershell
powershell -ExecutionPolicy Bypass -File tests\run_tests.ps1
```

M2 端到端验收（离线 mock，无需任何 API key）：

```powershell
powershell -ExecutionPolicy Bypass -File tests\e2e_proxy.ps1
```

## 已实现命令

```powershell
# M1：离线回放与自检
build\axr.exe selftest                                  # 79 条断言：hash / model / SSE / 链 / fuzzy / 配置 / 路由 / 脱敏 / 录制
build\axr.exe replay tests\fixtures\deepseek_tool_2 --format mermaid --out build\graph.mmd
build\axr.exe replay tests\fixtures\t2_single_tool --format json

# M2：真流量接入
build\axr.exe serve --config config.yaml                        # 前台代理，Ctrl+C 结束并落盘
build\axr.exe run -- python my_agent.py                         # 起代理 + 跑子进程 + 自动收尾
build\axr.exe run --stream -- python my_agent.py                # 同上（录制 SSE 逐 chunk）
build\axr.exe run --config tests\proxy_test.yaml -- python tests\fake_agent.py
```

## M2 工作方式

`axr run` 会在本机起一个反向代理（默认 `127.0.0.1:8080`），把
`OPENAI_BASE_URL` / `OPENAI_API_BASE` / `DEEPSEEK_BASE_URL`（→ `.../v1`）和
`OLLAMA_HOST`（→ `.../ollama`）注入子进程环境，因此**客户端代码零改造**。
每条上游请求/响应（含 SSE 分片）都写入会话目录：

```
<storage.root>/runs/<sessionId>/
  events.jsonl   # req / sse_chunk / sse_end / resp / err
  run.json       # 还原出的 Run 步骤
  graph.mmd      # Mermaid 流程图
  meta.json      # 摘要（步数、工具数、token、完整性、模型）
```

上游地址在 `config.yaml` 的 `proxy.upstreams` 配置。路由规则为**最长前缀匹配 + 剥离 match 前缀**：

```yaml
proxy:
  upstreams:
    - match: "/v1"        # /v1/chat/completions → https://api.deepseek.com/v1/chat/completions
      url: "https://api.deepseek.com/v1"
      name: "deepseek"
    - match: "/ollama"    # /ollama/chat/completions → http://127.0.0.1:11434/v1/chat/completions
      url: "http://127.0.0.1:11434/v1"
      name: "ollama"
```

落盘副本自动脱敏：`authorization` / `x-api-key` / `api-key` 头置为 `***`，
body 中 `sk-...` 形态密钥按正则替换；原始请求照常转发，不影响上游鉴权。

## 与设计文档的偏离

| 项 | 设计文档 | 实现 | 原因 |
| --- | --- | --- | --- |
| HTTP 转发库 | libcurl（§7.1.3） | WinHTTP（`winhttp.lib`） | 本机无 libcurl 开发包/OpenSSL；WinHTTP 走系统 Schannel，零第三方依赖 |
| 路由拼接 | 示例间语义冲突 | 统一为"剥离 match 前缀 + url 作为上游基址" | 使 deepseek 与 ollama 两种上游同时可用 |
| SSE 客户端体验 | 流式透传 | M2 采用 S1 非流式缓冲：上游逐 chunk 录制，响应聚合成完整包返回客户端 | 设计文档 §7.1.4 建议 M2 先用 S1；录制保真度不受影响 |

## 状态

- [x] 阶段 0：环境搭建（MSVC 14.41 / WinSDK 10.0.22621 / CMake 3.29.5 / Ninja 1.11）
- [x] 阶段 1（M1）：离线闭环 — mock fixture → parse → chain → mermaid
- [x] 阶段 2（M2）：真流量接入（反向代理 + 录制 + `axr run`）
- [ ] 阶段 3（M3）：分析质量（superstep / fuzzy / validate / cost / report）
- [ ] 阶段 4（M4）：工程化（diff / prune / list / export / 性能）
