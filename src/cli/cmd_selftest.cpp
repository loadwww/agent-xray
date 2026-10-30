#include "cli/cli.h"

#include <filesystem>
#include <functional>
#include <iostream>
#include <map>
#include <mutex>

#include "chain/fuzzy.h"
#include "chain/link.h"
#include "chain/run.h"
#include "common/hash.h"
#include "common/log.h"
#include "config/config.h"
#include "graph/mermaid.h"
#include "proxy/event.h"
#include "proxy/recorder.h"
#include "proxy/redact.h"
#include "proxy/route.h"
#include "store/events_reader.h"
#include "store/run_dir.h"

namespace axr {
namespace {

struct TestCtx {
    int pass = 0;
    int fail = 0;
    void check(bool cond, const std::string& what) {
        if (cond) {
            ++pass;
        } else {
            ++fail;
            std::cout << "  FAIL: " << what << "\n";
        }
    }
};

const char* kFixturesArg = "--fixtures";

std::string fixtureDir(const std::vector<std::string>& args) {
    for (std::size_t i = 0; i + 1 < args.size(); ++i) {
        if (args[i] == kFixturesArg) return args[i + 1];
    }
    return "tests/fixtures";
}

int toolCount(const Run& r) {
    int n = 0;
    for (const auto& s : r.steps) n += static_cast<int>(s.tools.size());
    return n;
}

void testHash(TestCtx& t) {
    t.check(sha256Hex("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
            "sha256(\"abc\") 匹配标准向量");
    t.check(sha256Hex("").substr(0, 16) == "e3b0c44298fc1c14", "sha256(\"\") 匹配标准向量");
}

void testModelRoundtrip(TestCtx& t) {
    Run run;
    run.id = "20261030-143012-a1b2c3d4";
    run.rootHash = "a1b2c3d4";
    run.startedAt = "2026-10-30T14:30:12.100+08:00";
    run.endedAt = "2026-10-30T14:30:20.000+08:00";
    run.incomplete = true;
    run.missing = {"step 2: 消息数未增长"};
    run.total = Usage{100, 50, 10};

    LLMCall call;
    call.id = "h-1";
    call.model = "deepseek-chat";
    call.endpoint = "https://api.deepseek.com/v1/chat/completions";
    call.params = nlohmann::json{{"temperature", 0.7}, {"max_tokens", 1024}};
    Message m1;
    m1.role = Role::System;
    m1.content.push_back(Content{"text", "you are helpful", "", 0, ""});
    Message m2;
    m2.role = Role::User;
    m2.content.push_back(Content{"text", "你好", "", 0, ""});
    call.messages = {m1, m2};

    LLMResponse resp;
    resp.callId = "h-1";
    resp.text = "hi";
    resp.usage = Usage{10, 5, 0};
    resp.finishReason = "stop";

    ToolExec te;
    te.callId = "call_1";
    te.name = "get_weather";
    te.arguments = "{\"city\":\"北京\"}";
    te.result = "晴";
    te.durationMs = 1234;

    Step s;
    s.index = 0;
    s.call = call;
    s.response = resp;
    s.tools = {te};
    s.tokens = 15;
    s.kind = "llm";
    run.steps = {s};

    nlohmann::json j1 = run;
    Run back = j1.get<Run>();
    nlohmann::json j2 = back;
    t.check(j1.dump() == j2.dump(), "Run 序列化→反序列化→再序列化 一致");
    t.check(back.steps[0].tools[0].name == "get_weather", "反序列化保留工具名");
    t.check(back.total.promptTokens == 100, "反序列化保留 usage");
    t.check(back.missing.size() == 1, "反序列化保留 missing");
}

void testFixture(const std::string& dir, const std::string& name, TestCtx& t,
                 const std::function<void(const ChainResult&)>& assertions) {
    auto chain = buildChainFromEvents(dir + "/" + name);
    if (!chain.ok()) {
        t.check(false, name + " 可加载: " + chain.err);
        return;
    }
    assertions(chain.value);
}

void testM2Units(TestCtx& t) {
    // 配置解析：upstreams / chain / redact / storage 覆盖默认值
    {
        const std::string yaml = R"(
proxy:
  listen: 8081
  bind: 127.0.0.1
  max_body_bytes: 1024
  upstreams:
    - match: "/v1"
      url: "http://127.0.0.1:9099/v1"
      name: "mock"
chain:
  run_gap_minutes: 5
  fuzzy_threshold: 0.85
redact:
  headers: [authorization]
  patterns:
    - "sk-[A-Za-z0-9]{20,}"
storage:
  root: "~/.xray"
)";
        auto cfg = parseConfigText(yaml);
        t.check(cfg.ok(), "M2 配置可解析");
        if (cfg.ok()) {
            t.check(cfg.value.proxy.listen == 8081, "M2 配置 listen");
            t.check(cfg.value.proxy.maxBodyBytes == 1024, "M2 配置 max_body_bytes");
            t.check(cfg.value.proxy.upstreams.size() == 1, "M2 配置 upstreams 覆盖默认");
            t.check(cfg.value.proxy.upstreams[0].name == "mock", "M2 配置 upstream name");
            t.check(cfg.value.chainOptions().runGapMinutes == 5, "M2 配置 chain 生效");
            t.check(cfg.value.redact.headers.size() == 1, "M2 配置 redact.headers 覆盖默认");
            t.check(cfg.value.redact.patterns.size() == 1, "M2 配置 redact.patterns");
            t.check(!cfg.value.storage.root.empty() && cfg.value.storage.root != "~/.xray", "M2 配置 ~ 展开");
        }
    }

    // 上游路由：最长前缀 + 剥离 match 前缀
    {
        std::vector<UpstreamDef> ups = {
            {"/v1", "https://api.deepseek.com/v1", "deepseek"},
            {"/v1/chat", "http://127.0.0.1:11434/v1", "ollama"},
            {"/ollama", "http://127.0.0.1:11434/v1", "ollama2"},
        };
        auto r1 = resolveUpstream("/v1/models", ups);
        t.check(r1.matched && r1.url == "https://api.deepseek.com/v1/models", "M2 路由 /v1 拼接");
        auto r2 = resolveUpstream("/v1/chat/completions", ups);
        t.check(r2.matched && r2.upstreamName == "ollama", "M2 路由取最长前缀");
        t.check(r2.url == "http://127.0.0.1:11434/v1/completions", "M2 路由剥离前缀");
        auto r3 = resolveUpstream("/v1beta/models", ups);
        t.check(!r3.matched, "M2 路由边界检查（/v1beta 不匹配 /v1）");
        auto r4 = resolveUpstream("/nope", ups);
        t.check(!r4.matched, "M2 未匹配路径");
    }

    // 脱敏：头大小写不敏感 + body 正则
    {
        std::map<std::string, std::string> h{{"Authorization", "Bearer sk-x"}, {"content-type", "application/json"}};
        auto out = redactHeaders(h, {"authorization", "x-api-key"});
        t.check(out["Authorization"] == kRedacted, "M2 authorization 头脱敏");
        t.check(out["content-type"] == "application/json", "M2 非敏感头保留");

        const std::string body = R"({"api_key":"sk-0123456789abcdefghijklmnopqrstuv"})";
        const std::string red = redactBody(body, {"sk-[A-Za-z0-9]{20,}"});
        t.check(red.find("sk-0123") == std::string::npos, "M2 body 中 sk- 密钥被替换");
        t.check(red.find(kRedacted) != std::string::npos, "M2 body 脱敏占位符");
    }

    // 事件 JSON：req / resp 字段齐全，不写无关字段
    {
        Event e;
        e.seq = 7;
        e.ts = "2026-10-30T14:30:12.100+08:00";
        e.kind = "req";
        e.hop = "h-1";
        e.upstream = "https://api.deepseek.com/v1/chat/completions";
        e.reqBody = "{}";
        e.reqHeaders = {{"authorization", "***"}};
        auto j = e.toJson();
        t.check(j["seq"] == 7 && j["kind"] == "req", "M2 事件基础字段");
        t.check(j.contains("req_body") && j.contains("req_headers"), "M2 req 事件字段");
        t.check(!j.contains("data") && !j.contains("status"), "M2 req 事件不写无关字段");

        Event r;
        r.kind = "resp";
        r.hop = "h-1";
        r.status = 200;
        r.latencyMs = 1200;
        auto jr = r.toJson();
        t.check(jr["status"] == 200 && jr["latency_ms"] == 1200, "M2 resp 事件字段");
    }

    // 录制器：异步落盘可读回、seq 递增
    {
        const std::filesystem::path tmp =
            std::filesystem::temp_directory_path() / ("axr-selftest-" + makeSessionId());
        std::filesystem::create_directories(tmp);
        const std::string file = (tmp / "events.jsonl").string();
        {
            Recorder rec(file, 1LL << 20, 1000);
            for (int i = 0; i < 3; ++i) {
                Event e;
                e.kind = "req";
                e.hop = "h-" + std::to_string(i);
                e.reqBody = "{}";
                rec.push(e);
            }
            rec.flush();
            t.check(rec.bytes() > 0, "M2 录制器写入非空");
        }
        auto events = readEventsFile(file);
        t.check(events.ok() && events.value.size() == 3, "M2 录制事件可读回 3 条");
        if (events.ok() && events.value.size() == 3) {
            t.check(events.value[0].seq == 1 && events.value[2].seq == 3, "M2 事件 seq 递增");
            t.check(events.value[1].hop == "h-1", "M2 事件顺序保持");
            t.check(!events.value[0].ts.empty(), "M2 录制器自动补时间戳");
        }
        std::filesystem::remove_all(tmp);
    }

    // 会话 id 形如 YYYYMMDD-HHMMSS-xxxx
    {
        const std::string id = makeSessionId();
        t.check(id.size() == 20 && id[8] == '-' && id[15] == '-', "M2 会话 id 格式");
        t.check(expandHome("~/.xray") != "~/.xray", "M2 ~ 展开为真实目录");
    }
}

} // namespace

int cmdSelftest(const std::vector<std::string>& args) {
    TestCtx t;
    const std::string dir = fixtureDir(args);
    std::cout << "fixtures: " << dir << "\n";

    testHash(t);
    testModelRoundtrip(t);
    testM2Units(t);

    // T1 普通对话
    testFixture(dir, "t1_simple", t, [&](const ChainResult& cr) {
        t.check(cr.runs.size() == 1, "T1 切出 1 个 run");
        if (cr.runs.empty()) return;
        const Run& r = cr.runs[0];
        t.check(r.steps.size() == 1, "T1 1 Step");
        t.check(r.steps[0].tools.empty(), "T1 无工具");
        t.check(r.total.promptTokens == 30 && r.total.completionTokens == 10, "T1 usage 正确");
        t.check(!r.incomplete, "T1 完整");
        t.check(r.steps[0].response.text.find("你好") != std::string::npos, "T1 文本正确");
    });

    // T2 单工具
    testFixture(dir, "t2_single_tool", t, [&](const ChainResult& cr) {
        t.check(cr.runs.size() == 1, "T2 切出 1 个 run");
        if (cr.runs.empty()) return;
        const Run& r = cr.runs[0];
        t.check(r.steps.size() == 2, "T2 2 Step");
        t.check(r.steps[0].tools.size() == 1, "T2 第一步 1 个工具");
        t.check(r.steps[0].tools[0].name == "get_weather", "T2 工具名");
        t.check(r.steps[0].tools[0].arguments == "{\"city\":\"北京\"}", "T2 工具参数");
        t.check(r.steps[0].tools[0].result.find("晴") != std::string::npos, "T2 工具结果");
        t.check(r.steps[0].call.tools.size() == 1, "T2 请求里的 tool 定义被解析");
        t.check(r.steps[1].kind == "final", "T2 第二步是 final");
        t.check(r.steps[1].tokens == 95, "T2 第二步 token 数");
    });

    // T3 并行工具
    testFixture(dir, "t3_parallel_tools", t, [&](const ChainResult& cr) {
        t.check(cr.runs.size() == 1, "T3 1 run");
        if (cr.runs.empty()) return;
        const Run& r = cr.runs[0];
        t.check(r.steps.size() == 2, "T3 2 Step（并行工具合并）");
        t.check(r.steps[0].tools.size() == 2, "T3 第一步 2 个 ToolExec");
        t.check(r.steps[0].response.toolCalls.size() == 2, "T3 响应含 2 个 tool_call");
    });

    // T4 流式累积
    testFixture(dir, "t4_stream", t, [&](const ChainResult& cr) {
        t.check(cr.runs.size() == 1, "T4 1 run");
        if (cr.runs.empty()) return;
        const Run& r = cr.runs[0];
        t.check(r.steps.size() == 2, "T4 2 Step");
        t.check(r.steps[0].response.streamed, "T4 第一步是流式");
        t.check(r.steps[0].response.text == "北京", "T4 流式文本拼接");
        t.check(r.steps[0].response.toolCalls.size() == 1, "T4 流式 tool_call 1 个");
        t.check(r.steps[0].response.toolCalls[0].arguments == "{\"city\":\"北京\"}",
                "T4 arguments 分片拼接为完整 JSON");
        t.check(r.steps[0].tools.size() == 1, "T4 工具执行被还原");
        t.check(r.steps[0].response.usage.promptTokens == 40, "T4 末块 usage 生效");
        t.check(r.steps[1].response.text == "北京今天晴。", "T4 第二步流式文本");
    });

    // T5 缺失中间请求 → incomplete
    testFixture(dir, "t5_missing", t, [&](const ChainResult& cr) {
        t.check(cr.runs.size() == 1, "T5 仍能成链（1 run）");
        if (cr.runs.empty()) return;
        t.check(cr.runs[0].incomplete, "T5 标记 incomplete");
        t.check(!cr.runs[0].missing.empty(), "T5 missing 有内容");
    });

    // T6 跨 run 切分
    testFixture(dir, "t6_two_runs", t, [&](const ChainResult& cr) {
        t.check(cr.runs.size() == 2, "T6 间隔 45 分钟切成 2 个 run");
    });

    // 30 条请求的长链
    testFixture(dir, "deepseek_tool_2", t, [&](const ChainResult& cr) {
        t.check(cr.runs.size() == 1, "长链 1 run");
        if (cr.runs.empty()) return;
        const Run& r = cr.runs[0];
        t.check(r.steps.size() == 30, "长链 30 Step");
        t.check(toolCount(r) == 30, "长链 30 次工具执行");
        t.check(!r.incomplete, "长链完整");
    });

    // T7 模糊匹配：改写系统提示词导致前缀失配
    {
        auto path = resolveEventsPath(dir + "/deepseek_tool_2");
        auto events = readEventsFile(path);
        if (events.ok()) {
            auto pairs = assembleCalls(events.value);
            if (pairs.ok() && pairs.value.size() > 6) {
                auto& msgs = pairs.value[5].call.messages;
                if (!msgs.empty() && !msgs[0].content.empty()) msgs[0].content[0].text += "x";
                ChainResult cr = buildRuns(pairs.value);
                t.check(cr.fuzzyCount > 0, "T7 前缀失配后走模糊匹配");
                t.check(cr.runs.size() == 1, "T7 仍然归到同一 run");
            } else {
                t.check(false, "T7 夹具加载失败");
            }
        } else {
            t.check(false, "T7 夹具加载失败: " + events.err);
        }
    }

    // T8 特殊字符转义
    {
        const std::string evil = "search/web?q=\"a&b\"";
        const std::string esc = escapeMermaidLabel(evil);
        t.check(esc.find("&quot;") != std::string::npos, "T8 双引号被转义");
        t.check(esc.find("&amp;") != std::string::npos, "T8 & 被转义");
        t.check(esc.find("\"") == std::string::npos, "T8 转义后无裸双引号");

        Run r;
        r.steps.resize(2);
        r.steps[0].call.model = "deepseek-chat";
        r.steps[0].response.latencyMs = 1200;
        r.steps[0].tokens = 100;
        r.steps[0].response.toolCalls.push_back(ToolCall{"call_1", evil, "{}"});
        ToolExec te;
        te.callId = "call_1";
        te.name = evil;
        te.arguments = "{\"q\":\"a&b\"}";
        r.steps[0].tools.push_back(te);
        r.steps[1].call.model = "deepseek-chat";
        r.steps[1].kind = "final";
        const std::string mmd = toMermaid(r);
        t.check(mmd.find("S0") != std::string::npos && mmd.find("T0_0") != std::string::npos,
                "T8 节点 id 使用 S/T 编号");
        t.check(mmd.find("\"a&b\"") == std::string::npos, "T8 图中无未转义的工具名");
        t.check(mmd.find("flowchart TD") == 0, "T8 输出以 flowchart TD 开头");
    }

    std::cout << "\n结果: " << t.pass << " passed, " << t.fail << " failed\n";
    return t.fail == 0 ? 0 : 1;
}

} // namespace axr
