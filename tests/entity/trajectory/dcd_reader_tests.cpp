// =============================================================================
// MolecuLens · tests · entity/trajectory/dcd_reader_tests.cpp —— DCD 读取器测试
//
// 不引入二进制夹具：测试内用最小写入器现场构造 DCD 字节流再读回，
// 这样格式字段（NSET/NSAVC/DELTA/帧记录长度标记）与读取端一一对照，
// 断言失败时能直接定位是哪个字段理解错了。
// =============================================================================

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "entity/trajectory/dcd_reader.h"

using namespace moleculens::entity;

namespace {

void putI32(std::vector<uint8_t>& v, int32_t x) {
    const auto* p = reinterpret_cast<const uint8_t*>(&x);
    v.insert(v.end(), p, p + 4);
}
void putF32(std::vector<uint8_t>& v, float x) {
    const auto* p = reinterpret_cast<const uint8_t*>(&x);
    v.insert(v.end(), p, p + 4);
}
void putF64(std::vector<uint8_t>& v, double x) {
    const auto* p = reinterpret_cast<const uint8_t*>(&x);
    v.insert(v.end(), p, p + 8);
}

// 构造最小合法 DCD：固定头(84) + 标题记录 + 原子数记录 [+ DELTA] + N 帧 XYZ
std::vector<uint8_t> makeDcd(uint32_t natom, uint32_t nframes, int32_t nsavc,
                             double delta) {
    std::vector<uint8_t> d;
    putI32(d, 84);                                    // [0] 记录长度
    d.insert(d.end(), {'C', 'O', 'R', 'D'});          // [4] 标识
    putI32(d, static_cast<int32_t>(nframes));         // [8] NSET
    putI32(d, 1);                                     // [12] ISTART
    putI32(d, nsavc);                                 // [16] NSAVC
    putI32(d, 0);                                     // [20] NAMNF
    while (d.size() < 84) {                           // [24..83] 保留
        d.push_back(0);
    }
    const char* title = "MolecuLens test";
    const int32_t tl = static_cast<int32_t>(std::strlen(title));
    putI32(d, tl);
    d.insert(d.end(), title, title + tl);
    putI32(d, tl);
    // 原子数记录：len(4) + natom(4) + len(4)
    putI32(d, 4);
    putI32(d, static_cast<int32_t>(natom));
    putI32(d, 4);
    // 可选 DELTA：len(8) + double + len(8)
    if (delta > 0.0) {
        putI32(d, 8);
        putF64(d, delta);
        putI32(d, 8);
    }
    for (uint32_t f = 0; f < nframes; ++f) {
        for (int axis = 0; axis < 3; ++axis) {
            const int32_t bytes = static_cast<int32_t>(natom * 4u);
            putI32(d, bytes);
            for (uint32_t i = 0; i < natom; ++i) {
                putF32(d, static_cast<float>(f) * 100.0f +
                              static_cast<float>(axis) * 10.0f +
                              static_cast<float>(i));
            }
            putI32(d, bytes);
        }
    }
    return d;
}

void writeFile(const std::filesystem::path& p, const std::vector<uint8_t>& bytes) {
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
}

} // namespace

TEST_CASE("DCD reader reports atoms, frames and dt", "[dcd][trajectory]") {
    const auto path = std::filesystem::temp_directory_path() / "moleculens_test.dcd";
    writeFile(path, makeDcd(/*natom=*/3, /*nframes=*/4, /*nsavc=*/50, /*delta=*/0.002));

    DcdReader rd;
    REQUIRE(rd.canOpen(path));
    REQUIRE(rd.open(path).has_value());
    REQUIRE(rd.atomCount() == 3u);
    REQUIRE(rd.frameCount() == 4u);
    // 帧间隔 = NSAVC × DELTA = 50 × 0.002 = 0.1 ps
    REQUIRE(rd.dt() == Catch::Approx(0.1f).epsilon(1e-4));
    rd.close();
    std::filesystem::remove(path);
}

TEST_CASE("DCD reader returns per-frame coordinates", "[dcd][trajectory]") {
    const auto path = std::filesystem::temp_directory_path() / "moleculens_coords.dcd";
    writeFile(path, makeDcd(3, 4, 50, 0.002));

    DcdReader rd;
    REQUIRE(rd.open(path).has_value());
    for (uint64_t f = 0; f < 4; ++f) {
        auto frame = rd.readFrame(f);
        REQUIRE(frame.has_value());
        REQUIRE(frame->positions.size() == 3u);
        for (uint32_t i = 0; i < 3; ++i) {
            REQUIRE(frame->positions[i].x ==
                    Catch::Approx(static_cast<float>(f) * 100.0f + static_cast<float>(i)));
            REQUIRE(frame->positions[i].y ==
                    Catch::Approx(static_cast<float>(f) * 100.0f + 10.0f +
                                  static_cast<float>(i)));
            REQUIRE(frame->positions[i].z ==
                    Catch::Approx(static_cast<float>(f) * 100.0f + 20.0f +
                                  static_cast<float>(i)));
        }
    }
    // 越界访问必须报错而不是读越界内存
    REQUIRE_FALSE(rd.readFrame(4).has_value());
    rd.close();
    std::filesystem::remove(path);
}

TEST_CASE("DCD reader sequential and random access agree", "[dcd][trajectory]") {
    const auto path = std::filesystem::temp_directory_path() / "moleculens_seq.dcd";
    writeFile(path, makeDcd(5, 3, 10, 0.0));   // 无 DELTA

    DcdReader seq;
    REQUIRE(seq.open(path).has_value());
    std::vector<float> viaNext;
    for (uint64_t i = 0; i < 3; ++i) {
        auto f = seq.nextFrame();
        REQUIRE(f.has_value());
        viaNext.push_back(f->positions[0].x);
    }

    DcdReader rnd;
    REQUIRE(rnd.open(path).has_value());
    for (uint64_t i = 0; i < 3; ++i) {
        auto f = rnd.readFrame(i);
        REQUIRE(f.has_value());
        REQUIRE(f->positions[0].x == Catch::Approx(viaNext[static_cast<size_t>(i)]));
    }
    seq.close();
    rnd.close();
    std::filesystem::remove(path);
}

TEST_CASE("DCD reader rejects non-DCD input", "[dcd][trajectory]") {
    const auto path = std::filesystem::temp_directory_path() / "moleculens_bad.dcd";
    std::vector<uint8_t> junk(200, 0x42);
    writeFile(path, junk);

    DcdReader rd;
    REQUIRE(rd.open(path).has_value() == false);
    REQUIRE_FALSE(rd.open(path).has_value());

    // 速度文件（VELD）应给出可读的错误而非当成坐标读
    auto veld = makeDcd(2, 1, 1, 0.0);
    veld[4] = 'V';
    veld[5] = 'E';
    veld[6] = 'L';
    veld[7] = 'D';
    writeFile(path, veld);
    DcdReader rd2;
    auto r = rd2.open(path);
    REQUIRE_FALSE(r.has_value());
    std::filesystem::remove(path);
}
