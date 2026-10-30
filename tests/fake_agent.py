#!/usr/bin/env python3
# 测试用最小 agent（无第三方依赖）：通过 OPENAI_BASE_URL 指向 axr 代理，
# 走 3 轮工具调用 + 1 轮最终回答，用来验证录制与链还原（§10 M2）。
import json
import os
import sys
import urllib.request

BASE = os.environ.get("OPENAI_BASE_URL", "")
API_KEY = "sk-0123456789abcdefghijklmnopqrstuvwxyz"

TOOLS = [{
    "type": "function",
    "function": {
        "name": "get_weather",
        "description": "查询城市天气",
        "parameters": {
            "type": "object",
            "properties": {"city": {"type": "string"}},
            "required": ["city"],
        },
    },
}]


def post(messages, stream=False):
    payload = json.dumps({
        "model": "deepseek-chat",
        "messages": messages,
        "tools": TOOLS,
        "stream": stream,
    }).encode("utf-8")
    req = urllib.request.Request(
        BASE + "/chat/completions",
        data=payload,
        headers={"Content-Type": "application/json", "Authorization": "Bearer " + API_KEY},
    )
    with urllib.request.urlopen(req, timeout=30) as resp:
        return resp.read().decode("utf-8")


def main():
    if not BASE:
        print("OPENAI_BASE_URL 未被注入", file=sys.stderr)
        return 2
    print("[agent] base =", BASE)

    if "--stream" in sys.argv:
        raw = post([{"role": "user", "content": "北京天气"}], stream=True)
        print("[agent] sse bytes =", len(raw))
        return 0

    messages = [{"role": "user", "content": "帮我查一下北京今天天气"}]
    for _ in range(3):
        resp = json.loads(post(messages))
        msg = resp["choices"][0]["message"]
        calls = msg.get("tool_calls") or []
        messages.append({"role": "assistant", "content": msg.get("content"), "tool_calls": calls})
        for call in calls:
            messages.append({
                "role": "tool",
                "tool_call_id": call["id"],
                "content": json.dumps({"temp": "21C", "weather": "晴"}, ensure_ascii=False),
            })
        if not calls:
            break

    final = json.loads(post(messages))
    print("[agent] final:", final["choices"][0]["message"]["content"])
    return 0


if __name__ == "__main__":
    sys.exit(main())
