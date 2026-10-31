#include "cli/cli.h"

#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <mutex>
#include <sstream>

#include "chain/fuzzy.h"
#include "chain/link.h"
#include "chain/run.h"
#include "chain/superstep.h"
#include "chain/validate.h"
#include "cli/cli_common.h"
#include "common/hash.h"
#include "common/log.h"
#include "common/utf8.h"
#include "config/config.h"
#include "diff/rundiff.h"
#include "graph/mermaid.h"
#include "parse/openai.h"
#include "proxy/event.h"
#include "proxy/inject.h"
#include "proxy/recorder.h"
#include "proxy/redact.h"
#include "proxy/route.h"
#include "store/events_reader.h"
#include "store/finalize.h"
#include "store/run_dir.h"
#include "store/run_index.h"
#include "ui/report.h"
#include "ui/webui.h"

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
    run.total = Usage{100, 50, 10, true};

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
    resp.usage = Usage{10, 5, 0, true};
    resp.finishReason = "stop";

    ToolExec te;
    te.callId = "call_1";
    te.name = "get_weather";
    te.arguments = "{\"city\":\"北京\"}";
    te.result = "晴";
    te.errorKind = "confirmed";
    te.durationMs = 1234;
    te.durationSource = "mcp";

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
    t.check(back.steps[0].tools[0].errorKind == "confirmed", "反序列化保留工具失败等级");
    t.check(back.steps[0].tools[0].durationSource == "mcp", "反序列化保留工具耗时来源");
    {
        // P2-11 旧 run.json 只有 approx 布尔 → 等价于 inferred；未知来源回落 inferred
        ToolExec legacy = nlohmann::json{{"duration_ms", 10}, {"approx", true}};
        t.check(legacy.durationSource == "inferred", "P2-11 旧 approx 字段回退为 inferred");
        ToolExec bogus = nlohmann::json{{"duration_source", "bogus"}};
        t.check(bogus.durationSource == "inferred", "P2-11 未知耗时来源回落为 inferred");
    }
    t.check(back.total.promptTokens == 100, "反序列化保留 usage");
    t.check(back.total.observed && back.steps[0].response.usage.observed, "反序列化保留 usage.observed");
    t.check(back.missing.size() == 1, "反序列化保留 missing");
}

void testFixture(const std::string& dir, const std::string& name, TestCtx& t,
                 const std::function<void(const ChainResult&)>& assertions) {
    // 发布二进制不带 fixtures：静默跳过，由 cmdSelftest 开头的提示统一说明
    if (!std::filesystem::exists(dir + "/" + name)) return;
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

    // §7.2.3 inject_usage：默认开（P2-8）+ 配置开关 + 流式请求体改写 + 400 自动降级
    {
        auto dflt = parseConfigText("proxy:\n");
        t.check(dflt.ok() && dflt.value.proxy.injectUsage, "P2-8 inject_usage 默认开启");
        auto off = parseConfigText("proxy:\n  inject_usage: false\n");
        t.check(off.ok() && !off.value.proxy.injectUsage, "P2-8 inject_usage 可显式关闭");
        t.check(shouldRetryWithoutUsage(400, R"({"error":{"message":"unknown field stream_options"}})"),
                "P2-8 上游 400 提到 stream_options 时降级重试");
        t.check(!shouldRetryWithoutUsage(400, R"({"error":{"message":"invalid model"}})"),
                "P2-8 其它 400 不重试");
        t.check(!shouldRetryWithoutUsage(500, "stream_options"), "P2-8 非 400 不重试");

        auto cfg = parseConfigText("proxy:\n  inject_usage: true\n");
        t.check(cfg.ok() && cfg.value.proxy.injectUsage, "M2 inject_usage 配置解析");

        std::string s = R"({"model":"m","stream":true})";
        t.check(injectStreamUsage(s) && s.find("\"include_usage\":true") != std::string::npos,
                "M2 流式请求补 include_usage");
        std::string ns = R"({"model":"m","stream":false})";
        t.check(!injectStreamUsage(ns) && ns == R"({"model":"m","stream":false})", "M2 非流式请求不改写");
        std::string has = R"({"stream":true,"stream_options":{"include_usage":true}})";
        t.check(!injectStreamUsage(has), "M2 已带 include_usage 不重复改写");
        std::string bad = "not json";
        t.check(!injectStreamUsage(bad) && bad == "not json", "M2 非 JSON 原样转发");
    }

    // model_map：客户端模型名 -> 上游模型名（AutoGPT 只认 gpt-4o 这类名字）
    {
        const std::string yaml = R"(
proxy:
  upstreams:
    - match: "/v1"
      url: "https://api.deepseek.com/v1"
      name: "deepseek"
      model_map:
        gpt-4o: deepseek-chat
        gpt-4-turbo: deepseek-chat
    - match: "/ollama"
      url: "http://127.0.0.1:11434/v1"
      name: "ollama"
)";
        auto cfg = parseConfigText(yaml);
        t.check(cfg.ok() && cfg.value.proxy.upstreams.size() == 2, "M2 model_map 配置可解析");
        if (cfg.ok() && cfg.value.proxy.upstreams.size() == 2) {
            const auto& m = cfg.value.proxy.upstreams[0].modelMap;
            t.check(m.size() == 2 && m.at("gpt-4o") == "deepseek-chat", "M2 model_map 映射项");
            t.check(cfg.value.proxy.upstreams[1].match == "/ollama" &&
                        cfg.value.proxy.upstreams[1].modelMap.empty(),
                    "M2 model_map 之后的 upstream 仍正确解析");
            auto r = resolveUpstream("/v1/chat/completions", cfg.value.proxy.upstreams);
            t.check(r.matched && r.modelMap.size() == 2, "M2 路由携带 model_map");
        }

        auto map = std::map<std::string, std::string>{{"gpt-4o", "deepseek-chat"}};
        std::string hit = R"({"model":"gpt-4o","messages":[]})";
        std::string applied;
        t.check(rewriteModel(hit, map, &applied) && hit.find("\"deepseek-chat\"") != std::string::npos,
                "M2 model_map 命中后改写 model 字段");
        t.check(applied == "gpt-4o -> deepseek-chat", "M2 model_map 记录改写来源");
        std::string miss = R"({"model":"deepseek-chat"})";
        t.check(!rewriteModel(miss, map) && miss == R"({"model":"deepseek-chat"})", "M2 model_map 未命中不改写");
        std::string bad = "not json";
        t.check(!rewriteModel(bad, map) && bad == "not json", "M2 model_map 非 JSON 原样转发");
        std::string noModel = R"({"input":"hi"})";
        t.check(!rewriteModel(noModel, map), "M2 model_map 请求无 model 字段不改写");
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
        t.check(expandHome("~/.xray").find(".xra/") == std::string::npos, "M2 ~/.xray 展开未把 .xray 截断");
        t.check(expandHome("~/logs") == expandHome("~") + "/logs", "M2 ~ 展开拼接一致");
    }
}

// P2-7 工具失败分级 + 词表配置
void testToolErrorClassify(TestCtx& t) {
    // 判定顺序：结果 JSON error → 协议层 is_error → 文本启发式
    t.check(classifyToolError(R"({"error":"boom"})", false) == "confirmed", "P2-7 结果 JSON error 判为 confirmed");
    t.check(classifyToolError(R"({"error":""})", false) == "none", "P2-7 空 error 字段不算失败");
    t.check(classifyToolError(R"({"result":"ok"})", true) == "confirmed", "P2-7 协议 is_error 判为 confirmed");
    t.check(classifyToolError("Error: Error code: 403 Forbidden", false) == "inferred",
            "P2-7 文本 Error/403 判为 inferred");
    t.check(classifyToolError("read_webpage: Failed to read webpage", false) == "inferred",
            "P2-7 文本 Failed to 判为 inferred");
    t.check(classifyToolError("晴", false) == "none", "P2-7 正常结果判为 none");
    t.check(classifyToolError("boom happened", false, {"boom"}) == "inferred", "P2-7 自定义词表命中");
    t.check(classifyToolError("boom happened", false) == "none", "P2-7 未配置时自定义词不生效");

    // 配置：行内列表与多行列表两种写法，并传递到链选项
    {
        auto cfg = parseConfigText("proxy:\n  tool_error_patterns: [\"boom\", \"kaput\"]\n");
        t.check(cfg.ok() && cfg.value.proxy.toolErrorPatterns.size() == 2,
                "P2-7 tool_error_patterns 行内列表可解析");
        t.check(cfg.ok() && cfg.value.chainOptions().toolErrorPatterns.size() == 2,
                "P2-7 词表传递到 ChainOptions");
    }
    {
        auto cfg = parseConfigText("proxy:\n  tool_error_patterns:\n    - boom\n    - kaput\n");
        t.check(cfg.ok() && cfg.value.proxy.toolErrorPatterns.size() == 2,
                "P2-7 tool_error_patterns 多行列表可解析");
    }
    // upstreams 列表之后的 proxy 键不能被吞进最后一个 upstream
    {
        auto cfg = parseConfigText(
            "proxy:\n  upstreams:\n    - match: \"/v1\"\n      url: \"https://api.example.com/v1\"\n"
            "  tool_error_patterns: [\"boom\"]\n");
        t.check(cfg.ok() && cfg.value.proxy.upstreams.size() == 1 && cfg.value.proxy.upstreams[0].url ==
                    "https://api.example.com/v1",
                "P2-7 upstreams 之后列表字段仍正确归属");
        t.check(cfg.ok() && cfg.value.proxy.toolErrorPatterns.size() == 1,
                "P2-7 upstreams 之后的 tool_error_patterns 可解析");
    }
}

// P2-9：Usage.observed —— 区分"0 token"与"未观测"
void testUsageObserved(TestCtx& t) {
    auto parseBody = [](const char* body) { return parseOpenAiResponse(nlohmann::json::parse(body)); };
    auto withUsage = parseBody(
        R"({"id":"x","choices":[{"message":{"role":"assistant","content":"hi"},"finish_reason":"stop"}],)"
        R"("usage":{"prompt_tokens":10,"completion_tokens":5}})");
    t.check(withUsage.ok() && withUsage.value.usage.observed && withUsage.value.usage.promptTokens == 10,
            "P2-9 带 usage 的响应标记 observed");
    auto noUsage = parseBody(
        R"({"id":"x","choices":[{"message":{"role":"assistant","content":"hi"},"finish_reason":"stop"}]})");
    t.check(noUsage.ok() && !noUsage.value.usage.observed, "P2-9 无 usage 的响应不标记 observed");
    auto zeroUsage = parseBody(
        R"({"id":"x","choices":[{"message":{"role":"assistant","content":"hi"},"finish_reason":"stop"}],)"
        R"("usage":{"prompt_tokens":0,"completion_tokens":0}})");
    t.check(zeroUsage.ok() && zeroUsage.value.usage.observed, "P2-9 上游明确给 0 token 仍算观测到");
}

// P2-13 采集模式与隐私：summary 不落正文、脱敏词表扩充、run 目录 0700、非回环 bind 需显式开关
void testCapturePrivacy(TestCtx& t) {
    auto dflt = parseConfigText("proxy:\n");
    t.check(dflt.ok() && dflt.value.proxy.capture == "full", "P2-13 capture 默认 full");
    auto sum = parseConfigText("proxy:\n  capture: summary\n");
    t.check(sum.ok() && sum.value.proxy.capture == "summary", "P2-13 capture 可配置 summary");
    auto bogus = parseConfigText("proxy:\n  capture: bogus\n");
    t.check(bogus.ok() && bogus.value.proxy.capture == "full", "P2-13 未知 capture 回落 full");

    const std::string marker = summarizeBody("hello");
    t.check(marker.find("size=5") != std::string::npos, "P2-13 summary 记录长度");
    t.check(marker.find(sha256Hex("hello").substr(0, 16)) != std::string::npos,
            "P2-13 summary 记录 sha256 前 16 位");
    t.check(marker.find("hello") == std::string::npos, "P2-13 summary 不含正文");

    Config cfg = defaultConfig();
    t.check(cfg.redact.patterns.size() == 4, "P2-13 默认脱敏词表 4 条");
    const std::string body =
        R"({"api_key": "abcdefgh12345678", "note": "Bearer abcdefgh12345678", "x-api-key": "zzzzzzzz12345678"})";
    const std::string red = redactBody(body, cfg.redact.patterns);
    t.check(red.find("abcdefgh12345678") == std::string::npos, "P2-13 api_key 值被脱敏");
    t.check(red.find("zzzzzzzz12345678") == std::string::npos, "P2-13 x-api-key 值被脱敏");
    t.check(red.find("Bearer") == std::string::npos, "P2-13 Bearer token 被脱敏");

    t.check(isLoopbackBind("127.0.0.1") && isLoopbackBind("LOCALHOST") && isLoopbackBind("::1"),
            "P2-13 回环 bind 识别");
    t.check(!isLoopbackBind("0.0.0.0") && !isLoopbackBind("192.168.1.5"), "P2-13 非回环 bind 识别");
    Config remote = defaultConfig();
    remote.proxy.bind = "0.0.0.0";
    t.check(!enforceBindPolicy(remote, false).ok(), "P2-13 非回环 bind 未加 --allow-remote 被拒绝");
    t.check(enforceBindPolicy(remote, true).ok(), "P2-13 --allow-remote 放行");
    t.check(enforceBindPolicy(defaultConfig(), false).ok(), "P2-13 默认回环 bind 放行");

#ifndef _WIN32
    namespace fs = std::filesystem;
    const fs::path tmp = fs::temp_directory_path() / ("axr-p13-" + makeSessionId());
    fs::create_directories(tmp);
    const std::string dir = ensureRunDir(tmp.string(), "20261030-160000-p13x");
    std::error_code ec;
    const auto perms = fs::status(dir, ec).permissions();
    t.check(!ec && perms == fs::perms::owner_all, "P2-13 run 目录权限 0700");
    fs::remove_all(tmp);
#endif
}

// P2-14 截断事件不参与解析：req / resp 带 truncated 时跳过 parse，直接计入 parse_failed
void testTruncatedSkipParse(TestCtx& t) {
    RawEvent req;
    req.kind = "req";
    req.hop = "h-1";
    req.upstream = "https://api.deepseek.com/v1/chat/completions";
    req.reqBody = R"({"model":"m","messages":[{"role":"user","content":"hi"}]})";
    req.truncated = true;

    std::vector<RawEvent> evs = {req};
    AssembleStats st;
    auto pairs = assembleCalls(evs, &st);
    t.check(pairs.ok() && pairs.value.size() == 1, "P2-14 截断请求仍保留 pair");
    t.check(st.parseFailed == 1 && st.truncatedEvents == 1, "P2-14 截断请求计入 parse_failed");
    t.check(pairs.ok() && pairs.value[0].call.messages.empty(), "P2-14 截断请求不进入解析");
    t.check(pairs.ok() && pairs.value[0].resp.err.find("截断") != std::string::npos,
            "P2-14 截断请求留下可诊断错误");

    RawEvent good = req;
    good.truncated = false;
    RawEvent resp;
    resp.kind = "resp";
    resp.hop = "h-1";
    resp.status = 200;
    resp.latencyMs = 100;
    resp.respBody =
        R"({"choices":[{"message":{"role":"assistant","content":"hi"},"finish_reason":"stop"}],)"
        R"("usage":{"prompt_tokens":1,"completion_tokens":1}})";
    resp.truncated = true;

    evs = {good, resp};
    AssembleStats st2;
    auto pairs2 = assembleCalls(evs, &st2);
    t.check(pairs2.ok() && pairs2.value.size() == 1, "P2-14 截断响应仍保留 pair");
    t.check(st2.parseFailed == 1 && st2.truncatedEvents == 1, "P2-14 截断响应计入 parse_failed");
    t.check(pairs2.ok() && pairs2.value[0].resp.text.empty(), "P2-14 截断响应不解析正文");
    t.check(pairs2.ok() && pairs2.value[0].resp.err.find("截断") != std::string::npos,
            "P2-14 截断响应留下可诊断错误");
    t.check(pairs2.ok() && pairs2.value[0].resp.latencyMs == 100, "P2-14 截断响应仍保留延迟");
}

// WP3：请求正文不可解析（capture=summary / 截断）时 run 必须标记 incomplete，
// 但 Step 仍保留，能看出调用发生过与时间信息
void testParseFailureStatus(TestCtx& t) {
    RawEvent req;
    req.kind = "req";
    req.hop = "h-1";
    req.upstream = "https://api.deepseek.com/v1/chat/completions";
    req.reqBody = "[capture:summary size=1234 sha256=abcd1234abcd1234]";
    req.ts = "2026-10-30T10:00:00.000+08:00";

    RawEvent resp;
    resp.kind = "resp";
    resp.hop = "h-1";
    resp.status = 200;
    resp.latencyMs = 120;
    resp.ts = "2026-10-30T10:00:01.000+08:00";
    resp.respBody =
        R"({"choices":[{"message":{"role":"assistant","content":"hi"},"finish_reason":"stop"}],)"
        R"("usage":{"prompt_tokens":10,"completion_tokens":2}})";

    auto cr = buildChainFromRawEvents({req, resp});
    t.check(cr.ok() && cr.value.runs.size() == 1, "WP3 summary 请求仍切出 1 个 run");
    if (!cr.ok() || cr.value.runs.empty()) return;
    const Run& r = cr.value.runs[0];
    t.check(r.steps.size() == 1, "WP3 summary 模式的 Step 仍保留");
    t.check(r.steps[0].call.messages.empty(), "WP3 summary 模式 Step 无消息链");
    t.check(r.incomplete, "WP3 summary 模式 run 标记 incomplete");
    bool hasNote = false;
    for (const auto& m : r.missing)
        if (m.find("请求正文不可解析") != std::string::npos) hasNote = true;
    t.check(hasNote, "WP3 missing 含正文不可解析文案");

    // 截断请求同样不可用 → incomplete
    RawEvent truncReq = req;
    truncReq.truncated = true;
    truncReq.reqBody = R"({"model":"m","messages":[{"role":"user","content":"hi"}]})";
    auto cr2 = buildChainFromRawEvents({truncReq, resp});
    t.check(cr2.ok() && !cr2.value.runs.empty() && cr2.value.runs[0].incomplete,
            "WP3 截断请求的 run 标记 incomplete");

    // 正常 full 模式不受影响
    RawEvent fullReq = req;
    fullReq.reqBody = R"({"model":"m","messages":[{"role":"user","content":"hi"}]})";
    auto cr3 = buildChainFromRawEvents({fullReq, resp});
    t.check(cr3.ok() && !cr3.value.runs.empty() && !cr3.value.runs[0].incomplete,
            "WP3 正常 full 模式 run 仍为 complete");
}

// P2-15 增量读取 + 空闲收尾配置：只续读新字节、尾行不完整先不解析、重建后 run id 稳定
void testLiveIncremental(TestCtx& t) {
    auto on = parseConfigText("storage:\n  idle_finalize_minutes: 10\n");
    t.check(on.ok() && on.value.storage.idleFinalizeMinutes == 10, "P2-15 空闲收尾分钟可配置");
    auto off = parseConfigText("storage:\n  idle_finalize_minutes: 0\n");
    t.check(off.ok() && off.value.storage.idleFinalizeMinutes == 0, "P2-15 空闲收尾可关闭");
    t.check(defaultConfig().storage.idleFinalizeMinutes == 10, "P2-15 空闲收尾默认 10 分钟");

    namespace fs = std::filesystem;
    const fs::path tmp = fs::temp_directory_path() / ("axr-p15-" + makeSessionId());
    fs::create_directories(tmp);
    const std::string path = (tmp / "events.jsonl").string();

    const std::string req1 =
        R"ev({"seq":1,"ts":"2026-10-30T15:00:00.000+08:00","kind":"req","hop":"h-1","upstream":"https://api.deepseek.com/v1/chat/completions","req_body":"{\"model\":\"deepseek-chat\",\"messages\":[{\"role\":\"user\",\"content\":\"hi\"}]}"})ev";
    const std::string resp1 =
        R"ev({"seq":2,"ts":"2026-10-30T15:00:01.000+08:00","kind":"resp","hop":"h-1","status":200,"latency_ms":100,"resp_body":"{\"choices\":[{\"message\":{\"role\":\"assistant\",\"content\":\"hello\"},\"finish_reason\":\"stop\"}],\"usage\":{\"prompt_tokens\":5,\"completion_tokens\":2}}"})ev";
    const std::string req2 =
        R"ev({"seq":3,"ts":"2026-10-30T15:00:02.000+08:00","kind":"req","hop":"h-2","upstream":"https://api.deepseek.com/v1/chat/completions","req_body":"{\"model\":\"deepseek-chat\",\"messages\":[{\"role\":\"user\",\"content\":\"hi\"},{\"role\":\"assistant\",\"content\":\"hello\"},{\"role\":\"user\",\"content\":\"again\"}]}"})ev";
    const std::string resp2 =
        R"ev({"seq":4,"ts":"2026-10-30T15:00:03.000+08:00","kind":"resp","hop":"h-2","status":200,"latency_ms":100,"resp_body":"{\"choices\":[{\"message\":{\"role\":\"assistant\",\"content\":\"ok\"},\"finish_reason\":\"stop\"}],\"usage\":{\"prompt_tokens\":8,\"completion_tokens\":3}}"})ev";

    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out << req1 << "\n" << resp1 << "\n";
    }

    EventsTail tail;
    auto p1 = tail.poll(path);
    t.check(p1.ok() && p1.value && tail.events.size() == 2, "P2-15 首次读取全量事件");
    auto c1 = buildChainFromRawEvents(tail.events);
    t.check(c1.ok() && c1.value.runs.size() == 1 && c1.value.runs[0].steps.size() == 1,
            "P2-15 首次建链 1 Step");
    const std::string runId = c1.ok() && !c1.value.runs.empty() ? c1.value.runs[0].id : "";

    auto p2 = tail.poll(path);
    t.check(p2.ok() && !p2.value, "P2-15 文件未变不重复解析");

    {
        std::ofstream out(path, std::ios::binary | std::ios::app);
        out << req2 << "\n" << resp2 << "\n";
    }
    auto p3 = tail.poll(path);
    t.check(p3.ok() && p3.value && tail.events.size() == 4, "P2-15 只续读新增事件");
    auto c2 = buildChainFromRawEvents(tail.events);
    t.check(c2.ok() && c2.value.runs.size() == 1 && c2.value.runs[0].steps.size() == 2,
            "P2-15 追加后 Step 增长");
    t.check(c2.ok() && !c2.value.runs.empty() && c2.value.runs[0].id == runId, "P2-15 增量重建 run id 稳定");

    // 只写到一半的行先留在 pending，补齐换行后再解析
    {
        std::ofstream out(path, std::ios::binary | std::ios::app);
        out << req2.substr(0, 24);
    }
    auto p4 = tail.poll(path);
    t.check(p4.ok() && !p4.value && tail.events.size() == 4, "P2-15 未写完的尾行不解析");
    {
        std::ofstream out(path, std::ios::binary | std::ios::app);
        out << req2.substr(24) << "\n";
    }
    auto p5 = tail.poll(path);
    t.check(p5.ok() && p5.value && tail.events.size() == 5, "P2-15 补齐尾行后解析");

    fs::remove_all(tmp);
}

// P2-16 retention 自动执行：--auto 读取 retention_days 并直接 apply；serve 启动复用同一策略
void testAutoPrune(TestCtx& t) {
    auto cf = parseConfigText("storage:\n  retention_days: 30\n");
    t.check(cf.ok() && cf.value.storage.retentionDays == 30, "P2-16 retention_days 可配置");
    t.check(defaultConfig().storage.retentionDays == 30, "P2-16 retention_days 默认 30");

    namespace fs = std::filesystem;
    const fs::path tmp = fs::temp_directory_path() / ("axr-p16-" + makeSessionId());
    fs::create_directories(tmp);
    const fs::path runs = tmp / "runs";
    const fs::path oldRun = runs / "20260901-090000-aaaa";
    const fs::path newRun = runs / "20261030-090000-bbbb";
    fs::create_directories(oldRun);
    fs::create_directories(newRun);
    writeTextFile((oldRun / "meta.json").string(),
                  R"({"id":"20260901-090000-aaaa","started_at":"2026-09-01T09:00:00.000+08:00","status":"complete","step_count":2})");
    fs::last_write_time(oldRun, fs::file_time_type::clock::now() - std::chrono::hours(24 * 40));

    const fs::path cfg30 = tmp / "prune.yaml";
    writeTextFile(cfg30.string(), "storage:\n  retention_days: 30\n");

    std::ostringstream out;
    std::streambuf* oldBuf = std::cout.rdbuf(out.rdbuf());
    const int rc = cmdPrune({"--auto", "--root", tmp.string(), "--config", cfg30.string()});
    std::cout.rdbuf(oldBuf);

    t.check(rc == 0, "P2-16 prune --auto 执行成功");
    t.check(!fs::exists(oldRun), "P2-16 prune --auto 删除过期 run");
    t.check(fs::exists(newRun), "P2-16 prune --auto 保留未过期 run");
    t.check(out.str().find("已删除 1 个 run") != std::string::npos,
            "P2-16 prune --auto 直接执行而非 dry-run");

    // retention_days <= 0 视为关闭自动清理
    const fs::path cfgOff = tmp / "prune-off.yaml";
    writeTextFile(cfgOff.string(), "storage:\n  retention_days: 0\n");
    std::ostringstream err;
    std::streambuf* oldErr = std::cerr.rdbuf(err.rdbuf());
    const int rcOff = cmdPrune({"--auto", "--root", tmp.string(), "--config", cfgOff.string()});
    std::cerr.rdbuf(oldErr);
    t.check(rcOff == 1 && err.str().find("自动清理已关闭") != std::string::npos,
            "P2-16 retention_days<=0 时 --auto 拒绝执行");

    fs::remove_all(tmp);
}

// 统计 Mermaid 里的节点定义行（S*/Sc* 矩形与 T* 菱形）
int mermaidNodeCount(const std::string& mmd) {
    int n = 0;
    std::size_t pos = 0;
    while ((pos = mmd.find("[\"", pos)) != std::string::npos) {
        ++n;
        pos += 2;
    }
    pos = 0;
    while ((pos = mmd.find("{\"", pos)) != std::string::npos) {
        ++n;
        pos += 2;
    }
    return n;
}

// M4 单元：run 差异 / run 目录索引 / 建链性能
void testM4Units(TestCtx& t, const std::string& fixturesDir) {
    // ---- diff：新增 Step + T3 参数变化 ----
    {
        auto makeStep = [](int i, bool finalStep, const std::string& city) {
            Step s;
            s.index = i;
            s.call.model = "deepseek-chat";
            s.tokens = 100;
            s.kind = finalStep ? "final" : "llm";
            s.response.usage = Usage{100, 10, 0};
            if (!finalStep) {
                ToolExec te;
                te.callId = "call_" + std::to_string(i + 1);
                te.name = "get_weather";
                te.arguments = "{\"city\":\"" + city + "\",\"day\":" + std::to_string(i + 1) + "}";
                te.result = city == "上海" ? "多云" : "晴";
                s.tools.push_back(te);
            }
            return s;
        };

        Run base, cur;
        for (int i = 0; i < 5; ++i) base.steps.push_back(makeStep(i, i == 4, "北京"));
        for (int i = 0; i < 6; ++i) cur.steps.push_back(makeStep(i, i == 5, i == 3 ? "上海" : "北京"));
        base.total = Usage{500, 50, 0};
        cur.total = Usage{650, 60, 0};

        const RunDiff d = diffRuns(base, cur);
        t.check(!d.empty(), "M4 diff 检测到差异");
        t.check(d.steps.size() == 3, "M4 diff 报出 S3 变化 + S4 变化（final→tool）+ 新增 S5");
        if (!d.steps.empty()) {
            bool hasAdded = false, hasChanged = false;
            for (const auto& s : d.steps) {
                if (s.index == 5 && s.change == "added") hasAdded = true;
                if (s.index == 3 && s.change == "changed") hasChanged = true;
            }
            t.check(hasAdded, "M4 diff 报出新增 S5");
            t.check(hasChanged, "M4 diff 报出 S3 变化");
        }
        bool argChanged = false;
        for (const auto& td : d.tools)
            if (td.toolIndex == 3 && td.change == "arguments") argChanged = true;
        t.check(argChanged, "M4 diff 报出 T3 参数变化");
        t.check(d.baseSteps == 5 && d.curSteps == 6, "M4 diff 步骤数");
        t.check(d.curTokens > d.baseTokens, "M4 diff token 增量");
    }

    // ---- run 目录索引：meta.json 解析 + mtime 年龄 + dry-run / apply ----
    {
        using namespace std::filesystem;
        const path tmp = temp_directory_path() / ("axr-m4-" + makeSessionId());
        const path runs = tmp / "runs";
        const path oldRun = runs / "20261001-090000-aaaa";
        const path newRun = runs / "20261030-090000-bbbb";
        create_directories(oldRun);
        create_directories(newRun);
        writeTextFile((oldRun / "meta.json").string(),
                      R"({"id":"20261001-090000-aaaa","started_at":"2026-10-01T09:00:00.000+08:00","status":"complete","step_count":4,"tool_count":3,"total_tokens":312,"models":["deepseek-chat"]})");
        // newRun 故意不写 meta.json，走目录名推导
        last_write_time(oldRun, file_time_type::clock::now() - std::chrono::hours(24 * 10));

        auto listed = listRuns(tmp.string());
        t.check(listed.ok(), "M4 listRuns 可读目录");
        if (listed.ok()) {
            t.check(listed.value.size() == 2, "M4 listRuns 返回 2 个 run");
            const RunMeta* oldMeta = nullptr;
            for (const auto& m : listed.value)
                if (m.dirName == "20261001-090000-aaaa") oldMeta = &m;
            t.check(oldMeta != nullptr && oldMeta->hasMeta && oldMeta->stepCount == 4 &&
                        oldMeta->totalTokens == 312,
                    "M4 listRuns 解析 meta.json");
            t.check(oldMeta != nullptr && std::abs(oldMeta->ageDays - 10.0) < 1.0, "M4 listRuns 按 mtime 算年龄");
            bool derived = false;
            for (const auto& m : listed.value)
                if (m.dirName == "20261030-090000-bbbb") derived = !m.hasMeta && m.startedAt == "2026-10-30 09:00:00";
            t.check(derived, "M4 无 meta 时从目录名推导时间");
        }

        auto dry = pruneRuns(tmp.string(), 7, false);
        t.check(dry.ok() && dry.value.candidates.size() == 1 && dry.value.removed == 0,
                "M4 prune 默认 dry-run 只列出");
        auto applied = pruneRuns(tmp.string(), 7, true);
        t.check(applied.ok() && applied.value.removed == 1, "M4 prune --apply 删除旧 run");
        t.check(!exists(oldRun), "M4 prune 后旧目录已删除");
        t.check(exists(newRun), "M4 prune 不删未到期的 run");
        remove_all(tmp);
    }

    // ---- 真实 agent 兼容：文本回填 / 终止型工具 ----
    {
        // 响应里有 tool_calls，但下一请求用文本 Observation 回填（smolagents 行为）
        LLMResponse resp;
        resp.toolCalls.push_back(ToolCall{"call_1", "get_weather", R"({"city":"北京"})"});
        const Step s = buildStep(0, LLMCall{}, resp, {});
        t.check(s.tools.size() == 1 && s.tools[0].name == "get_weather", "真实 agent 文本回填仍识别工具调用");
        t.check(s.tools[0].result.empty() && s.tools[0].durationSource == "inferred",
                "文本回填时工具结果留空、耗时标记推断");
        t.check(s.kind == "llm", "有真实工具调用时 Step 不是 final");

        // 终止型工具 final_answer：不计入工具，Step 视为 final，末尾不报缺失
        LLMResponse term;
        term.toolCalls.push_back(ToolCall{"call_x", "final_answer", R"({"answer":"ok"})"});
        const Step st = buildStep(0, LLMCall{}, term, {});
        t.check(st.tools.empty(), "final_answer 不计入工具统计");
        t.check(st.kind == "final", "final_answer 使 Step 为 final");
        Run run;
        run.steps.push_back(st);
        std::vector<std::string> missing;
        t.check(validate(run, missing), "末尾 final_answer 不判定为采集缺失");

        // AutoGPT 的 finish 同样是终止型工具
        LLMResponse fin;
        fin.toolCalls.push_back(ToolCall{"call_z", "finish", R"({"reason":"done"})"});
        const Step sf = buildStep(0, LLMCall{}, fin, {});
        t.check(sf.tools.empty(), "finish 不计入工具统计");
        t.check(sf.kind == "final", "finish 使 Step 为 final");
        Run run3;
        run3.steps.push_back(sf);
        std::vector<std::string> missing3;
        t.check(validate(run3, missing3), "末尾 finish 不判定为采集缺失");

        // 非终止工具在末尾仍应判缺失
        LLMResponse normal;
        normal.toolCalls.push_back(ToolCall{"call_y", "get_weather", "{}"});
        Run run2;
        run2.steps.push_back(buildStep(0, LLMCall{}, normal, {}));
        std::vector<std::string> missing2;
        t.check(!validate(run2, missing2), "末尾真实工具无后续请求仍判缺失");
    }

    // ---- 时间推导 ----
    t.check(timeFromDirName("20261030-143012-a1b2") == "2026-10-30 14:30:12", "M4 目录名转时间");
    t.check(timeFromDirName("bad-name").empty(), "M4 非法目录名不推导");

    // ---- 性能：50 步链的建链耗时 < 100ms（§10 M4）----
    if (std::filesystem::exists(fixturesDir + "/t12_long_chain")) {
        const auto t0 = std::chrono::steady_clock::now();
        auto chain = buildChainFromEvents(fixturesDir + "/t12_long_chain");
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        t.check(chain.ok(), "M4 bench 长链可建链");
        t.check(ms < 100.0, "M4 50 步建链 < 100ms（实际 " + std::to_string(ms) + " ms）");
        std::cout << "  bench: t12_long_chain 建链 " << ms << " ms\n";
    }
}

// 内置 Web UI：页面 + runs 列表 + 单 run 明细（采集中从 events.jsonl 实时建链）
void testWebUi(TestCtx& t, const std::string& fixturesDir) {
    t.check(webUiPage().body.find("AgentXray") != std::string::npos, "UI 页面包含标题");
    t.check(webUiPage().body.find("showNetErr") != std::string::npos, "UI 页面有轮询失败提示逻辑");

    // utf8Truncate：截断点落在多字节字符中间时回退到字符边界，结果始终是合法 UTF-8
    {
        const std::string cn = "工具调用结果包含中文正文，用于复现字节截断问题。"; // 每字 3 字节
        const std::string cut = utf8Truncate(cn, 11); // 落在第 4 个汉字中间
        bool dumped = true;
        try {
            const nlohmann::json j = cut;
            (void)j.dump();
        } catch (...) {
            dumped = false;
        }
        t.check(dumped, "utf8Truncate 截断结果可安全 dump 为 JSON（不再 type_error.316）");
        t.check(cut.size() >= 3 && cut.compare(cut.size() - 3, 3, "…") == 0, "utf8Truncate 截断后追加省略号");
        t.check(cut.size() == 12, "utf8Truncate 截到字符边界（9 字节正文 + 省略号，实际 " +
                                  std::to_string(cut.size()) + "）");
        t.check(cn.compare(0, 9, cut, 0, 9) == 0, "utf8Truncate 保留的前缀完整");
        t.check(utf8Truncate("abcdefghij", 5) == "abcde…", "utf8Truncate ASCII 按原样截断");
        t.check(utf8Truncate(cn, cn.size()) == cn, "utf8Truncate 不超长时原样返回");
    }
    t.check(webUiPage().body.find("/__xray/api/runs") != std::string::npos, "UI 页面带 runs 接口");
    t.check(webUiWantsHtml("/", "text/html,application/xhtml+xml"), "UI 浏览器根路径识别");
    t.check(!webUiWantsHtml("/", "application/json"), "UI 非浏览器根路径不接管");
    t.check(!webUiWantsHtml("/v1/models", "text/html"), "UI 不影响其他路径");

    const std::filesystem::path tmp =
        std::filesystem::temp_directory_path() / ("axr-webui-" + makeSessionId());

    Config cfg = defaultConfig();
    cfg.storage.root = tmp.string();

    // 依赖 t2 fixture 的 runs 列表 / run 明细检查：发布二进制不带 fixtures，跳过
    if (std::filesystem::exists(fixturesDir + "/t2_single_tool/events.jsonl")) {
        const std::filesystem::path runDir = tmp / "runs" / "20261030-120000-abcd";
        std::filesystem::create_directories(runDir);
        std::filesystem::copy_file(fixturesDir + "/t2_single_tool/events.jsonl", runDir / "events.jsonl");

        auto runs = webUiRuns(cfg);
        t.check(runs.status == 200, "UI runs 接口 200");
        auto list = nlohmann::json::parse(runs.body, nullptr, false);
        t.check(list.is_array() && list.size() == 1, "UI runs 列出 1 个 run");
        if (list.is_array() && !list.empty()) t.check(list[0]["id"] == "20261030-120000-abcd", "UI runs id 正确");

        auto detail = webUiRun(cfg, "20261030-120000-abcd", "");
        t.check(detail.status == 200, "UI run 明细 200");
        auto d = nlohmann::json::parse(detail.body, nullptr, false);
        t.check(d.is_object() && d.contains("steps") && d["steps"].is_array(), "UI run 明细含 steps");
        t.check(d.contains("mermaid") && d["mermaid"].get<std::string>().find("flowchart") != std::string::npos,
                "UI run 明细含 mermaid 源码");
        t.check(d.contains("total") && d["total"]["tokens"].get<int>() > 0, "UI run 明细含 token 汇总");
        t.check(d.contains("total") && d["total"]["observed"].get<bool>(), "UI 汇总标记 usage 已观测");
        t.check(d.contains("steps") && d["steps"][0]["tokensObserved"].get<bool>(), "UI 步骤标记 usage 已观测");
        t.check(d.contains("failTools") && d["failTools"].get<int>() == 0, "UI run 明细含失败工具计数");
        t.check(d.contains("collapseThreshold"), "UI run 明细带折叠阈值");

        // 采集中（没有 meta.json）也要在侧栏给出真实数值
        if (list.is_array() && !list.empty()) {
            t.check(list[0]["live"].get<bool>(), "UI runs 标记采集中");
            t.check(list[0]["steps"].get<int>() > 0, "UI runs 对未收尾 run 现算步数");
            t.check(list[0]["status"].get<std::string>() == "采集中" ||
                        list[0]["status"].get<std::string>() == "未收尾",
                    "UI runs 未收尾 run 有明确状态");
        }
    }
    t.check(webUiPage().body.find("折叠工具调用") != std::string::npos, "UI 页面有折叠/展开工具按钮");
    t.check(webUiPage().body.find("时间线") != std::string::npos, "UI 页面有全局时间线（甘特图）");

    // 失败工具高亮：上游没标 is_error，但结果文本里有 403 → 也要标红
    {
        const std::filesystem::path errDir = tmp / "runs" / "20261030-121500-errr";
        std::filesystem::create_directories(errDir);
        Run r;
        r.id = "20261030-121500-errr";
        r.startedAt = "2026-10-30T12:15:00.000+08:00";
        r.endedAt = "2026-10-30T12:15:20.000+08:00";
        LLMCall call;
        call.model = "deepseek-chat";
        call.endpoint = "https://api.deepseek.com/v1/chat/completions";
        LLMResponse resp;
        resp.text = "let me search";
        resp.usage = Usage{100, 20, 0, true};
        resp.finishReason = "tool_calls";
        resp.latencyMs = 1500;
        ToolExec te;
        te.callId = "call_e";
        te.name = "web_search";
        te.arguments = R"({"query":"ai news"})";
        te.result = "Error: Error code: 403 Forbidden - web access denied";
        te.errorKind = "inferred"; // 协议层没标，由链层按文本判为推断失败
        te.durationMs = 900;
        ToolExec te2;
        te2.callId = "call_f";
        te2.name = "read_webpage";
        te2.arguments = R"({"url":"https://example.com"})";
        te2.result = "read_webpage: Failed to read webpage: Failed to launch browser: Executable doesn't exist";
        te2.errorKind = "inferred";
        te2.durationMs = 1200;
        Step s;
        s.index = 0;
        s.call = call;
        s.response = resp;
        s.tools = {te, te2};
        s.tokens = 120;
        s.kind = "llm";
        r.steps = {s};
        writeTextFile((errDir / "run.json").string(), nlohmann::json(r).dump(2));

        auto d3 = webUiRun(cfg, "20261030-121500-errr", "");
        auto j3 = nlohmann::json::parse(d3.body, nullptr, false);
        t.check(d3.status == 200 && j3.contains("failTools"), "UI 失败 run 明细可读");
        t.check(j3["failTools"].get<int>() == 2, "UI 403 / Failed to 结果都计入失败工具");
        t.check(j3["steps"][0]["hasError"].get<bool>(), "UI 含失败工具的 Step 标记 hasError");
        t.check(j3["steps"][0]["tools"][0]["errorKind"].get<std::string>() == "inferred",
                "UI 403 工具标记为推断失败");
        t.check(j3["steps"][0]["tools"][0]["isError"].get<bool>(), "UI 推断失败仍计入 isError");
        t.check(j3["steps"][0]["tools"][0]["errorInferred"].get<bool>(), "UI 403 工具标记为文本推断");
        t.check(j3["steps"][0]["failInferred"].get<int>() == 2, "UI 失败工具按等级计数");
        t.check(j3["steps"][0]["tools"][1]["isError"].get<bool>(), "UI Failed to 工具标记为失败");
    }

    // 回归：长多字节正文曾让 trimText 切在汉字中间，j.dump() 抛 type_error.316，/api/run 500
    {
        const std::filesystem::path longDir = tmp / "runs" / "20261030-123000-long";
        std::filesystem::create_directories(longDir);
        Run r;
        r.id = "20261030-123000-long";
        r.startedAt = "2026-10-30T12:30:00.000+08:00";
        r.endedAt = "2026-10-30T12:30:20.000+08:00";
        LLMCall call;
        call.model = "deepseek-chat";
        LLMResponse resp;
        for (int i = 0; i < 60; ++i) resp.text += "读取文件并分析结果，正文包含大量中文内容。";
        resp.usage = Usage{100, 20, 0, true};
        resp.latencyMs = 1200;
        ToolExec te;
        te.callId = "call_l";
        te.name = "read_file";
        for (int i = 0; i < 30; ++i) te.arguments += "参数包含中文描述，";
        for (int i = 0; i < 40; ++i) te.result += "工具返回内容很长，包含中文与路径，";
        te.durationMs = 300;
        Step s;
        s.index = 0;
        s.call = call;
        s.response = resp;
        s.tools = {te};
        s.tokens = 120;
        s.kind = "llm";
        r.steps = {s};
        writeTextFile((longDir / "run.json").string(), nlohmann::json(r).dump(2));

        const auto dl = webUiRun(cfg, "20261030-123000-long", "");
        t.check(dl.status == 200, "长中文正文的 run 明细返回 200（曾是 500）");
        const auto jl = nlohmann::json::parse(dl.body, nullptr, false);
        t.check(jl.is_object() && jl.contains("steps") && !jl["steps"].empty(), "长中文正文 run 明细可解析");
        if (jl.is_object() && jl.contains("steps") && !jl["steps"].empty()) {
            const std::string txt = jl["steps"][0]["text"].get<std::string>();
            t.check(txt.size() <= 1203, "长正文截到 1200 字节预算内（实际 " + std::to_string(txt.size()) + "）");
            const std::string tres = jl["steps"][0]["tools"][0]["result"].get<std::string>();
            t.check(tres.size() <= 603, "长工具结果截到 600 字节预算内（实际 " + std::to_string(tres.size()) + "）");
        }
    }

    t.check(webUiRun(cfg, "../etc", "").status == 400, "UI run 拒绝路径穿越 id");
    t.check(webUiRun(cfg, "nope", "").status == 404, "UI run 未知 id 返回 404");

    std::filesystem::remove_all(tmp);
}

} // namespace

int cmdSelftest(const std::vector<std::string>& args) {
    TestCtx t;
    const std::string dir = fixtureDir(args);
    std::cout << "fixtures: " << dir << "\n";
    if (!std::filesystem::is_directory(dir))
        std::cout << "fixtures 不存在：跳过依赖夹具的用例（在开发仓库根目录运行才会执行全部用例）\n";

    testHash(t);
    testModelRoundtrip(t);
    testM2Units(t);
    testToolErrorClassify(t);
    testUsageObserved(t);
    testM4Units(t, dir);
    testWebUi(t, dir);
    testCapturePrivacy(t);
    testTruncatedSkipParse(t);
    testParseFailureStatus(t);
    testLiveIncremental(t);
    testAutoPrune(t);

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
        // resp_body 解析曾把整个结构体覆盖，导致 latency_ms 丢失（时间线恒为 0.0s）
        t.check(r.steps[1].response.latencyMs == 1200, "T2 步骤耗时保留 resp 事件的 latency_ms");

        // P2-11 耗时来源：流量推断才带 "~"，实测来源（mcp/otel）不带
        t.check(r.steps[0].tools[0].durationSource == "inferred", "P2-11 流量推断的耗时标记 inferred");
        t.check(toMermaid(r).find("&lt;br/&gt;~") != std::string::npos, "P2-11 inferred 耗时在图里带 ~");
        Run measured = r;
        measured.steps[0].tools[0].durationSource = "otel";
        t.check(toMermaid(measured).find("&lt;br/&gt;~") == std::string::npos, "P2-11 otel 耗时在图里不带 ~");
        t.check(buildReport(measured).find("~0.5s") == std::string::npos, "P2-11 otel 耗时在报告里不带 ~");
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

    // T14 两个独立任务共享同一 system prompt（间隔仅 5 秒）：不得串成一条链
    testFixture(dir, "t14_two_tasks_same_system", t, [&](const ChainResult& cr) {
        t.check(cr.runs.size() == 2, "T14 共享 system prompt 的两个任务切成 2 个 run");
        bool allTwoSteps = true, allComplete = true;
        for (const auto& r : cr.runs) {
            if (r.steps.size() != 2) allTwoSteps = false;
            if (r.incomplete) allComplete = false;
        }
        t.check(allTwoSteps, "T14 每个 run 各 2 步");
        t.check(allComplete, "T14 两个 run 均完整（无 incomplete）");

        // P2-10 run id 末尾追加 4 位序号，同一秒同 system prompt 的 run 不再撞 id
        // WP1 再追加 4 位 session 命名空间段：YYYYMMDD-HHMMSS-xxxxxxxx-nnnn-序号
        bool idsOk = true;
        for (const auto& r : cr.runs) {
            if (r.id.size() != 34 || r.id[8] != '-' || r.id[15] != '-' || r.id[24] != '-' || r.id[29] != '-')
                idsOk = false;
        }
        t.check(idsOk, "P2-10 run id 形如 YYYYMMDD-HHMMSS-xxxx-nnnn-序号");
        t.check(cr.runs[0].id != cr.runs[1].id, "P2-10 同一链内 run id 唯一");

        auto again = buildChainFromEvents(dir + "/t14_two_tasks_same_system");
        t.check(again.ok() && again.value.runs.size() == 2 && again.value.runs[0].id == cr.runs[0].id &&
                    again.value.runs[1].id == cr.runs[1].id,
                "P2-10 重复建链 run id 稳定");
    });

    // P0-2 多 run 输出：meta 登记全部 run、非主 run 落盘 run-<i>.json、CLI/UI 可按 id 访问
    if (std::filesystem::exists(dir + "/t14_two_tasks_same_system/events.jsonl")) {
        namespace fs = std::filesystem;
        const fs::path tmp = fs::temp_directory_path() / ("axr-p02-" + makeSessionId());
        const fs::path sessionDir = tmp / "runs" / "20261030-150000-p02x";
        fs::create_directories(sessionDir);
        fs::copy_file(dir + "/t14_two_tasks_same_system/events.jsonl", sessionDir / "events.jsonl");

        Config cfg = defaultConfig();
        cfg.storage.root = tmp.string();

        auto chain = buildChainFromEvents(sessionDir.string());
        t.check(chain.ok() && chain.value.runs.size() == 2, "P0-2 建链得到 2 个 run");
        const Run* primary = chain.ok() ? primaryRun(chain.value) : nullptr;
        std::string otherId;
        if (primary) {
            for (const auto& r : chain.value.runs)
                if (r.id != primary->id) otherId = r.id;
        }
        t.check(primary != nullptr && !otherId.empty(), "P0-2 找到主 run 与非主 run");
        t.check(findRunById(chain.value, otherId) != nullptr, "P0-2 findRunById 命中非主 run");
        t.check(findRunById(chain.value, "no-such-run") == nullptr, "P0-2 findRunById 未命中返回空");

        const RunSummary s = finalizeRun(sessionDir.string(), cfg, 0);
        t.check(s.status == "complete", "P0-2 finalize 主 run 完整");

        nlohmann::json meta;
        {
            std::ifstream in((sessionDir / "meta.json").string());
            in >> meta;
        }
        t.check(meta.value("primary", "") == (primary ? primary->id : ""), "P0-2 meta.primary 指向主 run");
        t.check(meta.contains("runs") && meta["runs"].size() == chain.value.runs.size(),
                "P0-2 meta.runs 数量与建链结果一致");
        bool briefOk = true;
        for (const auto& b : meta["runs"]) {
            if (b.value("id", "").empty() || b.value("steps", 0) != 2) briefOk = false;
        }
        t.check(briefOk, "P0-2 meta.runs 摘要字段齐全（id/steps）");

        // WP2：目标可以是 sessionID，也可以直接是非主 run 的 runId
        auto tgtOther = resolveRunTarget(otherId, cfg);
        t.check(tgtOther.ok() && !tgtOther.value.runId.empty() &&
                    fs::equivalent(tgtOther.value.path, sessionDir),
                "WP2 runId 可定位到所属 session");
        auto tgtSession = resolveRunTarget("20261030-150000-p02x", cfg);
        t.check(tgtSession.ok() && tgtSession.value.runId.empty() &&
                    fs::equivalent(tgtSession.value.path, sessionDir),
                "WP2 sessionId 行为不变");
        t.check(isRunIdLike(otherId) && !isRunIdLike("20261030-150000-p02x"), "WP2 runId 形态识别正确");

        // 非主 run 落盘 run-0.json，内容是完整 Run JSON
        const fs::path secondary = sessionDir / "run-0.json";
        t.check(fs::exists(secondary), "P0-2 非主 run 落盘 run-0.json");
        Run secondaryRun;
        bool parsed = false;
        try {
            std::ifstream in(secondary.string());
            secondaryRun = nlohmann::json::parse(in).get<Run>();
            parsed = true;
        } catch (...) {
        }
        t.check(parsed && secondaryRun.id == otherId, "P0-2 run-0.json 即非主 run");
        t.check(parsed && secondaryRun.steps.size() == 2, "P0-2 run-0.json 含 2 个 Step");

        // listRuns 能读回多 run 摘要
        auto listed = listRuns(tmp.string());
        t.check(listed.ok() && listed.value.size() == 1, "P0-2 listRuns 1 个目录");
        if (listed.ok() && !listed.value.empty()) {
            const RunMeta& rm = listed.value[0];
            t.check(rm.runs.size() == 2 && !rm.primary.empty(), "P0-2 listRuns 解析 primary 与 runs");
        }

        // Web UI：runs 列表带 runCount/runs，run 明细可按 run 参数取非主 run
        auto list2 = nlohmann::json::parse(webUiRuns(cfg).body, nullptr, false);
        t.check(list2.is_array() && list2.size() == 1 && list2[0].value("runCount", 0) == 2 &&
                    list2[0].contains("runs") && list2[0]["runs"].size() == 2,
                "P0-2 UI runs 带 runCount 与 runs 摘要");
        auto d2 = webUiRun(cfg, "20261030-150000-p02x", otherId);
        t.check(d2.status == 200, "P0-2 UI 按 run 参数取非主 run 200");
        auto j2 = nlohmann::json::parse(d2.body, nullptr, false);
        t.check(j2.value("id", "") == otherId && j2.contains("steps") && j2["steps"].size() == 2,
                "P0-2 UI 非主 run 明细内容正确");
        t.check(webUiRun(cfg, "20261030-150000-p02x", "no-such-run").status == 404,
                "P0-2 UI 未知 run id 返回 404");

        fs::remove_all(tmp);
    }

    // T15 混合端点：非 chat/completions 请求被显式隔离，不进入链与统计
    testFixture(dir, "t15_mixed_endpoints", t, [&](const ChainResult& cr) {
        t.check(cr.runs.size() == 1, "T15 runs 只由 chat/completions 组成（1 个 run）");
        t.check(cr.unsupportedRequests == 2, "T15 responses/embeddings 计入 unsupported_requests");
        t.check(cr.truncatedEvents == 1, "T15 truncated 事件单独计数");
        t.check(cr.parseFailed == 1, "T15 截断响应不再解析，计入 parse_failed");
        if (cr.runs.empty()) return;
        const Run& r = cr.runs[0];
        t.check(r.steps.size() == 2, "T15 2 Step（本机 11434 上游也算 chat/completions）");
        bool allChat = true;
        for (const auto& s : r.steps) {
            if (!isSupportedEndpoint(s.call.endpoint)) allChat = false;
        }
        t.check(allChat, "T15 链上步骤全部是 chat/completions 端点");
    });

    // P0-3 白名单边界、解析失败计数、meta 落盘与 Web UI 提示字段
    {
        // 白名单：以 /chat/completions 结尾才还原，兼容本地上游
        t.check(isSupportedEndpoint("https://api.deepseek.com/v1/chat/completions"),
                "P0-3 标准端点命中白名单");
        t.check(isSupportedEndpoint("http://127.0.0.1:11434/v1/chat/completions"),
                "P0-3 本地上游命中白名单");
        t.check(!isSupportedEndpoint("https://api.openai.com/v1/responses"),
                "P0-3 Responses API 被隔离");
        t.check(!isSupportedEndpoint("https://api.anthropic.com/v1/messages"),
                "P0-3 Anthropic /v1/messages 被隔离");

        // 白名单端点但 req_body 是坏 JSON：parse_failed 单独计数，pair 仍保留（链可诊断）
        std::vector<RawEvent> evs;
        RawEvent bad;
        bad.seq = 1;
        bad.kind = "req";
        bad.hop = "h-1";
        bad.upstream = "https://api.deepseek.com/v1/chat/completions";
        bad.reqBody = "{not json";
        evs.push_back(bad);
        AssembleStats st;
        auto pairs = assembleCalls(evs, &st);
        t.check(pairs.ok() && pairs.value.size() == 1, "P0-3 解析失败的请求仍留在链上");
        t.check(st.parseFailed == 1 && st.unsupportedRequests == 0, "P0-3 解析失败单独计数");

        // 现有 fixture 不得被误过滤 / 误计数（发布二进制无 fixtures，整段跳过）
        if (std::filesystem::exists(dir + "/t15_mixed_endpoints/events.jsonl")) {
        auto chain14 = buildChainFromEvents(dir + "/t14_two_tasks_same_system");
        t.check(chain14.ok() && chain14.value.unsupportedRequests == 0 &&
                    chain14.value.parseFailed == 0 && chain14.value.truncatedEvents == 0,
                "P0-3 现有 fixture 不被误过滤");

        // finalize → meta 写 unsupported_requests / parse_degraded；UI 明细带提示字段
        namespace fs = std::filesystem;
        const fs::path tmp = fs::temp_directory_path() / ("axr-p03-" + makeSessionId());
        const fs::path sessionDir = tmp / "runs" / "20261030-150000-p03x";
        fs::create_directories(sessionDir);
        fs::copy_file(dir + "/t15_mixed_endpoints/events.jsonl", sessionDir / "events.jsonl");

        Config cfg = defaultConfig();
        cfg.storage.root = tmp.string();

        const RunSummary s = finalizeRun(sessionDir.string(), cfg, 0);
        t.check(s.status == "complete" && s.stepCount == 2,
                "P0-3 finalize 只还原 chat/completions 的 2 步");

        nlohmann::json meta;
        {
            std::ifstream in((sessionDir / "meta.json").string());
            in >> meta;
        }
        t.check(meta.value("unsupported_requests", 0) == 2, "P0-3 meta.unsupported_requests == 2");
        t.check(meta.value("truncated_events", 0) == 1, "P0-3 meta.truncated_events == 1");
        t.check(meta.value("parse_failed", 0) == 1, "P0-3 截断响应计入 meta.parse_failed");
        t.check(meta.value("parse_degraded", false), "P0-3 meta.parse_degraded 置真");

        auto d3 = webUiRun(cfg, "20261030-150000-p03x", "");
        t.check(d3.status == 200, "P0-3 UI 明细 200");
        auto j3 = nlohmann::json::parse(d3.body, nullptr, false);
        t.check(j3.value("unsupportedRequests", 0) == 2 && j3.value("truncatedEvents", 0) == 1,
                "P0-3 UI 明细带降级计数字段");

        fs::remove_all(tmp);
        }
    }

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
    if (std::filesystem::exists(dir + "/deepseek_tool_2")) {
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

    // T7b 模糊匹配 fixture：第 2 个请求改写了 system 提示词
    testFixture(dir, "t7_fuzzy", t, [&](const ChainResult& cr) {
        t.check(cr.fuzzyCount >= 1, "M3 fuzzy fixture 走模糊匹配");
        t.check(cr.runs.size() == 1, "M3 fuzzy 仍归到同一 run");
        if (cr.runs.empty()) return;
        const Run& r = cr.runs[0];
        t.check(r.steps.size() == 2, "M3 fuzzy 2 Step");
        t.check(r.steps[1].fuzzyIn, "M3 fuzzy 边标记到后一步");
        t.check(r.fuzzyEdges >= 1, "M3 fuzzy 边计数");
        const std::string mmd = toMermaid(r);
        t.check(mmd.find("-.->|推测|") != std::string::npos, "M3 fuzzy 边在图中是虚线且标注推测");
    });

    // 长链折叠：50 步 fixture，节点数 ≤ 12 + 折叠节点
    testFixture(dir, "t12_long_chain", t, [&](const ChainResult& cr) {
        t.check(cr.runs.size() == 1, "M3 长链 1 run");
        if (cr.runs.empty()) return;
        const Run& r = cr.runs[0];
        t.check(r.steps.size() == 50, "M3 长链 50 Step");
        t.check(r.requestCount == 51, "M3 原始请求 51");
        t.check(r.retryCount == 1, "M3 重试请求 1 条");
        t.check(r.steps.size() < r.requestCount, "M3 步骤数比原始请求数少");
        const std::string mmd = toMermaid(r);
        t.check(mermaidNodeCount(mmd) <= 13, "M3 长链折叠后节点数 ≤ 12 + 折叠节点");
        t.check(mmd.find("「循环」") != std::string::npos, "M3 折叠节点出现在图上");
    });

    // report.md：概览 / 流程图 / 时间线 / 工具表 / Token 用量表 / 缺失警告
    if (std::filesystem::exists(dir + "/t5_missing")) {
        auto chain = buildChainFromEvents(dir + "/t5_missing");
        if (chain.ok() && !chain.value.runs.empty()) {
            const Run& r = chain.value.runs[0];
            const std::string rep = buildReport(r);
            t.check(rep.find("⚠ 检测到缺失步骤") != std::string::npos, "M3 报告顶部有缺失警告");
            t.check(rep.find("## 执行流程") != std::string::npos && rep.find("```mermaid") != std::string::npos,
                    "M3 报告内嵌流程图");
            t.check(rep.find("## 时间线") != std::string::npos, "M3 报告含时间线");
            t.check(rep.find("## 工具调用") != std::string::npos, "M3 报告含工具调用表");
            t.check(rep.find("## Token 用量") != std::string::npos && rep.find("Total:") != std::string::npos,
                    "M3 报告含 Token 用量表与合计");
            // 单模型 run 不再逐行重复模型名（上方概览已写一次），表头应无 Model 列
            t.check(rep.find("| Step | Prompt | Completion | 合计 |") != std::string::npos,
                    "M3 单模型报告的 Token 表不重复模型列");
        } else {
            t.check(false, "M3 报告夹具加载失败");
        }
    }

    // M4 diff fixture：base 5 步 / cur 6 步，且 T3 参数与结果不同
    if (std::filesystem::exists(dir + "/t13_diff_base")) {
        auto baseChain = buildChainFromEvents(dir + "/t13_diff_base");
        auto curChain = buildChainFromEvents(dir + "/t13_diff_cur");
        if (baseChain.ok() && curChain.ok() && primaryRun(baseChain.value) && primaryRun(curChain.value)) {
            const RunDiff d = diffRuns(*primaryRun(baseChain.value), *primaryRun(curChain.value));
            t.check(d.baseSteps == 5 && d.curSteps == 6, "M4 diff fixture 5 步 → 6 步");
            bool added5 = false, changed3 = false;
            for (const auto& s : d.steps) {
                if (s.index == 5 && s.change == "added") added5 = true;
                if (s.index == 3 && s.change == "changed") changed3 = true;
            }
            t.check(added5, "M4 diff fixture 新增 S5");
            t.check(changed3, "M4 diff fixture S3 变化");
            bool args3 = false, result3 = false;
            for (const auto& td : d.tools) {
                if (td.toolIndex == 3 && td.change == "arguments") args3 = true;
                if (td.toolIndex == 3 && td.change == "result") result3 = true;
            }
            t.check(args3, "M4 diff fixture T3 参数变化");
            t.check(result3, "M4 diff fixture T3 结果变化");

            // WP1：run id 含 session 命名空间，两个目录即使时间戳/根哈希相同也不再撞 id
            const std::string bId = primaryRun(baseChain.value)->id;
            const std::string cId = primaryRun(curChain.value)->id;
            t.check(bId.substr(0, 24) == cId.substr(0, 24) && bId != cId,
                    "WP1 同时间戳同根哈希的两个 session run id 不同");
            auto baseAgain = buildChainFromEvents(dir + "/t13_diff_base");
            t.check(baseAgain.ok() && primaryRun(baseAgain.value) &&
                        primaryRun(baseAgain.value)->id == bId,
                    "WP1 同一 fixture 重复 replay run id 稳定");
        } else {
            t.check(false, "M4 diff fixture 加载失败");
        }
    }

    // WP7：主 run 并列（步数相同）时应取开始最早的一条
    {
        ChainResult cr;
        Run early;
        early.id = "early";
        early.startedAt = "2026-10-30T10:00:00.000+08:00";
        early.steps.resize(2);
        Run late = early;
        late.id = "late";
        late.startedAt = "2026-10-30T10:00:05.000+08:00";
        cr.runs.push_back(early);
        cr.runs.push_back(late);
        const Run* p = primaryRun(cr);
        t.check(p != nullptr && p->id == "early", "WP7 主 run 步数并列时取最早");
    }

    std::cout << "\n结果: " << t.pass << " passed, " << t.fail << " failed\n";
    return t.fail == 0 ? 0 : 1;
}

} // namespace axr
