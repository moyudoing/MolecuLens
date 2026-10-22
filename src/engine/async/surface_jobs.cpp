// =============================================================================
// MolecuLens · engine · async/surface_jobs.cpp —— 表面生成后台工作线程实现
// 见 surface_jobs.h：单线程 FIFO + 同键去重 + 完成计数通知。
// =============================================================================

#include "engine/async/surface_jobs.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <unordered_set>
#include <utility>

namespace moleculens::engine::async {

namespace {

struct Job {
    uint64_t key = 0;
    std::function<void()> work;
};

struct State {
    std::mutex mtx;
    std::condition_variable cv;

    std::deque<Job> queue;          // 当前帧/阻塞任务（优先执行）
    std::deque<Job> prefetchQueue;  // 后续帧预取（低优先级，不计入 busy）
    // 排队中或正在运行的任务键（去重集合）：任务开始时仍在集合里，完成后才移除，
    // 因此"运行中"也算 pending —— 避免主线程每帧重复提交同一个几何指纹。
    std::unordered_set<uint64_t> keys;
    std::thread worker;
    bool workerStarted = false;
    bool running = false;       // 工作线程正在执行某个任务
    bool runningNormal = false; // 正在执行的是普通任务（预取不算 busy）
    bool stop = false;          // 请求工作线程退出（shutdown）

    std::atomic<uint64_t> completed{0};   // 已完成任务总数
    std::atomic<uint64_t> consumed{0};    // 主线程已消费的完成数
    std::atomic<uint64_t> submitted{0};   // 累计成功提交数（去重被拒不计入）
};

State& state() {
    static State s;
    return s;
}

// 工作线程主循环：取队首任务执行 → 通知完成。异常一律吞掉（下一轮重建会重试）。
void workerMain() {
    State& s = state();
    std::unique_lock<std::mutex> lk(s.mtx);
    for (;;) {
        s.cv.wait(lk, [&s] { return s.stop || !s.queue.empty() || !s.prefetchQueue.empty(); });
        if (s.stop && s.queue.empty() && s.prefetchQueue.empty()) {
            break;
        }
        Job job;
        bool normal = false;
        if (!s.queue.empty()) {
            job = std::move(s.queue.front());
            s.queue.pop_front();
            normal = true;
        } else {
            job = std::move(s.prefetchQueue.front());
            s.prefetchQueue.pop_front();
        }
        s.running = true;
        s.runningNormal = normal;
        lk.unlock();
        try {
            if (job.work) {
                job.work();
            }
        } catch (...) {
            // 生成失败不应让进程崩溃：丢弃该次结果，主线程之后的重建会重新提交。
        }
        lk.lock();
        s.running = false;
        s.runningNormal = false;
        s.keys.erase(job.key);
        s.completed.fetch_add(1, std::memory_order_release);
        s.cv.notify_all();   // 唤醒 waitForSurfaceJobs / 其它等待者
    }
}

// 前提：持有 s.mtx。首次提交时按需启动工作线程。
void ensureWorkerLocked(State& s) {
    if (s.workerStarted) {
        return;
    }
    s.workerStarted = true;
    s.stop = false;
    s.worker = std::thread(&workerMain);
}

} // namespace

bool submitSurfaceJob(uint64_t key, std::function<void()> work) {
    State& s = state();
    std::lock_guard<std::mutex> lk(s.mtx);
    if (s.keys.find(key) != s.keys.end()) {
        // 同键任务已在排队/运行：虽然不重复入队，但调用方"本轮缺少该表面"的
        // 事实仍然成立。Renderer 用 submitted 计数判断命令流是否完整，若这里
        // 不计数，它会误把缺少表面的命令流换入 → 播放时表面闪断。
        s.submitted.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    ensureWorkerLocked(s);
    s.keys.insert(key);
    s.queue.push_back(Job{key, std::move(work)});
    s.submitted.fetch_add(1, std::memory_order_relaxed);
    s.cv.notify_one();
    return true;
}

bool submitPrefetchSurfaceJob(uint64_t key, std::function<void()> work) {
    State& s = state();
    std::lock_guard<std::mutex> lk(s.mtx);
    if (s.keys.find(key) != s.keys.end()) {
        return false;
    }
    ensureWorkerLocked(s);
    s.keys.insert(key);
    s.prefetchQueue.push_back(Job{key, std::move(work)});
    s.cv.notify_one();
    return true;
}

bool surfaceJobsBusy() {
    State& s = state();
    std::lock_guard<std::mutex> lk(s.mtx);
    return s.runningNormal || !s.queue.empty();
}

bool consumeSurfaceJobCompletion() {
    State& s = state();
    const uint64_t done = s.completed.load(std::memory_order_acquire);
    const uint64_t seen = s.consumed.load(std::memory_order_relaxed);
    if (done == seen) {
        return false;
    }
    s.consumed.store(done, std::memory_order_relaxed);
    return true;
}

int pendingSurfaceJobCount() {
    State& s = state();
    std::lock_guard<std::mutex> lk(s.mtx);
    return static_cast<int>(s.queue.size() + s.prefetchQueue.size()) + (s.running ? 1 : 0);
}

uint64_t submittedSurfaceJobCount() {
    return state().submitted.load(std::memory_order_relaxed);
}

void waitForSurfaceJobs() {
    State& s = state();
    std::unique_lock<std::mutex> lk(s.mtx);
    s.cv.wait(lk, [&s] {
        return s.queue.empty() && s.prefetchQueue.empty() && !s.running;
    });
}

void shutdownSurfaceJobs() {
    State& s = state();
    std::thread toJoin;
    {
        std::lock_guard<std::mutex> lk(s.mtx);
        if (!s.workerStarted) {
            return;
        }
        s.stop = true;
        toJoin = std::move(s.worker);
        s.workerStarted = false;
        s.cv.notify_all();
    }
    if (toJoin.joinable()) {
        toJoin.join();
    }
    // 退出后清空残留状态，允许后续再次提交（测试/重启场景）
    std::lock_guard<std::mutex> lk(s.mtx);
    s.queue.clear();
    s.prefetchQueue.clear();
    s.keys.clear();
    s.stop = false;
    s.running = false;
    s.runningNormal = false;
}

} // namespace moleculens::engine::async
