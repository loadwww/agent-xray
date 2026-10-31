// spawnWithEnv 的 Linux/macOS 实现（posix_spawn，P1-4 / WP7）
// 与 Windows 版对齐的语义：继承控制台、等待子进程结束、返回退出码；
// 额外环境变量只作用于子进程（独立 envp，不改父进程 environ）。
// 用 posix_spawn 而不是 fork+exec：本进程是多线程的，fork 后子进程在 exec 前
// 构造 std::string / 调 write 都不是 async-signal-safe，可能与其它线程的 malloc 死锁。
#include "proxy/runner.h"

#include <map>
#include <string>
#include <vector>

#if !defined(_WIN32)
#include <cerrno>
#include <cstring>

#include <spawn.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

// libc 的进程环境表（全局作用域声明：进命名空间会被改名，找不到符号）
extern char** environ;

namespace axr {
namespace {

// 合并进程当前环境 + 覆盖项（同键覆盖，追加在末尾）
std::vector<std::string> buildEnv(const std::map<std::string, std::string>* extra) {
    std::vector<std::string> env;
    if (environ) {
        for (char** p = environ; *p; ++p) {
            const std::string entry(*p);
            const std::size_t eq = entry.find('=');
            const std::string key = (eq == std::string::npos) ? entry : entry.substr(0, eq);
            if (extra && extra->count(key)) continue; // 覆盖项统一在末尾追加
            env.push_back(entry);
        }
    }
    if (extra) {
        for (const auto& kv : *extra) env.push_back(kv.first + "=" + kv.second);
    }
    return env;
}

// 无斜杠的命令按 env 里的 PATH 搜索（execve 不做 PATH 搜索）
std::string resolveExecutable(const std::string& cmd, const std::vector<std::string>& env) {
    if (cmd.empty()) return "";
    if (cmd.find('/') != std::string::npos) return cmd;

    std::string path;
    for (const auto& e : env) {
        if (e.compare(0, 5, "PATH=") == 0) {
            path = e.substr(5);
            break;
        }
    }
    std::size_t start = 0;
    while (start <= path.size()) {
        std::size_t end = path.find(':', start);
        if (end == std::string::npos) end = path.size();
        const std::string dir = (start == end) ? "." : path.substr(start, end - start);
        const std::string candidate = dir + "/" + cmd;
        if (::access(candidate.c_str(), X_OK) == 0) return candidate;
        if (end == path.size()) break;
        start = end + 1;
    }
    return "";
}

} // namespace

Result<int> spawnWithEnv(const std::vector<std::string>& argv, const std::map<std::string, std::string>* extraEnv) {
    if (argv.empty()) return Result<int>::Fail("命令为空");

    const std::vector<std::string> env = buildEnv(extraEnv);
    const std::string prog = resolveExecutable(argv[0], env);
    if (prog.empty()) return Result<int>::Fail("未找到可执行文件: " + argv[0]);

    std::vector<char*> argPtrs;
    argPtrs.reserve(argv.size() + 1);
    for (const auto& a : argv) argPtrs.push_back(const_cast<char*>(a.c_str()));
    argPtrs.push_back(nullptr);

    std::vector<char*> envPtrs;
    envPtrs.reserve(env.size() + 1);
    for (const auto& e : env) envPtrs.push_back(const_cast<char*>(e.c_str()));
    envPtrs.push_back(nullptr);

    // posix_spawn：文件动作/属性都用默认（继承控制台、继承 fd），
    // 返回值直接是 errno（不会在子进程里拼错误字符串）
    pid_t pid = 0;
    const int spawnRc = posix_spawn(&pid, prog.c_str(), nullptr, nullptr, argPtrs.data(), envPtrs.data());
    if (spawnRc != 0) return Result<int>::Fail("posix_spawn 失败: " + std::string(std::strerror(spawnRc)));

    int status = 0;
    if (waitpid(pid, &status, 0) < 0) {
        return Result<int>::Fail("waitpid 失败: " + std::string(std::strerror(errno)));
    }
    if (WIFEXITED(status)) return Result<int>::Ok(WEXITSTATUS(status));
    if (WIFSIGNALED(status)) return Result<int>::Ok(128 + WTERMSIG(status));
    return Result<int>::Fail("子进程异常结束");
}

} // namespace axr

#endif // !_WIN32