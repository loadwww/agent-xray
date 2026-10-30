#include "proxy/recorder.h"

#include "common/log.h"
#include "common/time_util.h"

namespace axr {

Recorder::Recorder(const std::string& file, long long maxRunBytes, int maxQueue)
    : file_(file), maxRunBytes_(maxRunBytes), maxQueue_(static_cast<std::size_t>(maxQueue > 0 ? maxQueue : 10000)) {
    out_.open(file_, std::ios::binary | std::ios::trunc);
    if (!out_) logError("recorder", "无法打开事件文件: " + file_);
    worker_ = std::thread([this] { workerLoop(); });
}

Recorder::~Recorder() {
    {
        std::lock_guard<std::mutex> lk(mu_);
        stopping_ = true;
    }
    cv_.notify_all();
    if (worker_.joinable()) worker_.join();
    if (out_.is_open()) out_.flush();
}

void Recorder::push(Event e) {
    if (overLimit_.load()) return;
    if (e.ts.empty()) e.ts = nowRfc3339();
    e.seq = ++seq_;

    std::lock_guard<std::mutex> lk(mu_);
    if (stopping_) return;
    if (q_.size() >= maxQueue_) {
        ++dropped_;
        return;
    }
    q_.push_back(std::move(e));
    cv_.notify_one();
}

void Recorder::flush() {
    std::unique_lock<std::mutex> lk(mu_);
    cv_.notify_all();
    cvDone_.wait(lk, [this] { return q_.empty() && !writing_; });
    if (out_.is_open()) out_.flush();
}

void Recorder::workerLoop() {
    std::deque<Event> batch;
    while (true) {
        {
            std::unique_lock<std::mutex> lk(mu_);
            cv_.wait(lk, [this] { return stopping_ || !q_.empty(); });
            if (q_.empty() && stopping_) break;
            batch.swap(q_);
            writing_ = true;
        }
        writeBatch(batch);
        batch.clear();
        {
            std::lock_guard<std::mutex> lk(mu_);
            writing_ = false;
        }
        cvDone_.notify_all();
    }
}

bool Recorder::writeBatch(std::deque<Event>& batch) {
    if (!out_.is_open()) return false;
    for (const auto& e : batch) {
        std::string line = e.toJson().dump();
        line.push_back('\n');
        if (bytes_.load() + static_cast<long long>(line.size()) > maxRunBytes_) {
            overLimit_ = true;
            logWarn("recorder", "达到 max_run_bytes，停止录制");
            break;
        }
        out_.write(line.data(), static_cast<std::streamsize>(line.size()));
        bytes_ += static_cast<long long>(line.size());
    }
    out_.flush();
    return true;
}

} // namespace axr
