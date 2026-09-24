// =============================================================================
// MolecuLens · app · data/log_buffer.h —— 日志环形缓冲（主方案 §14.5）
//
// spdlog sink 与界面之间的桥：sink 侧把每条日志压入固定容量环形缓冲，
// 界面侧每帧取一份快照用于"日志"面板渲染。
//
// 放在 data/ 而非 view/ —— 写侧在 shell/application.cpp（装配根），
// 读侧在 view/control/view_control.cpp，这里只做数据存放，两侧都只依赖本头。
// =============================================================================
#pragma once

#include <chrono>
#include <cstdio>
#include <ctime>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

#include <spdlog/details/log_msg.h>
#include <spdlog/sinks/base_sink.h>

namespace moleculens::app {

// 一条日志：级别 + 已格式化的时间与正文
struct LogRecord {
    spdlog::level::level_enum level = spdlog::level::info;
    std::string time;      // "HH:MM:SS"
    std::string text;      // 已格式化正文（不含级别/时间前缀）
};

// 固定容量环形缓冲。容量满后丢最旧的一条，不阻塞生产者。
// 线程安全：base_sink 已用 Mutex 保护 sink_it_，快照读取共用同一把锁。
class LogRingBuffer : public spdlog::sinks::base_sink<std::mutex> {
public:
    static constexpr std::size_t kCapacity = 2000;

    // 进程内单例（装配根安装 sink，界面读取，二者都不持有所有权）。
    // 堆分配且永不释放：spdlog 的静态析构与日志调用都可能晚于函数局部静态的
    // 销毁，落在已析构对象上会崩（同类问题见 VMA allocator 映射的处理）。
    static LogRingBuffer& instance() {
        static LogRingBuffer* inst = new LogRingBuffer();
        return *inst;
    }

    // 取一份快照（拷贝，调用方可脱离锁自由遍历）
    std::vector<LogRecord> snapshot() {
        std::lock_guard<std::mutex> lock(mutex_);
        return std::vector<LogRecord>(buf_.begin(), buf_.end());
    }

    void clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        buf_.clear();
    }

    std::size_t size() {
        std::lock_guard<std::mutex> lock(mutex_);
        return buf_.size();
    }

    // 累计压入条数（单调递增，不因环形淘汰而回退）。
    // 界面用它判断"有没有新日志"，比比较 size() 可靠——缓冲满后 size()
    // 恒为容量，内容却在滚动。
    std::uint64_t totalPushed() {
        std::lock_guard<std::mutex> lock(mutex_);
        return total_;
    }

protected:
    void sink_it_(const spdlog::details::log_msg& msg) override {
        LogRecord r;
        r.level = msg.level;
        const auto t = std::chrono::system_clock::to_time_t(msg.time);
        std::tm tm{};
#ifdef _WIN32
        localtime_s(&tm, &t);
#else
        localtime_r(&t, &tm);
#endif
        char stamp[16] = {};
        std::snprintf(stamp, sizeof(stamp), "%02d:%02d:%02d",
                      tm.tm_hour, tm.tm_min, tm.tm_sec);
        r.time = stamp;
        r.text.assign(msg.payload.data(), msg.payload.size());

        if (buf_.size() >= kCapacity) {
            buf_.pop_front();
        }
        buf_.push_back(std::move(r));
        ++total_;
    }

    void flush_() override {}

private:
    LogRingBuffer() = default;
    std::deque<LogRecord> buf_;
    std::uint64_t total_ = 0;
};

} // namespace moleculens::app
