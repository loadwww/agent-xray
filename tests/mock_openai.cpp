// 测试台：可控的 OpenAI 兼容 mock（M2 起用于转发层验证，见 §11.1）
// 用法: mock_openai --port 9099
#include <cstdlib>
#include <iostream>
#include <string>

#include "httplib.h"
#include <nlohmann/json.hpp>

namespace {

std::string g_mode = "ok";     // ok | fail | slow | truncate
std::string g_case = "plain";  // plain | tool

std::string buildReply(const std::string& text, bool withTool, int promptTokens, int completionTokens) {
    nlohmann::json msg = {{"role", "assistant"}};
    nlohmann::json j;
    j["id"] = "chatcmpl-mock";
    j["object"] = "chat.completion";
    j["model"] = "deepseek-chat";

    if (withTool) {
        msg["content"] = nullptr;
        msg["tool_calls"] = nlohmann::json::array(
            {{{"id", "call_1"},
              {"type", "function"},
              {"function", {{"name", "get_weather"}, {"arguments", "{\"city\":\"北京\"}"}}}}});
        j["choices"] = nlohmann::json::array(
            {{{"index", 0}, {"message", msg}, {"finish_reason", "tool_calls"}}});
    } else {
        msg["content"] = text;
        j["choices"] = nlohmann::json::array(
            {{{"index", 0}, {"message", msg}, {"finish_reason", "stop"}}});
    }
    j["usage"] = {{"prompt_tokens", promptTokens}, {"completion_tokens", completionTokens}, {"total_tokens", promptTokens + completionTokens}};
    return j.dump();
}

namespace {

// 构造一个 SSE data 事件：delta 可为任意 JSON
std::string sseEvent(const nlohmann::json& delta, const std::string& finishReason) {
    nlohmann::json choice = {{"index", 0}, {"delta", delta}};
    if (!finishReason.empty()) choice["finish_reason"] = finishReason;
    nlohmann::json j;
    j["choices"] = nlohmann::json::array({choice});
    return "data: " + j.dump() + "\n\n";
}

std::string toolDelta(int index, const std::string& id, const std::string& name, const std::string& argsPart) {
    nlohmann::json fn = nlohmann::json::object();
    if (!name.empty()) fn["name"] = name;
    if (!argsPart.empty()) fn["arguments"] = argsPart;
    nlohmann::json tc = {{"index", index}};
    if (!id.empty()) {
        tc["id"] = id;
        tc["type"] = "function";
    }
    tc["function"] = fn;
    return sseEvent({{"tool_calls", nlohmann::json::array({tc})}}, "");
}

} // namespace

std::string buildSse(const std::string& text, bool withTool) {
    std::string out = sseEvent({{"role", "assistant"}, {"content", text}}, "");
    if (withTool) {
        out += toolDelta(0, "call_1", "get_weather", "{\"ci");
        out += toolDelta(0, "", "", "ty\":\"北");
        out += toolDelta(0, "", "", "京\"}");
    }
    out += sseEvent(nlohmann::json::object(), withTool ? "tool_calls" : "stop");
    nlohmann::json usage;
    usage["choices"] = nlohmann::json::array();
    usage["usage"] = {{"prompt_tokens", 40}, {"completion_tokens", 20}, {"total_tokens", 60}};
    out += "data: " + usage.dump() + "\n\n";
    out += "data: [DONE]\n\n";
    return out;
}

} // namespace

int main(int argc, char** argv) {
    int port = 9099;
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::string(argv[i]) == "--port") port = std::atoi(argv[i + 1]);
    }

    httplib::Server svr;

    svr.Get("/__mode/fail", [](const httplib::Request&, httplib::Response& res) {
        g_mode = "fail";
        res.set_content("{\"mode\":\"fail\"}", "application/json");
    });
    svr.Get("/__mode/slow", [](const httplib::Request&, httplib::Response& res) {
        g_mode = "slow";
        res.set_content("{\"mode\":\"slow\"}", "application/json");
    });
    svr.Get("/__mode/ok", [](const httplib::Request&, httplib::Response& res) {
        g_mode = "ok";
        res.set_content("{\"mode\":\"ok\"}", "application/json");
    });
    svr.Get("/__case/tool", [](const httplib::Request&, httplib::Response& res) {
        g_case = "tool";
        res.set_content("{\"case\":\"tool\"}", "application/json");
    });
    svr.Get("/__case/plain", [](const httplib::Request&, httplib::Response& res) {
        g_case = "plain";
        res.set_content("{\"case\":\"plain\"}", "application/json");
    });

    svr.Post("/v1/chat/completions", [](const httplib::Request& req, httplib::Response& res) {
        if (g_mode == "fail") {
            res.status = 500;
            res.set_content("{\"error\":{\"message\":\"mock failure\"}}", "application/json");
            return;
        }
        if (g_mode == "slow") std::system("ping -n 3 127.0.0.1 > nul");

        nlohmann::json body;
        try {
            body = nlohmann::json::parse(req.body);
        } catch (...) {
            res.status = 400;
            res.set_content("{\"error\":\"bad json\"}", "application/json");
            return;
        }
        const std::size_t msgCount = body.contains("messages") && body["messages"].is_array()
                                         ? body["messages"].size()
                                         : 0;
        const bool stream = body.value("stream", false);
        const bool withTool = (g_case == "tool" && msgCount <= 2);

        if (stream) {
            res.set_content(buildSse("北京", withTool), "text/event-stream");
        } else {
            res.set_content(buildReply("北京今天晴。", withTool, withTool ? 50 : 90, withTool ? 20 : 12),
                            "application/json");
        }
    });

    std::cout << "mock_openai listening on 127.0.0.1:" << port << "\n";
    if (!svr.listen("127.0.0.1", port)) {
        std::cerr << "listen failed\n";
        return 1;
    }
    return 0;
}
