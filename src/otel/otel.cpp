#include "otel/otel.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <vector>

#include "common/hash.h"
#include "common/log.h"
#include "common/time_util.h"
#include "proxy/forward.h"
#include "store/run_dir.h"

namespace axr {
namespace {

nlohmann::json strAttr(const std::string& key, const std::string& v) {
    return nlohmann::json{{"key", key}, {"value", {{"stringValue", v}}}};
}

// OTLP JSON 里 64 位整数按字符串编码（proto 的 uint64 -> string）
nlohmann::json intAttr(const std::string& key, long long v) {
    return nlohmann::json{{"key", key}, {"value", {{"intValue", std::to_string(v)}}}};
}

nlohmann::json boolAttr(const std::string& key, bool v) {
    return nlohmann::json{{"key", key}, {"value", {{"boolValue", v}}}};
}

nlohmann::json strArrayAttr(const std::string& key, const std::vector<std::string>& values) {
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& v : values) arr.push_back(nlohmann::json{{"stringValue", v}});
    return nlohmann::json{{"key", key}, {"value", {{"arrayValue", {{"values", arr}}}}}};
}

long long toNs(const std::string& rfc3339, long long fallbackNs) {
    const auto tp = parseRfc3339(rfc3339);
    if (!tp.ok()) return fallbackNs;
    return std::chrono::duration_cast<std::chrono::nanoseconds>(tp.value.time_since_epoch()).count();
}

// traceId / spanId 由 run id + 位置确定，重复导出得到同一组 id
std::string hexId(const std::string& seed, std::size_t len) { return sha256Hex(seed).substr(0, len); }

nlohmann::json makeSpan(const std::string& traceId, const std::string& spanId, const std::string& parentId,
                        const std::string& name, int kind, long long startNs, long long endNs,
                        nlohmann::json attributes, bool error, const std::string& errorMsg) {
    if (endNs < startNs) endNs = startNs;
    nlohmann::json j;
    j["traceId"] = traceId;
    j["spanId"] = spanId;
    if (!parentId.empty()) j["parentSpanId"] = parentId;
    j["name"] = name;
    j["kind"] = kind; // 1 = INTERNAL，3 = CLIENT
    j["startTimeUnixNano"] = std::to_string(startNs);
    j["endTimeUnixNano"] = std::to_string(endNs);
    j["attributes"] = std::move(attributes);
    if (error) j["status"] = nlohmann::json{{"code", 2}, {"message", errorMsg.empty() ? "error" : errorMsg}};
    return j;
}

// 一个 run 的全部 span，按 traceId 归组
nlohmann::json runSpans(const Run& run, const std::string& traceId) {
    nlohmann::json spans = nlohmann::json::array();
    const long long runStart = toNs(run.startedAt, 0);
    const long long runEnd = toNs(run.endedAt, runStart);
    const std::string rootId = hexId("root:" + run.id, 16);

    int toolCount = 0;
    for (const auto& s : run.steps) toolCount += static_cast<int>(s.tools.size());

    // 1. root span：整个 agent run
    {
        nlohmann::json attrs = nlohmann::json::array();
        attrs.push_back(strAttr("gen_ai.operation.name", "invoke_agent"));
        attrs.push_back(strAttr("gen_ai.system", "openai"));
        attrs.push_back(strAttr("agentxray.run.id", run.id));
        attrs.push_back(intAttr("agentxray.step.count", static_cast<long long>(run.steps.size())));
        attrs.push_back(intAttr("agentxray.tool.count", toolCount));
        attrs.push_back(boolAttr("agentxray.run.incomplete", run.incomplete));
        if (run.total.observed) {
            attrs.push_back(intAttr("gen_ai.usage.input_tokens", run.total.promptTokens));
            attrs.push_back(intAttr("gen_ai.usage.output_tokens", run.total.completionTokens));
        }
        std::vector<std::string> models;
        for (const auto& s : run.steps) {
            if (s.call.model.empty()) continue;
            bool seen = false;
            for (const auto& m : models)
                if (m == s.call.model) seen = true;
            if (!seen) models.push_back(s.call.model);
        }
        if (!models.empty()) attrs.push_back(strArrayAttr("gen_ai.request.models", models));

        std::string errMsg;
        if (run.incomplete) {
            errMsg = "run incomplete";
            for (const auto& m : run.missing) errMsg += "; " + m;
        }
        spans.push_back(makeSpan(traceId, rootId, "", "invoke_agent", 1, runStart, runEnd, std::move(attrs),
                                 run.incomplete, errMsg));
    }

    // 2. step（LLM 调用）与 3. tool 执行，都挂在 root 下面
    for (const auto& s : run.steps) {
        const std::string stepId = hexId("step:" + run.id + ":" + std::to_string(s.index), 16);
        const long long stepStart = toNs(s.call.startedAt, runStart);
        const long long stepEnd = toNs(s.call.endedAt, stepStart + s.response.latencyMs * 1000000LL);

        nlohmann::json attrs = nlohmann::json::array();
        attrs.push_back(strAttr("gen_ai.operation.name", "chat"));
        attrs.push_back(strAttr("gen_ai.system", "openai"));
        if (!s.call.model.empty()) attrs.push_back(strAttr("gen_ai.request.model", s.call.model));
        if (!s.call.endpoint.empty()) attrs.push_back(strAttr("agentxray.endpoint", s.call.endpoint));
        if (s.response.usage.observed) {
            attrs.push_back(intAttr("gen_ai.usage.input_tokens", s.response.usage.promptTokens));
            attrs.push_back(intAttr("gen_ai.usage.output_tokens", s.response.usage.completionTokens));
            if (s.response.usage.cachedTokens > 0)
                attrs.push_back(intAttr("agentxray.usage.cached_tokens", s.response.usage.cachedTokens));
        }
        if (!s.response.finishReason.empty())
            attrs.push_back(strArrayAttr("gen_ai.response.finish_reasons", {s.response.finishReason}));
        attrs.push_back(intAttr("agentxray.step.index", static_cast<long long>(s.index)));
        attrs.push_back(intAttr("agentxray.tool_definitions.count", static_cast<long long>(s.call.tools.size())));
        attrs.push_back(boolAttr("agentxray.streamed", s.response.streamed));
        attrs.push_back(boolAttr("agentxray.fuzzy_edge", s.fuzzyIn));
        attrs.push_back(boolAttr("gen_ai.usage.observed", s.response.usage.observed));

        const std::string name = s.call.model.empty() ? "chat" : ("chat " + s.call.model);
        const bool failed = !s.response.err.empty();
        spans.push_back(makeSpan(traceId, stepId, rootId, name, 3, stepStart, stepEnd, std::move(attrs), failed,
                                 s.response.err));

        for (std::size_t t = 0; t < s.tools.size(); ++t) {
            const ToolExec& te = s.tools[t];
            const std::string toolId =
                hexId("tool:" + run.id + ":" + std::to_string(s.index) + ":" + std::to_string(t), 16);
            // 工具在对应请求返回之后执行；没有实测耗时时退化为零时长点事件
            const long long toolStart = stepEnd;
            const long long toolEnd = toolStart + static_cast<long long>(te.durationMs) * 1000000LL;

            nlohmann::json tattrs = nlohmann::json::array();
            tattrs.push_back(strAttr("gen_ai.operation.name", "execute_tool"));
            tattrs.push_back(strAttr("gen_ai.tool.name", te.name));
            tattrs.push_back(strAttr("gen_ai.tool.type", "function"));
            if (!te.callId.empty()) tattrs.push_back(strAttr("gen_ai.tool.call.id", te.callId));
            tattrs.push_back(intAttr("agentxray.tool.duration_ms", te.durationMs));
            tattrs.push_back(strAttr("agentxray.tool.duration_source", te.durationSource));
            tattrs.push_back(strAttr("agentxray.tool.error_kind", te.errorKind));
            // 正文不导出：只给字节数，需要看内容就去本地 run.json
            tattrs.push_back(intAttr("agentxray.tool.arguments.bytes", static_cast<long long>(te.arguments.size())));
            tattrs.push_back(intAttr("agentxray.tool.result.bytes", static_cast<long long>(te.result.size())));

            const bool toolFailed = te.errorKind != "none" && !te.errorKind.empty();
            spans.push_back(makeSpan(traceId, toolId, stepId, "execute_tool " + te.name, 1, toolStart, toolEnd,
                                     std::move(tattrs), toolFailed, te.errorKind));
        }
    }
    return spans;
}

std::vector<Run> loadRunsInDir(const std::string& runDir) {
    std::vector<Run> runs;
    const std::filesystem::path dir(runDir);
    auto addFile = [&](const std::filesystem::path& p) {
        std::ifstream in(p.string(), std::ios::binary);
        if (!in) return;
        nlohmann::json j = nlohmann::json::parse(in, nullptr, false);
        if (j.is_discarded() || !j.is_object()) return;
        try {
            runs.push_back(j.get<Run>());
        } catch (...) {
            // 落盘文件损坏不影响其它 run
        }
    };
    addFile(dir / "run.json");
    for (int i = 0;; ++i) {
        const std::filesystem::path p = dir / ("run-" + std::to_string(i) + ".json");
        if (!std::filesystem::exists(p)) break;
        addFile(p);
    }
    return runs;
}

} // namespace

nlohmann::json buildOtlpPayload(const Run& run, const OtelOptions& opts) {
    const std::string traceId = hexId("trace:" + run.id, 32);
    nlohmann::json scopeSpans;
    scopeSpans["scope"] = nlohmann::json{{"name", "agentxray"}, {"version", "0.4.3"}};
    scopeSpans["spans"] = runSpans(run, traceId);

    nlohmann::json resource;
    resource["attributes"] = nlohmann::json::array({strAttr("service.name", opts.serviceName),
                                                    strAttr("service.version", "0.4.3")});
    nlohmann::json resourceSpan;
    resourceSpan["resource"] = std::move(resource);
    resourceSpan["scopeSpans"] = nlohmann::json::array({std::move(scopeSpans)});

    return nlohmann::json{{"resourceSpans", nlohmann::json::array({std::move(resourceSpan)})}};
}

int exportRunDirToOtel(const std::string& runDir, const OtelOptions& opts, std::string& err) {
    err.clear();
    if (opts.endpoint.empty()) return 0;
    const std::string& url = opts.endpoint;
    if (url.find("://") == std::string::npos) {
        err = "otel.endpoint 必须以 http:// 或 https:// 开头: " + url;
        return -1;
    }

    const std::vector<Run> runs = loadRunsInDir(runDir);
    int exported = 0;
    for (const auto& run : runs) {
        if (run.id.empty() || run.steps.empty()) continue;
        const nlohmann::json payload = buildOtlpPayload(run, opts);
        ForwardRequest req;
        req.method = "POST";
        req.url = url;
        req.body = payload.dump();
        req.headers = {{"Content-Type", "application/json"}};
        const ForwardResponse resp = forwardHttp(req, nullptr);
        if (resp.status >= 200 && resp.status < 300) {
            ++exported;
            continue;
        }
        err = resp.error.empty() ? ("OTLP 导出返回 HTTP " + std::to_string(resp.status))
                                 : ("OTLP 导出失败: " + resp.error);
        // 一次失败不继续重试其它 run，避免收尾被拖太久
        return exported;
    }
    return exported;
}

} // namespace axr
