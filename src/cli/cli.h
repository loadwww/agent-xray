#pragma once
#include <string>
#include <vector>

namespace axr {

int cmdReplay(const std::vector<std::string>& args);
int cmdSelftest(const std::vector<std::string>& args);
int cmdServe(const std::vector<std::string>& args);
int cmdRun(const std::vector<std::string>& args);
int cmdShow(const std::vector<std::string>& args);
int cmdReport(const std::vector<std::string>& args);
int cmdList(const std::vector<std::string>& args);
int cmdExport(const std::vector<std::string>& args);
int cmdDiff(const std::vector<std::string>& args);
int cmdPrune(const std::vector<std::string>& args);

} // namespace axr
