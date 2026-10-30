#pragma once
// 完整性校验（§7.3.5）：缺请求时把"跳步"显式暴露，而不是静默出错
#include <string>
#include <vector>

#include "parse/model.h"

namespace axr {

// 返回 true 表示完整；missing 收集缺失点描述
bool validate(const Run& r, std::vector<std::string>& missing);

} // namespace axr
