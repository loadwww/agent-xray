#include "cli/cli.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <memory>
#include <mutex>
#include <thread>

#include "cli/cli_common.h"
#include "common/log.h"
#include "proxy/recorder.h"
#include "proxy/server.h"
#include "store/finalize.h"
#include "store/run_dir.h"
#include "store/run_index.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <csignal>
#endif

namespace axr {
namespace {

#ifdef _WIN32
std::atomic<bool> g_stop{false};

BOOL WINAPI onCtrlSignal(DWORD type) {
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT || type == CTRL_CLOSE_EVENT) {
        g_stop = true;
        return TRUE; // 交给我们自己收尾，不立即终止进程
    }
    return FALSE;
}
#else
// WP5：POSIX 下 Ctrl+C / SIGTERM 只置标志，收尾在主循环退出后做。
// handler 内不能做 IO、不能构造对象、不能加锁，否则可能死锁。
volatile sig_atomic_t g_stopSig = 0;

void onPosixSignal(int) { g_stopSig = 1; }
#endif

void printSummary(const RunSummary& s) {
    if (s.status == "empty") {
        std::cout << "  （未捕获到 LLM 流量）\n";
        return;
    }
    std::cout << "  " << s.stepCount << " steps · " << s.toolCount << " tools · "
              << (s.usageObserved ? std::to_string(s.totalTokens) : std::string("—")) << " tokens"
              << (s.incomplete ? " · ⚠ incomplete" : "") << "\n";
    for (const auto& m : s.missing) std::cout << "  ! " << m << "\n";
}

// P2-15：一次 serve 可能跨多个「会话」。空闲超过阈值时把当前 events 收尾成 run 产物，
// 再开一个新目录继续采，避免长驻进程攒出巨型 events.jsonl，也让已结束的任务先出现在列表里。
class ServeSessions {
public:
    ServeSessions(const Config& cfg, ReverseProxySource& source) : cfg_(cfg), source_(source) {}

    // WP6：懒建会话。start() 只记录配置，不在启动时创建目录；
    // 首个事件到来时才 beginLocked()，避免 serve 空跑留下 0 步的“无 meta”噪音目录。
    bool start() { return true; }

    const std::string& runDir() const { return runDir_; }
    const std::string& sessionId() const { return sessionId_; }

    void push(const Event& e) {
        std::lock_guard<std::mutex> lk(mu_);
        if (!rec_) {
            if (!beginLocked()) {
                logWarn("serve", "无法创建会话目录，事件已丢弃");
                return;
            }
            std::cout << "  新会话: " << runDir_ << "（开始采集）\n";
        }
        rec_->push(e);
        ++events_;
        lastEvent_ = std::chrono::steady_clock::now();
    }

    // 空闲分钟数 <=0 时关闭自动收尾；当前会话还没收到任何事件也不轮转
    void rotateIfIdle(int minutes) {
        if (minutes <= 0) return;
        std::lock_guard<std::mutex> lk(mu_);
        if (!rec_ || events_ == 0) return;
        const auto idle = std::chrono::steady_clock::now() - lastEvent_;
        if (idle < std::chrono::minutes(minutes)) return;

        const std::string oldSid = sessionId_;
        RunSummary s = finalizeLocked();
        std::cout << "\n✓ 空闲 " << minutes << " 分钟，已收尾会话: " << oldSid << "\n";
        printSummary(s);
        if (s.status != "empty")
            std::cout << "  流程图: " << (std::filesystem::path(s.runDir) / "graph.mmd").string() << "\n";

        if (!beginLocked()) {
            logError("serve", "无法创建新会话，后续事件将丢弃");
            return;
        }
        std::cout << "  新会话: " << runDir_ << "（继续采集）\n";
    }

    void finish() {
        std::lock_guard<std::mutex> lk(mu_);
        if (!rec_) {
            // WP6：懒建会话后，无流量退出时根本没有目录，不写 meta
            std::cout << "✓ 采集结束（未捕获到任何请求，未生成会话目录）\n";
            return;
        }
        const std::string sid = sessionId_;
        const RunSummary s = finalizeLocked();
        std::cout << "✓ 采集完成\n  run: " << sid << "\n";
        printSummary(s);
        if (s.status != "empty")
            std::cout << "  流程图: " << (std::filesystem::path(s.runDir) / "graph.mmd").string() << "\n";
    }

private:
    bool beginLocked() {
        const std::string sid = makeSessionId();
        const std::string dir = ensureRunDir(cfg_.storage.root, sid);
        if (dir.empty()) {
            logError("serve", "无法创建 run 目录: " + cfg_.storage.root);
            return false;
        }
        rec_ = std::make_unique<Recorder>((std::filesystem::path(dir) / "events.jsonl").string(),
                                          cfg_.proxy.maxRunBytes, cfg_.proxy.maxQueue);
        sessionId_ = sid;
        runDir_ = dir;
        events_ = 0;
        baseDropped_ = rec_->dropped();
        baseRewritten_ = source_.rewrittenRequests();
        lastEvent_ = std::chrono::steady_clock::now();
        return true;
    }

    // 调用前需持有 mu_；返回前会释放 Recorder（flush + 关闭 events.jsonl）
    RunSummary finalizeLocked() {
        rec_->flush();
        const long long dropped = rec_->dropped() - baseDropped_;
        const int rewritten = source_.rewrittenRequests() - baseRewritten_;
        const RunSummary s = finalizeRun(runDir_, cfg_, dropped, rewritten, source_.usageInjectionDisabled());
        rec_.reset();
        return s;
    }

    const Config& cfg_;
    ReverseProxySource& source_;
    std::mutex mu_;
    std::unique_ptr<Recorder> rec_;
    std::string sessionId_;
    std::string runDir_;
    long long events_ = 0;
    long long baseDropped_ = 0;
    int baseRewritten_ = 0;
    std::chrono::steady_clock::time_point lastEvent_{};
};

} // namespace

int cmdServe(const std::vector<std::string>& args) {
    ProxyCliOpts opts;
    auto parsed = parseProxyArgs(args, opts);
    if (!parsed.ok() || !opts.command.empty()) {
        std::cerr << (parsed.ok() ? "错误: serve 不接受 -- 之后的命令\n" : ("错误: " + parsed.err + "\n"))
                  << "用法: axr serve [--config config.yaml] [--root dir] [--listen port] [--bind host]"
                     " [--allow-remote]\n";
        return 2;
    }

    auto cfg = loadEffectiveConfig(opts);
    if (!cfg.ok()) {
        logError("serve", cfg.err);
        return 2;
    }

    auto bindOk = enforceBindPolicy(cfg.value, opts.allowRemote);
    if (!bindOk.ok()) {
        logError("serve", bindOk.err);
        return 2;
    }

    // P2-16：启动时按 retention_days 清一次旧 run（<=0 关闭自动清理）
    // WP6：懒建会话后首次启动可能还没有 runs 目录，这不算失败，直接跳过
    const std::filesystem::path runsDir = std::filesystem::path(cfg.value.storage.root) / "runs";
    if (cfg.value.storage.retentionDays > 0 && std::filesystem::exists(runsDir)) {
        auto pruned = pruneRuns(cfg.value.storage.root, cfg.value.storage.retentionDays, true);
        if (!pruned.ok()) {
            logWarn("serve", "启动清理失败: " + pruned.err);
        } else if (pruned.value.removed > 0) {
            logInfo("serve", "启动清理: 已删除 " + std::to_string(pruned.value.removed) + " 个旧 run（mtime >= " +
                                  std::to_string(cfg.value.storage.retentionDays) + " 天）");
        }
    }

    ReverseProxySource source(cfg.value);
    ServeSessions sessions(cfg.value, source);

    // WP5：尽早注册退出信号。端口在 source.start() 内就开始监听，
    // 若 handler 晚于监听注册，这段窗口内的 Ctrl+C 会走默认动作直接杀进程、丢掉收尾。
#ifdef _WIN32
    SetConsoleCtrlHandler(onCtrlSignal, TRUE);
#else
    std::signal(SIGINT, onPosixSignal);
    std::signal(SIGTERM, onPosixSignal);
#endif

    if (!sessions.start()) return 1;
    if (!source.start([&sessions](const Event& e) { sessions.push(e); })) return 1;

    const std::string base = "http://" + cfg.value.proxy.bind + ":" + std::to_string(cfg.value.proxy.listen);
    std::cout << "AgentXray 代理已启动\n"
              << "  监听: " << base << "\n"
              << "  用法: OPENAI_BASE_URL=" << base << "/v1\n"
              << "  页面: " << base << "/__xray （浏览器打开可看实时流程图）\n"
              << "  会话: 首个请求到达时自动创建（无流量不落盘）\n";
    if (cfg.value.storage.idleFinalizeMinutes > 0)
        std::cout << "  空闲 " << cfg.value.storage.idleFinalizeMinutes
                  << " 分钟自动收尾并开新会话（storage.idle_finalize_minutes，<=0 关闭）\n";
    std::cout << "  按 Ctrl+C 结束并生成分析产物\n";

#ifdef _WIN32
    while (!g_stop.load() && source.running()) {
        sessions.rotateIfIdle(cfg.value.storage.idleFinalizeMinutes);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
#else
    // SIGINT（Ctrl+C）与 SIGTERM（systemd / kill）都走同一条收尾路径
    while (g_stopSig == 0 && source.running()) {
        sessions.rotateIfIdle(cfg.value.storage.idleFinalizeMinutes);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
#endif

    std::cout << "\n正在收尾...\n";
    source.stop();
    sessions.finish();
    return 0;
}

} // namespace axr
