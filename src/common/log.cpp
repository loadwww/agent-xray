#include "common/log.h"

#include <iostream>

namespace axr {
namespace {
bool g_verbose = false;
}

void setVerbose(bool v) { g_verbose = v; }
bool verbose() { return g_verbose; }

void log(LogLevel level, const std::string& tag, const std::string& msg) {
    if (level == LogLevel::Debug && !g_verbose) return;
    const char* name = "INFO";
    switch (level) {
        case LogLevel::Debug: name = "DEBUG"; break;
        case LogLevel::Info:  name = "INFO";  break;
        case LogLevel::Warn:  name = "WARN";  break;
        case LogLevel::Error: name = "ERROR"; break;
    }
    std::cerr << "[" << name << "][" << tag << "] " << msg << "\n";
}

} // namespace axr
