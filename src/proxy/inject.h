#pragma once
// 请求体改写：流式请求补 stream_options.include_usage（§7.2.3，Mode A 的隐性收益）
// 以及模型名映射（客户端只认自家模型名时，转发前换成上游真实模型名）
#include <map>
#include <string>

namespace axr {

// 仅当 body 是 JSON 对象、stream=true 且尚未带 include_usage 时改写；
// 返回 true 表示发生了改写（已带 include_usage 或无需改写返回 false）
bool injectStreamUsage(std::string& bodyJson);

// 自动降级（P2-8）：上游 400 且响应体提到 stream_options 时，去掉注入后重试一次
bool shouldRetryWithoutUsage(int status, const std::string& responseBody);

// 把 body 里的 "model" 按 modelMap 改写（如 gpt-4o -> deepseek-chat）。
// 命中时返回 true，并在 applied 里填 "原模型名 -> 新模型名" 便于日志记录
bool rewriteModel(std::string& bodyJson, const std::map<std::string, std::string>& modelMap,
                  std::string* applied = nullptr);

} // namespace axr
