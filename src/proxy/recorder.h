#pragma once
// 异步落盘（§7.1.5）：push 在热路径上只入队，IO 交给后台线程
#include <atomic>
#include <condition_variable>
#include <deque>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>

#include "proxy/event.h"

namespace axr {

class Recorder {
public:
    Recorder(const std::string& file, long long maxRunBytes, int maxQueue);
    ~Recorder();

    Recorder(const Recorder&) = delete;
    Recorder& operator=(const Recorder&) = delete;

    void push(Event e);   // 非阻塞，seq 在此分配
    void flush();         // 退出前调用：等待队列清空并落盘

    long long dropped() const { return dropped_.load(); }
    long long bytes() const { return bytes_.load(); }
    bool overLimit() const { return overLimit_.load(); }

private:
    void workerLoop();
    bool writeBatch(std::deque<Event>& batch);

    std::string file_;
    long long maxRunBytes_;
    std::size_t maxQueue_;

    std::mutex mu_;
    std::condition_variable cv_;
    std::condition_variable cvDone_;
    std::deque<Event> q_;
    bool stopping_ = false;
    bool writing_ = false;

    std::thread worker_;
    std::ofstream out_;
    std::atomic<long long> seq_{0};
    std::atomic<long long> dropped_{0};
    std::atomic<long long> bytes_{0};
    std::atomic<bool> overLimit_{false};
};

} // namespace axr
