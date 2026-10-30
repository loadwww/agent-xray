#pragma once
// OpenAI 兼容协议解析（覆盖 OpenAI / DeepSeek / Ollama / vLLM，见 §7.2）
#include <string>

#include <nlohmann/json.hpp>

#include "common/result.h"
#include "parse/model.h"

namespace axr {

// 解析统一 Message（content 支持 string 与 array 两种形态）
Message parseOpenAiMessage(const nlohmann::json& m);

// 解析 chat/completions 请求体；id / startedAt 由调用方填充
Result<LLMCall> parseOpenAiRequest(const nlohmann::json& body, const std::string& endpoint);

// 解析非流式响应体
Result<LLMResponse> parseOpenAiResponse(const nlohmann::json& body);

// 请求体是否要求流式
bool isStreamRequest(const nlohmann::json& body);

} // namespace axr
