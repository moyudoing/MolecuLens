// =============================================================================
// MolecuLens · tests · engine/scheduler/frame_scheduler_tests.cpp
// 规格来源：主方案 §14.4.2（基本推进/累积器/速度倍率/选区循环/选区停止/
//           手动控制/边界）
// =============================================================================

#include <catch2/catch_test_macros.hpp>

#include "engine/scheduler/frame_scheduler.h"

using namespace moleculens::engine;

TEST_CASE("FrameScheduler basic advance", "[scheduler]") {
    FrameScheduler s;
    s.setFrameCount(100);
    s.setDtFrame(2.0f);           // 物理 dt 只用于显示，不再参与实时定时
    s.setPlaybackFps(1.0f);       // 1 FPS → 每 1.0 s 推进一帧
    s.setLoop(false);
    s.play();
    REQUIRE(s.advance(1.0f));       // dtReal=1.0 → 步进 1
    REQUIRE(s.frame() == 1);
    REQUIRE_FALSE(s.advance(0.0f)); // 0 累积 → 不步进（返回 false）
    REQUIRE(s.frame() == 1);
}

TEST_CASE("FrameScheduler accumulator", "[scheduler]") {
    FrameScheduler s;
    s.setFrameCount(100);
    s.setDtFrame(2.0f);
    s.setPlaybackFps(1.0f);
    s.setLoop(false);
    s.play();
    // dtReal=0.25 ×4 → 第 4 次才步进（不丢帧）
    REQUIRE_FALSE(s.advance(0.25f));
    REQUIRE_FALSE(s.advance(0.25f));
    REQUIRE_FALSE(s.advance(0.25f));
    REQUIRE(s.advance(0.25f));
    REQUIRE(s.frame() == 1);
}

TEST_CASE("FrameScheduler speed multiplier", "[scheduler]") {
    FrameScheduler s;
    s.setFrameCount(100);
    s.setDtFrame(2.0f);
    s.setPlaybackFps(1.0f);
    s.setSpeed(2.0f);
    s.setLoop(false);
    s.play();
    REQUIRE(s.advance(1.0f));       // 1.0 s × 2× / (1/1 FPS) = 2 步
    REQUIRE(s.frame() == 2);
}

TEST_CASE("FrameScheduler loop at outFrame", "[scheduler]") {
    FrameScheduler s;
    s.setFrameCount(10);
    s.setDtFrame(1.0f);
    s.setPlaybackFps(1.0f);
    s.setInOut(2, 5);
    s.setLoop(true);
    s.play();
    s.seek(5);
    REQUIRE(s.advance(1.0f));
    REQUIRE(s.frame() == 2);        // 回到 inFrame 继续播放
    REQUIRE(s.playing());
}

TEST_CASE("FrameScheduler stop at outFrame without loop", "[scheduler]") {
    FrameScheduler s;
    s.setFrameCount(10);
    s.setDtFrame(1.0f);
    s.setPlaybackFps(1.0f);
    s.setInOut(0, 5);
    s.setLoop(false);
    s.play();
    s.seek(5);
    REQUIRE(s.advance(1.0f));
    REQUIRE(s.frame() == 5);
    REQUIRE_FALSE(s.playing());     // 停在 outFrame 并 pause
}

TEST_CASE("FrameScheduler manual control", "[scheduler]") {
    FrameScheduler s;
    s.setFrameCount(10);
    s.seek(50);
    REQUIRE(s.frame() == 9);        // clamp
    s.step(-1);
    REQUIRE(s.frame() == 8);
    s.gotoFirst();
    REQUIRE(s.frame() == 0);
    s.gotoLast();
    REQUIRE(s.frame() == 9);
}

// 播放推进必须由**调用方传入的真实 dt** 与**播放帧率**共同决定（与渲染帧率解耦）。
// 此前 TimelineController::poll 硬编码 advance(1/60)：帧率 30fps 时每秒只推进
// 30×dtFrame/60，播放速度随帧率变化。下面用两种"总时长相同、每帧 dt 不同"的
// 调用方式验证结果一致；dtFrame（物理时间步长）完全不参与。
TEST_CASE("FrameScheduler advance is frame-rate independent",
          "[scheduler]") {
    const auto runFor = [](float dt, int calls) {
        FrameScheduler s;
        s.setFrameCount(1000);
        s.setDtFrame(0.5f);
        s.setPlaybackFps(1.0f);   // 1 FPS → 每 1 s 一帧
        s.setLoop(false);
        s.play();
        for (int i = 0; i < calls; ++i) {
            s.advance(dt);
        }
        return s.frame();
    };
    // 0.1s × 10 与 0.5s × 2 —— 总时长都是 1.0s → 都恰好推进 1 帧
    REQUIRE(runFor(0.1f, 10) == 1);
    REQUIRE(runFor(0.5f, 2) == 1);
    // 总时长 2.0s → 2 帧（与切分方式无关）
    REQUIRE(runFor(0.1f, 20) == 2);
    REQUIRE(runFor(0.25f, 8) == 2);
}

// 物理 dt（10 ps 之类）绝不能被当作实时步进间隔 —— 那会变成"10 秒一帧"，
// 也就是用户反馈的"不能自动播放"。此处固化：同一 dt 下，只有播放帧率决定速度。
TEST_CASE("FrameScheduler playback rate is independent of physical dt",
          "[scheduler]") {
    FrameScheduler slow;
    slow.setFrameCount(1000);
    slow.setDtFrame(10.0f);      // 物理步长 10 ps
    slow.setPlaybackFps(30.0f);
    slow.setLoop(false);
    slow.play();
    slow.advance(1.0f);
    REQUIRE(slow.frame() == 30);  // 1 秒推进 30 帧，而不是 0 帧

    FrameScheduler fast;
    fast.setFrameCount(1000);
    fast.setDtFrame(0.01f);      // 物理步长 0.01 ps
    fast.setPlaybackFps(30.0f);
    fast.setLoop(false);
    fast.play();
    fast.advance(1.0f);
    REQUIRE(fast.frame() == 30);  // 与物理 dt 无关
}

TEST_CASE("FrameScheduler selection loop wraps inside [in, out]",
          "[scheduler]") {
    FrameScheduler s;
    s.setFrameCount(100);
    s.setDtFrame(1.0f);
    s.setInOut(10, 20);
    s.setLoop(true);

    // 从区间中间前进到出点 → 回绕到入点（不是 0，也不是整段起点）
    s.seek(20);
    s.step(1);
    REQUIRE(s.frame() == 10);

    // 从入点后退 → 停在入点
    s.step(-1);
    REQUIRE(s.frame() == 10);

    // 越界 seek 夹到区间内（seek 只夹到 [0, frameCount-1]，故此处落在 in 侧）
    s.seek(5);
    s.step(0);
    REQUIRE(s.frame() == 10);
}

// setFrameCount 会把 out 上界重置为末帧（FrameScheduler 的既定行为）；
// TimelineController 依赖这一点在换轨迹后重新应用用户选区 —— 此处固化该契约，
// 若行为被改动，控制器侧的补偿逻辑会失效。
TEST_CASE("FrameScheduler setFrameCount resets the out bound", "[scheduler]") {
    FrameScheduler s;
    s.setFrameCount(100);
    s.setInOut(10, 20);
    s.seek(20);
    s.setFrameCount(30);            // 换轨迹 / 重新激活
    s.step(1000);                   // 一路步进出点 → 无循环则停在 out
    s.setLoop(false);
    s.step(1000);
    REQUIRE(s.frame() == 29);       // out 已被重置为新的末帧 29
}

TEST_CASE("FrameScheduler edge cases", "[scheduler]") {
    FrameScheduler empty;
    empty.setFrameCount(0);
    empty.play();
    REQUIRE_FALSE(empty.advance(1.0f));
    REQUIRE(empty.frame() == 0);

    FrameScheduler single;
    single.setFrameCount(1);
    single.play();
    REQUIRE_FALSE(single.advance(1.0f));
    REQUIRE(single.frame() == 0);
    single.step(5);
    REQUIRE(single.frame() == 0);
}
