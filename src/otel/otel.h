#pragma once
// B3：把一次 run 导出成 OTLP/HTTP traces（可选，默认关闭零网络）
#include <nlohmann/json.hpp>
#include <string>

#include "parse/model.h"

namespace axr {

struct OtelOptions {
    std::string endpoint; // 如 http://localhost:4318/v1/traces；空 = 关闭导出
    std::string serviceName = "agentxray";
};

// OTLP/HTTP JSON：一个 run = 一条 trace，run 是 root span，step / tool 是子 span，
// 带 gen_ai.* 语义属性。只导结构、计数与耗时，不导消息正文 / 工具参数与结果（隐私优先）。
nlohmann::json buildOtlpPayload(const Run& run, const OtelOptions& opts);

// 导出 run 目录下已落盘的全部 run（run.json / run-0.json ...）。endpoint 为空时直接返回成功。
// 返回导出的 run 数；失败原因写 err，不抛异常。
int exportRunDirToOtel(const std::string& runDir, const OtelOptions& opts, std::string& err);

} // namespace axr
