## Why it matters

（一句话说明这个版本解决了什么，例如：修掉了 Linux 下 Ctrl+C 退出留下半成品目录的问题。）

## Install

Linux x86_64：

```sh
curl -fsSL https://raw.githubusercontent.com/loadwww/agent-xray/master/scripts/install.sh | sh
axr demo
```

Windows：

```powershell
irm https://raw.githubusercontent.com/loadwww/agent-xray/master/scripts/install.ps1 | iex
```

Docker：

```sh
docker run --rm -p 8787:8787 ghcr.io/loadwww/agent-xray:latest
```

离线 demo（不需要 API key、不联网）：

```sh
axr demo          # 打开 Web UI 看两次示例 run
axr demo --diff   # 终端直接看两次运行的差异
```

## Known issues

- 只还原 OpenAI 兼容 `chat/completions`（含流式 SSE）；Responses API、Anthropic `/v1/messages` 原样转发但不还原。
- 耗时与工具失败是**推断**，不是被测代码主动上报的；结果里会标注来源。
- 单机工具：不做评测、Prompt 管理、团队协作与云端。

## Thanks

Issue / 复现 / 文档修正都算贡献。反馈请开 issue，或到 Discussions 里贴你的 Agent 配置。
