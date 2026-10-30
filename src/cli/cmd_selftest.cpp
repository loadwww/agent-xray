#include "cli/cli.h"

#include <chrono>
#include <cmath>
#include <filesystem>
#include <functional>
#include <iostream>
#include <map>
#include <mutex>

#include "chain/fuzzy.h"
#include "chain/link.h"
#include "chain/run.h"
#include "chain/superstep.h"
#include "chain/validate.h"
#include "common/hash.h"
#include "common/log.h"
#include "config/config.h"
#include "cost/pricing.h"
#include "diff/rundiff.h"
#include "graph/mermaid.h"
#include "proxy/event.h"
#include "proxy/inject.h"
#include "proxy/recorder.h"
#include "proxy/redact.h"
#include "proxy/route.h"
#include "store/events_reader.h"
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

    // §7.2.3 inject_usage：配置开关 + 流式请求体改写
    {
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

void testM3Units(TestCtx& t) {
    // pricing 配置：inline map / 块式 / default / 整段覆盖默认表
    {
        const std::string yaml = R"(
proxy:
  upstreams:
    - match: "/v1"
      url: "http://127.0.0.1:9099/v1"
chain:
  collapse_threshold: 12
pricing:
  deepseek-chat: { input: 0.14, output: 0.28 }
  my-model:
    input: 0.5
    output: 1.5
  default: { input: 1.00, output: 3.00 }
)";
        auto cfg = parseConfigText(yaml);
        t.check(cfg.ok(), "M3 pricing 配置可解析");
        if (cfg.ok()) {
            auto it = cfg.value.pricing.models.find("deepseek-chat");
            t.check(it != cfg.value.pricing.models.end() && std::abs(it->second.input - 0.14) < 1e-9,
                    "M3 pricing inline map");
            auto it2 = cfg.value.pricing.models.find("my-model");
            t.check(it2 != cfg.value.pricing.models.end() && std::abs(it2->second.output - 1.5) < 1e-9,
                    "M3 pricing 块式解析");
            t.check(cfg.value.pricing.models.find("gpt-4o") == cfg.value.pricing.models.end(),
                    "M3 pricing 覆盖默认单价表");
            t.check(cfg.value.chain.collapseThreshold == 12, "M3 collapse_threshold 解析");
        }
    }

    // 成本：与手算一致到 1e-6（§7.5 验收）
    {
        Run r;
        r.steps.resize(2);
        r.steps[0].call.model = "deepseek-chat";
        r.steps[0].response.usage = Usage{1000, 500, 0};
        r.steps[1].call.model = "deepseek-chat";
        r.steps[1].response.usage = Usage{2000, 300, 0};
        const CostBreakdown cb = computeCost(r, defaultConfig().pricing);
        t.check(std::abs(cb.stepUsd[0] - 0.000280) < 1e-6, "M3 step0 成本手算一致");
        t.check(std::abs(cb.stepUsd[1] - 0.000364) < 1e-6, "M3 step1 成本手算一致");
        t.check(std::abs(cb.totalUsd - 0.000644) < 1e-6, "M3 总成本手算一致");
        t.check(cb.complete && cb.hasUsage, "M3 usage 完整");
        t.check(cb.byModel.count("deepseek-chat") == 1, "M3 按模型归因");
    }

    // cached token 按 input 的 1/10 计
    {
        Run r;
        r.steps.resize(1);
        r.steps[0].call.model = "deepseek-chat";
        r.steps[0].response.usage = Usage{1000, 0, 1000};
        const CostBreakdown cb = computeCost(r, defaultConfig().pricing);
        t.check(std::abs(cb.stepUsd[0] - 1000.0 * 0.014 / 1000000.0) < 1e-9, "M3 cached token 1/10 计价");
    }

    // T9：usage 缺失 → n/a，不崩
    {
        Run r;
        r.steps.resize(1);
        r.steps[0].call.model = "deepseek-chat";
        const CostBreakdown cb = computeCost(r, defaultConfig().pricing);
        t.check(!cb.complete && !cb.stepKnown[0], "M3 无 usage 时成本标记 n/a");
        t.check(fmtCost(0.0) == "$0.000000", "M3 成本格式化");
    }

    // 未知模型走 default 单价
    {
        Run r;
        r.steps.resize(1);
        r.steps[0].call.model = "unknown-model";
        r.steps[0].response.usage = Usage{1000000, 0, 0};
        const CostBreakdown cb = computeCost(r, defaultConfig().pricing);
        t.check(std::abs(cb.stepUsd[0] - 1.00) < 1e-9, "M3 未知模型走 default 单价");
    }
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

        const RunDiff d = diffRuns(base, cur, defaultConfig().pricing);
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
                      R"({"id":"20261001-090000-aaaa","started_at":"2026-10-01T09:00:00.000+08:00","status":"complete","step_count":4,"tool_count":3,"total_tokens":312,"cost_usd":0.000054,"cost_known":true,"models":["deepseek-chat"]})");
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
                        std::abs(oldMeta->costUsd - 0.000054) < 1e-9,
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
        t.check(s.tools[0].result.empty() && s.tools[0].approx, "文本回填时工具结果留空、标记推断");
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
    {
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
    t.check(webUiPage().body.find("/__xray/api/runs") != std::string::npos, "UI 页面带 runs 接口");
    t.check(webUiWantsHtml("/", "text/html,application/xhtml+xml"), "UI 浏览器根路径识别");
    t.check(!webUiWantsHtml("/", "application/json"), "UI 非浏览器根路径不接管");
    t.check(!webUiWantsHtml("/v1/models", "text/html"), "UI 不影响其他路径");

    const std::filesystem::path tmp =
        std::filesystem::temp_directory_path() / ("axr-webui-" + makeSessionId());
    const std::filesystem::path runDir = tmp / "runs" / "20261030-120000-abcd";
    std::filesystem::create_directories(runDir);
    std::filesystem::copy_file(fixturesDir + "/t2_single_tool/events.jsonl", runDir / "events.jsonl");

    Config cfg = defaultConfig();
    cfg.storage.root = tmp.string();

    auto runs = webUiRuns(cfg);
    t.check(runs.status == 200, "UI runs 接口 200");
    auto list = nlohmann::json::parse(runs.body, nullptr, false);
    t.check(list.is_array() && list.size() == 1, "UI runs 列出 1 个 run");
    if (list.is_array() && !list.empty()) t.check(list[0]["id"] == "20261030-120000-abcd", "UI runs id 正确");

    auto detail = webUiRun(cfg, "20261030-120000-abcd");
    t.check(detail.status == 200, "UI run 明细 200");
    auto d = nlohmann::json::parse(detail.body, nullptr, false);
    t.check(d.is_object() && d.contains("steps") && d["steps"].is_array(), "UI run 明细含 steps");
    t.check(d.contains("mermaid") && d["mermaid"].get<std::string>().find("flowchart") != std::string::npos,
            "UI run 明细含 mermaid 源码");
    t.check(d.contains("total") && d["total"]["tokens"].get<int>() > 0, "UI run 明细含 token 汇总");
    t.check(d.contains("failTools") && d["failTools"].get<int>() == 0, "UI run 明细含失败工具计数");
    t.check(d.contains("collapseThreshold"), "UI run 明细带折叠阈值");
    t.check(webUiPage().body.find("折叠工具调用") != std::string::npos, "UI 页面有折叠/展开工具按钮");
    t.check(webUiPage().body.find("时间线") != std::string::npos, "UI 页面有全局时间线（甘特图）");

    // 采集中（没有 meta.json）也要在侧栏给出真实数值
    if (list.is_array() && !list.empty()) {
        t.check(list[0]["live"].get<bool>(), "UI runs 标记采集中");
        t.check(list[0]["steps"].get<int>() > 0, "UI runs 对未收尾 run 现算步数");
        t.check(list[0]["status"].get<std::string>() == "采集中" ||
                    list[0]["status"].get<std::string>() == "未收尾",
                "UI runs 未收尾 run 有明确状态");
    }

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
        resp.usage = Usage{100, 20, 0};
        resp.finishReason = "tool_calls";
        resp.latencyMs = 1500;
        ToolExec te;
        te.callId = "call_e";
        te.name = "web_search";
        te.arguments = R"({"query":"ai news"})";
        te.result = "Error: Error code: 403 Forbidden - web access denied";
        te.isError = false; // 协议层没标，靠文本兜底
        te.durationMs = 900;
        ToolExec te2;
        te2.callId = "call_f";
        te2.name = "read_webpage";
        te2.arguments = R"({"url":"https://example.com"})";
        te2.result = "read_webpage: Failed to read webpage: Failed to launch browser: Executable doesn't exist";
        te2.isError = false;
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

        auto d3 = webUiRun(cfg, "20261030-121500-errr");
        auto j3 = nlohmann::json::parse(d3.body, nullptr, false);
        t.check(d3.status == 200 && j3.contains("failTools"), "UI 失败 run 明细可读");
        t.check(j3["failTools"].get<int>() == 2, "UI 403 / Failed to 结果都计入失败工具");
        t.check(j3["steps"][0]["hasError"].get<bool>(), "UI 含失败工具的 Step 标记 hasError");
        t.check(j3["steps"][0]["tools"][0]["isError"].get<bool>(), "UI 403 工具标记为失败");
        t.check(j3["steps"][0]["tools"][0]["errorInferred"].get<bool>(), "UI 403 工具标记为文本推断");
        t.check(j3["steps"][0]["tools"][1]["isError"].get<bool>(), "UI Failed to 工具标记为失败");
    }

    t.check(webUiRun(cfg, "../etc").status == 400, "UI run 拒绝路径穿越 id");
    t.check(webUiRun(cfg, "nope").status == 404, "UI run 未知 id 返回 404");

    std::filesystem::remove_all(tmp);
}

} // namespace

int cmdSelftest(const std::vector<std::string>& args) {
    TestCtx t;
    const std::string dir = fixtureDir(args);
    std::cout << "fixtures: " << dir << "\n";

    testHash(t);
    testModelRoundtrip(t);
    testM2Units(t);
    testM3Units(t);
    testM4Units(t, dir);
    testWebUi(t, dir);

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

    // report.md：概览 / 流程图 / 时间线 / 工具表 / 成本表 / 缺失警告
    {
        auto chain = buildChainFromEvents(dir + "/t5_missing");
        if (chain.ok() && !chain.value.runs.empty()) {
            const Run& r = chain.value.runs[0];
            const std::string rep = buildReport(r, computeCost(r, defaultConfig().pricing));
            t.check(rep.find("⚠ 检测到缺失步骤") != std::string::npos, "M3 报告顶部有缺失警告");
            t.check(rep.find("## 执行流程") != std::string::npos && rep.find("```mermaid") != std::string::npos,
                    "M3 报告内嵌流程图");
            t.check(rep.find("## 时间线") != std::string::npos, "M3 报告含时间线");
            t.check(rep.find("## 工具调用") != std::string::npos, "M3 报告含工具调用表");
            t.check(rep.find("## 成本") != std::string::npos && rep.find("Total:") != std::string::npos,
                    "M3 报告含成本表与合计");
        } else {
            t.check(false, "M3 报告夹具加载失败");
        }
    }

    // M4 diff fixture：base 5 步 / cur 6 步，且 T3 参数与结果不同
    {
        auto baseChain = buildChainFromEvents(dir + "/t13_diff_base");
        auto curChain = buildChainFromEvents(dir + "/t13_diff_cur");
        if (baseChain.ok() && curChain.ok() && primaryRun(baseChain.value) && primaryRun(curChain.value)) {
            const RunDiff d = diffRuns(*primaryRun(baseChain.value), *primaryRun(curChain.value),
                                       defaultConfig().pricing);
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
        } else {
            t.check(false, "M4 diff fixture 加载失败");
        }
    }

    std::cout << "\n结果: " << t.pass << " passed, " << t.fail << " failed\n";
    return t.fail == 0 ? 0 : 1;
}

} // namespace axr
