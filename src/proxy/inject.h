#pragma once
// 请求体改写：流式请求补 stream_options.include_usage（§7.2.3，Mode A 的隐性收益）
#include <string>

namespace axr {

// 仅当 body 是 JSON 对象、stream=true 且尚未带 include_usage 时改写；
// 返回 true 表示发生了改写（已带 include_usage 或无需改写返回 false）
bool injectStreamUsage(std::string& bodyJson);

} // namespace axr
