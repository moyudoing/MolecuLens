// =============================================================================
// Moleculens · tests · entity/trajectory/trr_reader_tests.cpp —— TRR 读取器
//
// TRR 没有现成的小夹具，这里**自己按规范写字节**，因此期望值独立于被测实现：
//   · 经典布局（version 1）：14×int32 + 2×float32（64 字节头）
//   · 新布局（version 13，GROMACS 2023+）：magic/version/hdr_size +
//     12 字节 "GMX_trn_file" + 13×int32 + 2×float32（84 字节头）
// 两种布局都写 2 原子 × 2 帧，坐标按 (1,2,3) nm 与 (4,5,6) nm 摆放，
// 期望值 = 坐标 × 10 Å；盒子 4.0 nm 立方 → 40 Å。
// 另有一条可选的"真实文件"用例：仓库里若存在 data/md.trr 或用户算例路径，
// 就校验 6452 原子 / 1001 帧 / 10 ps；缺失则 SKIP（不阻断 CI）。
// =============================================================================

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include "entity/trajectory/trr/trr_reader.h"

using namespace moleculens::entity;

namespace {

void putBE32(std::vector<uint8_t>& out, int32_t v) {
    const auto u = static_cast<uint32_t>(v);
    out.push_back(static_cast<uint8_t>((u >> 24) & 0xFF));
    out.push_back(static_cast<uint8_t>((u >> 16) & 0xFF));
    out.push_back(static_cast<uint8_t>((u >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>(u & 0xFF));
}

void putBEF32(std::vector<uint8_t>& out, float v) {
    int32_t bits = 0;
    std::memcpy(&bits, &v, sizeof(bits));
    putBE32(out, bits);
}

// 一帧的负载：9 个 float 盒子（nm）+ 2 原子 × 3 坐标（nm）
void putFramePayload(std::vector<uint8_t>& out, float t, const float xyz[6]) {
    (void)t;
    const float box[9] = {4.0f, 0.0f, 0.0f, 0.0f, 4.0f, 0.0f, 0.0f, 0.0f, 4.0f};
    for (const float b : box) {
        putBEF32(out, b);
    }
    for (int i = 0; i < 6; ++i) {
        putBEF32(out, xyz[i]);
    }
}

std::filesystem::path tempFile(const char* name) {
    auto p = std::filesystem::temp_directory_path() / name;
    std::filesystem::remove(p);
    return p;
}

// 写一份两帧的 TRR 文件；modern=true → version 13 布局，否则经典 v1
std::filesystem::path writeFixture(const char* name, bool modern) {
    const auto path = tempFile(name);
    std::vector<uint8_t> bytes;
    const float frames[2][6] = {{1.0f, 2.0f, 3.0f, 1.5f, 2.5f, 3.5f},
                                {4.0f, 5.0f, 6.0f, 4.5f, 5.5f, 6.5f}};
    const float times[2] = {1.0f, 2.5f};

    for (int f = 0; f < 2; ++f) {
        putBE32(bytes, 1993);                     // magic
        if (modern) {
            putBE32(bytes, 13);                   // version
            putBE32(bytes, 12);                   // hdr_size
            static const char kMarker[12] = {'G', 'M', 'X', '_', 't', 'r', 'n',
                                             '_', 'f', 'i', 'l', 'e'};
            bytes.insert(bytes.end(), kMarker, kMarker + 12);
            putBE32(bytes, 0);                    // ir
            putBE32(bytes, 0);                    // e
            putBE32(bytes, 36);                   // box（9 × float32）
            putBE32(bytes, 0);                    // vir
            putBE32(bytes, 0);                    // pres
            putBE32(bytes, 0);                    // top
            putBE32(bytes, 0);                    // 保留
            putBE32(bytes, 2 * 3 * 4);            // x（2 原子 × 3 × float）
            putBE32(bytes, 0);                    // v
            putBE32(bytes, 0);                    // f
            putBE32(bytes, 2);                    // natoms
            putBE32(bytes, f);                    // step
            putBE32(bytes, 0);                    // nre
            putBEF32(bytes, times[f]);            // t
            putBEF32(bytes, 0.0f);                // lambda
        } else {
            putBE32(bytes, 1);                    // version
            putBE32(bytes, 0);                    // ir
            putBE32(bytes, 0);                    // e
            putBE32(bytes, 36);                   // box
            putBE32(bytes, 0);                    // vir
            putBE32(bytes, 0);                    // pres
            putBE32(bytes, 0);                    // top
            putBE32(bytes, 2 * 3 * 4);            // x
            putBE32(bytes, 0);                    // v
            putBE32(bytes, 0);                    // f
            putBE32(bytes, 2);                    // natoms
            putBE32(bytes, f);                    // step
            putBE32(bytes, 0);                    // nre
            putBEF32(bytes, times[f]);            // t
            putBEF32(bytes, 0.0f);                // lambda
        }
        putFramePayload(bytes, times[f], frames[f]);
    }

    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
    out.close();
    return path;
}

void checkFixture(const std::filesystem::path& path) {
    TrrReader r;
    REQUIRE(r.canOpen(path));
    auto opened = r.open(path);
    REQUIRE(opened.has_value());
    REQUIRE(r.atomCount() == 2);
    REQUIRE(r.frameCount() == 2);
    REQUIRE(r.dt() == Catch::Approx(1.5f));

    auto f0 = r.readFrame(0);
    REQUIRE(f0.has_value());
    REQUIRE(f0->positions.size() == 2);
    // nm → Å：期望 (10,20,30) 与 (15,25,35)
    REQUIRE(f0->positions[0].x == Catch::Approx(10.0f));
    REQUIRE(f0->positions[0].y == Catch::Approx(20.0f));
    REQUIRE(f0->positions[0].z == Catch::Approx(30.0f));
    REQUIRE(f0->positions[1].x == Catch::Approx(15.0f));
    REQUIRE(f0->positions[1].y == Catch::Approx(25.0f));
    REQUIRE(f0->positions[1].z == Catch::Approx(35.0f));
    // 盒子 4 nm 立方 → 40 Å 对角
    REQUIRE(f0->box[0] == Catch::Approx(40.0f));
    REQUIRE(f0->box[4] == Catch::Approx(40.0f));
    REQUIRE(f0->box[8] == Catch::Approx(40.0f));

    auto f1 = r.readFrame(1);
    REQUIRE(f1.has_value());
    REQUIRE(f1->positions[0].x == Catch::Approx(40.0f));
    REQUIRE(f1->positions[1].z == Catch::Approx(65.0f));

    // nextFrame 顺序推进；越界要报错而不是崩溃
    REQUIRE(r.nextFrame().has_value());
    REQUIRE(r.nextFrame().has_value());
    REQUIRE_FALSE(r.nextFrame().has_value());
    r.close();
}

std::filesystem::path realTrr() {
    const char* candidates[] = {
        "F:/MolecuLens/data/md.trr",
        "H:/02_科研_论文/05_分子模拟/md/sim/md.trr",
    };
    for (const char* c : candidates) {
        const std::filesystem::path p = std::filesystem::u8path(c);
        if (std::filesystem::exists(p)) {
            return p;
        }
    }
    return {};
}

} // namespace

TEST_CASE("TrrReader reads legacy layout (version 1)", "[trr]") {
    const auto path = writeFixture("moleculens_trr_legacy.trr", false);
    checkFixture(path);
    std::filesystem::remove(path);
}

TEST_CASE("TrrReader reads GROMACS 2023+ layout (version 13)", "[trr]") {
    const auto path = writeFixture("moleculens_trr_v13.trr", true);
    checkFixture(path);
    std::filesystem::remove(path);
}

TEST_CASE("TrrReader rejects non-TRR files", "[trr]") {
    const auto path = tempFile("moleculens_trr_bad.trr");
    {
        std::ofstream out(path, std::ios::binary);
        out << "not a trr file at all";
    }
    TrrReader r;
    // 扩展名对但 magic 错：canOpen 失败
    REQUIRE_FALSE(r.canOpen(path));
    std::filesystem::remove(path);
}

TEST_CASE("TrrReader reads real production trajectory (optional fixture)", "[trr]") {
    const auto path = realTrr();
    if (path.empty()) {
        SKIP("未找到 md.trr（data/ 或用户算例路径），跳过真实文件校验");
    }
    TrrReader r;
    REQUIRE(r.canOpen(path));
    REQUIRE(r.open(path).has_value());
    REQUIRE(r.atomCount() == 6452);
    REQUIRE(r.frameCount() == 1001);
    REQUIRE(r.dt() == Catch::Approx(10.0f));

    // 首帧 t = 300 ps，末帧 t = 300 + 1000×10 = 10 300 ps（用帧数×步长间接核对）
    auto last = r.readFrame(1000);
    REQUIRE(last.has_value());
    REQUIRE(last->positions.size() == 6452);
    for (const auto& p : last->positions) {
        REQUIRE(std::isfinite(p.x));
        REQUIRE(std::isfinite(p.y));
        REQUIRE(std::isfinite(p.z));
    }
    r.close();
}
