// =============================================================================
// MolecuLens · engine · async/surface_jobs.h —— 表面生成的异步任务队列
// 规格来源：REMAINING_WORK §1.1「异步 + 渐进生成」（原 197 ms 主线程阻塞）
//
// 背景：
//   整体系 QuickSurf 的密度场 + Marching Cubes 单次约 197 ms，原先在 GUI 主线程的
//   `Renderer::prepare()` 里同步执行 → 每点一次「QuickSurf」界面就卡住近 0.2 s
//   （大体系线性增长）。本模块提供一个**单工作线程**的后台任务队列：
//     · 主线程提交任务（键 = 几何指纹，同键自动去重，重复提交不会排两次）；
//     · 工作线程算完后把结果写入样式层自己的缓存（quicksurf 的网格缓存，带互斥）；
//     · 主线程每帧调用 consumeSurfaceJobCompletion() 得知"有新结果"，随后重建命令流
//       时缓存命中，表面直接换上 —— 期间旧命令流继续显示（旧画面不消失）。
//
// 线程契约（务必遵守）：
//   · 任务体（work）只在工作线程执行；**禁止**触碰命令流/场景/GPU/ImGui ——
//     这些都属于主线程。任务只应做纯 CPU 计算并把结果写进线程安全的缓存。
//   · 每个任务必须自行处理异常（或让本模块吞掉）：失败时下一轮重建会重试。
// =============================================================================

#pragma once

#include <cstdint>
#include <functional>

namespace moleculens::engine::async {

// 提交一个后台表面生成任务。
//   key  —— 任务唯一键（通常取几何指纹）：同一 key 在排队/运行中时不会重复提交。
//   work —— 工作线程上执行的函数；结果写入线程安全缓存。
// 返回 true = 本次新入队；false = 同 key 已在队列/运行中（无需重复计算）。
bool submitSurfaceJob(uint64_t key, std::function<void()> work);

// 预取任务：优先级低于普通任务（普通任务先出队），且**不计入** surfaceJobsBusy /
// submittedSurfaceJobCount —— 预取不应让 Renderer 认为"当前帧命令流不完整"而丢弃
// 重建结果。结果同样写缓存并触发完成通知，播放到该帧时直接命中。
bool submitPrefetchSurfaceJob(uint64_t key, std::function<void()> work);

// 是否仍有排队或运行中的任务（UI 显示"生成中…"、导出等待判定用）。
bool surfaceJobsBusy();

// 自上次调用后是否有任务完成（主线程消费；返回 true 表示"有结果可换入"）。
bool consumeSurfaceJobCompletion();

// 排队中 + 运行中的任务数。
int pendingSurfaceJobCount();

// 累计提交数（单调递增；不含去重被拒的提交）。重建时用它判断"本轮是否新提交了
// 表面任务"，从而决定是保留旧命令流还是换入新命令流。
uint64_t submittedSurfaceJobCount();

// 阻塞直到队列清空且没有运行中的任务（**导出线程**用；不要在 UI 主线程调用）。
void waitForSurfaceJobs();

// 停止并等待工作线程退出。幂等；之后若再次提交任务会自动重启工作线程。
// Renderer 析构时调用（保证进程退出/测试收尾时没有游离线程访问已析构的缓存）。
void shutdownSurfaceJobs();

} // namespace moleculens::engine::async
