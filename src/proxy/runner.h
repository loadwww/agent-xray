#pragma once
// axr run 的核心（§7.1.6）：起代理 -> 注入环境变量 -> 起子进程 -> 收尾分析
#include <map>
#include <string>
#include <vector>

#include "config/config.h"
#include "common/result.h"
#include "store/finalize.h"

namespace axr {

// 启动录制会话、拉起目标命令、结束后自动分析
Result<RunSummary> runTarget(const Config& cfg, const std::vector<std::string>& command, int& childExitCode);

// 创建子进程（继承控制台），额外环境变量只作用于子进程
Result<int> spawnWithEnv(const std::vector<std::string>& argv, const std::map<std::string, std::string>* extraEnv);

} // namespace axr
