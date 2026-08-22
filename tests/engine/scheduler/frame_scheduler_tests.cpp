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
    s.setDtFrame(2.0f);
    s.setLoop(false);
    s.play();
    REQUIRE(s.advance(2.0f));       // dtReal=2.0 → 步进 1
    REQUIRE(s.frame() == 1);
    REQUIRE_FALSE(s.advance(0.0f)); // 0 累积 → 不步进（返回 false）
    REQUIRE(s.frame() == 1);
}

TEST_CASE("FrameScheduler accumulator", "[scheduler]") {
    FrameScheduler s;
    s.setFrameCount(100);
    s.setDtFrame(2.0f);
    s.setLoop(false);
    s.play();
    // dtReal=0.5 ×4 → 第 4 次才步进（不丢帧）
    REQUIRE_FALSE(s.advance(0.5f));
    REQUIRE_FALSE(s.advance(0.5f));
    REQUIRE_FALSE(s.advance(0.5f));
    REQUIRE(s.advance(0.5f));
    REQUIRE(s.frame() == 1);
}

TEST_CASE("FrameScheduler speed multiplier", "[scheduler]") {
    FrameScheduler s;
    s.setFrameCount(100);
    s.setDtFrame(2.0f);
    s.setSpeed(2.0f);
    s.setLoop(false);
    s.play();
    REQUIRE(s.advance(2.0f));       // 2.0*2.0/2.0 = 2 步
    REQUIRE(s.frame() == 2);
}

TEST_CASE("FrameScheduler loop at outFrame", "[scheduler]") {
    FrameScheduler s;
    s.setFrameCount(10);
    s.setDtFrame(1.0f);
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
