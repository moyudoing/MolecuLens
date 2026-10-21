// =============================================================================
// MolecuLens · tests · io/cube_tests.cpp —— Gaussian cube 解析 / 三线性采样 / 场景导入
// 覆盖：
//   1. 头部（dims / 原点 / 网格向量）Bohr→Å 换算 + axesInverse 正确性；
//   2. 数据区读取（每行多个值、科学计数法、负值）与 minValue/maxValue；
//   3. sampleTrilinear：体素命中、中点插值、非正交网格、出界（out 不变）；
//   4. CubeFileReader：解析器注册表识别、SceneStructure 原子/键/坐标、体积警告；
//   5. 异常路径：文件不存在/头部残缺/网格非法/数据个数不符/非数字 token/多数据组；
//   6. 种类启发式（标题关键词 / 数值特征 / 关键词冲突回退）；
//   7. 自带 _esp/demo 合成 cube（存在时）真实大文件回归。
// 注意：测试文件写入系统临时目录（每个用例唯一文件名，避免并行互相覆盖）。
// =============================================================================

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

#include <glm/glm.hpp>

#include "io/cube/cube_parser.h"
#include "io/cube/cube_volume.h"
#include "io/registry/parser_registry.h"

using namespace moleculens;
using namespace moleculens::entity;

namespace {

constexpr float kBohr = 0.529177210903f;   // Bohr → Å（与解析器一致）

struct AtomRow {
    int         atomicNumber = 0;
    double      nuclearCharge = 0.0;
    glm::dvec3  positionBohr{0.0};
};

// cube 测试样本（默认值 = 3x3x3 + 2 原子 + 线性数据 0, 0.5, …, 13）
struct CubeSpec {
    std::string title = "MolecuLens cube unit test";
    std::string comment = "synthetic scalar field";
    int atomCountField = 0;   // 可为负（表示原子行之后有 DSET_IDS）
    glm::dvec3 originBohr{0.0};
    std::array<glm::dvec3, 3> axesBohr{
        glm::dvec3(2.0, 0.0, 0.0),
        glm::dvec3(0.0, 3.0, 0.0),
        glm::dvec3(0.0, 0.0, 1.5),
    };
    std::array<int, 3> dims{3, 3, 3};
    std::vector<AtomRow> atoms;
    std::vector<int>     dataSetIds;      // 非空 → 原子行之后写 DSET_IDS 行
    std::vector<double>  data;
    int valuesPerLine = 6;
    int valuesPerVoxelField = 0;          // >0 → 第 3 行追加 nval 字段
};

std::string buildCubeText(const CubeSpec& spec) {
    std::ostringstream out;
    out << spec.title << '\n' << spec.comment << '\n';

    char line[512] = {};
    if (spec.valuesPerVoxelField > 0) {
        std::snprintf(line, sizeof(line), "%5d %12.6f %12.6f %12.6f %5d\n",
                      spec.atomCountField, spec.originBohr.x, spec.originBohr.y,
                      spec.originBohr.z, spec.valuesPerVoxelField);
    } else {
        std::snprintf(line, sizeof(line), "%5d %12.6f %12.6f %12.6f\n",
                      spec.atomCountField, spec.originBohr.x, spec.originBohr.y,
                      spec.originBohr.z);
    }
    out << line;

    for (int axis = 0; axis < 3; ++axis) {
        std::snprintf(line, sizeof(line), "%5d %12.6f %12.6f %12.6f\n",
                      spec.dims[axis], spec.axesBohr[axis].x, spec.axesBohr[axis].y,
                      spec.axesBohr[axis].z);
        out << line;
    }
    for (const AtomRow& atom : spec.atoms) {
        std::snprintf(line, sizeof(line), "%5d %12.6f %12.6f %12.6f %12.6f\n",
                      atom.atomicNumber, atom.nuclearCharge, atom.positionBohr.x,
                      atom.positionBohr.y, atom.positionBohr.z);
        out << line;
    }
    if (!spec.dataSetIds.empty()) {
        out << spec.dataSetIds.size();
        for (const int id : spec.dataSetIds) {
            out << ' ' << id;
        }
        out << '\n';
    }
    const std::size_t perLine =
        spec.valuesPerLine > 0 ? static_cast<std::size_t>(spec.valuesPerLine) : 6u;
    for (std::size_t index = 0; index < spec.data.size(); ++index) {
        std::snprintf(line, sizeof(line), "%13.5E", spec.data[index]);
        out << line;
        if ((index + 1) % perLine == 0 || index + 1 == spec.data.size()) {
            out << '\n';
        } else {
            out << ' ';
        }
    }
    return out.str();
}

CubeSpec makeStandardSpec() {
    CubeSpec spec;
    spec.title = "MolecuLens cube IO unit test";
    spec.comment = "electron density (synthetic)";   // 关键词 → ElectronDensity
    spec.atomCountField = 2;
    spec.originBohr = glm::dvec3(1.0, -0.5, 2.0);
    spec.atoms = {
        AtomRow{8, 8.0, glm::dvec3(0.5, 0.25, -0.75)},     // O
        AtomRow{1, 1.0, glm::dvec3(-1.0, 0.5, 0.25)},      // H
    };
    spec.data.resize(27);
    for (int index = 0; index < 27; ++index) {
        spec.data[static_cast<std::size_t>(index)] = 0.5 * static_cast<double>(index);
    }
    return spec;
}

std::filesystem::path writeCubeFile(const std::string& name, const std::string& text) {
    const std::filesystem::path path = std::filesystem::temp_directory_path() / name;
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    REQUIRE(out);
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    out.close();
    return path;
}

// _esp/demo 目录（源码树相对路径；找不到则返回空 → 用例跳过）
std::filesystem::path findDemoCubeDir() {
    std::vector<std::filesystem::path> candidates;
    const std::filesystem::path sourceFile(__FILE__);
    if (sourceFile.has_parent_path()) {
        candidates.push_back(sourceFile.parent_path() / ".." / ".." / "_esp" / "demo");
    }
    const std::filesystem::path cwd = std::filesystem::current_path();
    candidates.push_back(cwd / ".." / "_esp" / "demo");
    candidates.push_back(cwd / "_esp" / "demo");
    candidates.push_back(cwd / ".." / ".." / "_esp" / "demo");
    for (const std::filesystem::path& candidate : candidates) {
        std::error_code code;
        if (std::filesystem::is_directory(candidate, code)) {
            return candidate.lexically_normal();
        }
    }
    return {};
}

} // namespace

// =============================================================================
// 1. 头部 / 单位换算 / 逆矩阵
// =============================================================================
TEST_CASE("Cube parser converts Bohr to Angstrom and fills grid metadata", "[io][cube]") {
    const CubeSpec spec = makeStandardSpec();
    const std::filesystem::path path =
        writeCubeFile("moleculens_cube_meta.cube", buildCubeText(spec));

    CubeVolume volume;
    std::string error;
    REQUIRE(parseCubeFile(path.string(), volume, &error));
    REQUIRE(error.empty());

    REQUIRE(volume.dims[0] == 3);
    REQUIRE(volume.dims[1] == 3);
    REQUIRE(volume.dims[2] == 3);
    REQUIRE(volume.valid());
    REQUIRE(volume.voxelCount() == 27u);
    REQUIRE(volume.data.size() == 27u);
    REQUIRE(volume.data[0] == Catch::Approx(0.0f));
    REQUIRE(volume.data[9] == Catch::Approx(4.5f));
    REQUIRE(volume.data[26] == Catch::Approx(13.0f));
    REQUIRE(volume.minValue() == Catch::Approx(0.0f));
    REQUIRE(volume.maxValue() == Catch::Approx(13.0f));
    REQUIRE(volume.kind == CubeFieldKind::ElectronDensity);

    // 原点 / 网格向量：Bohr → Å
    REQUIRE(volume.origin.x == Catch::Approx(1.0f * kBohr).margin(1e-6f));
    REQUIRE(volume.origin.y == Catch::Approx(-0.5f * kBohr).margin(1e-6f));
    REQUIRE(volume.origin.z == Catch::Approx(2.0f * kBohr).margin(1e-6f));
    REQUIRE(volume.axes[0].x == Catch::Approx(2.0f * kBohr).margin(1e-6f));
    REQUIRE(volume.axes[0].y == Catch::Approx(0.0f));
    REQUIRE(volume.axes[1].y == Catch::Approx(3.0f * kBohr).margin(1e-6f));
    REQUIRE(volume.axes[2].z == Catch::Approx(1.5f * kBohr).margin(1e-6f));

    // voxelPosition = origin + ix*ax0 + iy*ax1 + iz*ax2
    const glm::vec3 corner = volume.voxelPosition(2, 1, 2);
    REQUIRE(corner.x == Catch::Approx(volume.origin.x + 2.0f * volume.axes[0].x).margin(1e-6f));
    REQUIRE(corner.y == Catch::Approx(volume.origin.y + 1.0f * volume.axes[1].y).margin(1e-6f));
    REQUIRE(corner.z == Catch::Approx(volume.origin.z + 2.0f * volume.axes[2].z).margin(1e-6f));

    // axesInverse：把网格向量映射回单位坐标轴
    const glm::dvec3 unit0 = volume.axesInverse * glm::dvec3(volume.axes[0]);
    const glm::dvec3 unit1 = volume.axesInverse * glm::dvec3(volume.axes[1]);
    const glm::dvec3 unit2 = volume.axesInverse * glm::dvec3(volume.axes[2]);
    REQUIRE(unit0.x == Catch::Approx(1.0).margin(1e-9));
    REQUIRE(unit0.y == Catch::Approx(0.0).margin(1e-9));
    REQUIRE(unit0.z == Catch::Approx(0.0).margin(1e-9));
    REQUIRE(unit1.x == Catch::Approx(0.0).margin(1e-9));
    REQUIRE(unit1.y == Catch::Approx(1.0).margin(1e-9));
    REQUIRE(unit1.z == Catch::Approx(0.0).margin(1e-9));
    REQUIRE(unit2.x == Catch::Approx(0.0).margin(1e-9));
    REQUIRE(unit2.y == Catch::Approx(0.0).margin(1e-9));
    REQUIRE(unit2.z == Catch::Approx(1.0).margin(1e-9));

    // 原子记录（Bohr → Å；核电荷不换算）
    REQUIRE(volume.atoms.size() == 2u);
    REQUIRE(volume.atoms[0].atomicNumber == 8);
    REQUIRE(volume.atoms[0].nuclearCharge == Catch::Approx(8.0f));
    REQUIRE(volume.atoms[0].position.x == Catch::Approx(0.5f * kBohr).margin(1e-6f));
    REQUIRE(volume.atoms[0].position.y == Catch::Approx(0.25f * kBohr).margin(1e-6f));
    REQUIRE(volume.atoms[0].position.z == Catch::Approx(-0.75f * kBohr).margin(1e-6f));
    REQUIRE(volume.atoms[1].atomicNumber == 1);
    REQUIRE(volume.atoms[1].position.x == Catch::Approx(-1.0f * kBohr).margin(1e-6f));

    std::filesystem::remove(path);
}

// =============================================================================
// 2. 三线性采样（正交网格）：命中 / 中点 / 出界
// =============================================================================
TEST_CASE("CubeVolume::sampleTrilinear interpolates and rejects out-of-range points",
          "[io][cube]") {
    const CubeSpec spec = makeStandardSpec();
    const std::filesystem::path path =
        writeCubeFile("moleculens_cube_sample.cube", buildCubeText(spec));

    CubeVolume volume;
    std::string error;
    REQUIRE(parseCubeFile(path.string(), volume, &error));

    float value = 0.0f;
    REQUIRE(volume.sampleTrilinear(volume.voxelPosition(1, 2, 0), value));
    REQUIRE(value == Catch::Approx(7.5f).margin(1e-4f));    // data[15]
    REQUIRE(volume.sampleTrilinear(volume.voxelPosition(0, 0, 1), value));
    REQUIRE(value == Catch::Approx(0.5f).margin(1e-4f));    // data[1]
    REQUIRE(volume.sampleTrilinear(volume.voxelPosition(2, 2, 2), value));
    REQUIRE(value == Catch::Approx(13.0f).margin(1e-4f));   // data[26]

    // 沿 x 轴中点：(data[0] + data[9]) / 2 = 2.25
    const glm::vec3 midpoint = volume.origin + volume.axes[0] * 0.5f;
    REQUIRE(volume.sampleTrilinear(midpoint, value));
    REQUIRE(value == Catch::Approx(2.25f).margin(1e-3f));

    // 出界：ix = -1 / ix = 3 / 远处 → false 且 out 不变
    float untouched = 123.0f;
    REQUIRE_FALSE(volume.sampleTrilinear(volume.origin - volume.axes[0], untouched));
    REQUIRE(untouched == 123.0f);
    REQUIRE_FALSE(volume.sampleTrilinear(volume.voxelPosition(2, 2, 2) + volume.axes[0],
                                        untouched));
    REQUIRE(untouched == 123.0f);
    REQUIRE_FALSE(volume.sampleTrilinear(glm::vec3(1000.0f), untouched));
    REQUIRE(untouched == 123.0f);

    // 未解析（无效）体积 → false
    const CubeVolume empty;
    REQUIRE_FALSE(empty.sampleTrilinear(glm::vec3(0.0f), untouched));
    REQUIRE(empty.minValue() == Catch::Approx(0.0f));
    REQUIRE(empty.maxValue() == Catch::Approx(0.0f));

    std::filesystem::remove(path);
}

// =============================================================================
// 3. 三线性采样（非正交网格 → 必须依赖 axesInverse）
// =============================================================================
TEST_CASE("CubeVolume::sampleTrilinear handles non-orthogonal grid axes", "[io][cube]") {
    CubeSpec spec = makeStandardSpec();
    spec.axesBohr = {
        glm::dvec3(2.0, 0.0, 0.0),
        glm::dvec3(0.5, 3.0, 0.0),
        glm::dvec3(0.25, 0.5, 1.5),
    };
    const std::filesystem::path path =
        writeCubeFile("moleculens_cube_skew.cube", buildCubeText(spec));

    CubeVolume volume;
    std::string error;
    REQUIRE(parseCubeFile(path.string(), volume, &error));

    // 体素点必须精确命中（否则 axesInverse 有误）
    float value = 0.0f;
    REQUIRE(volume.sampleTrilinear(volume.voxelPosition(1, 2, 1), value));
    REQUIRE(value == Catch::Approx(8.0f).margin(1e-2f));    // data[16]
    REQUIRE(volume.sampleTrilinear(volume.voxelPosition(2, 0, 2), value));
    REQUIRE(value == Catch::Approx(10.0f).margin(1e-2f));   // data[20]

    // 分数坐标 (1.5, 0.5, 2)：z 落在最大层（clamp 后权重 1）→ 期望 8.5
    const glm::vec3 fractional = volume.origin + volume.axes[0] * 1.5f +
                                 volume.axes[1] * 0.5f + volume.axes[2] * 2.0f;
    REQUIRE(volume.sampleTrilinear(fractional, value));
    REQUIRE(value == Catch::Approx(8.5f).margin(1e-2f));

    std::filesystem::remove(path);
}

// =============================================================================
// 4. CubeFileReader：注册表识别 + SceneStructure（原子 / 键 / 坐标 / 警告）
// =============================================================================
TEST_CASE("CubeFileReader builds a bonded SceneStructure from cube atoms",
          "[io][cube][registry]") {
    const CubeSpec spec = makeStandardSpec();
    const std::filesystem::path path =
        writeCubeFile("moleculens_cube_scene.cube", buildCubeText(spec));

    CubeFileReader reader;
    REQUIRE(std::string(reader.name()) == "cube");
    const std::vector<std::string> extensions = reader.extensions();
    REQUIRE(std::find(extensions.begin(), extensions.end(), "cube") != extensions.end());
    REQUIRE(std::find(extensions.begin(), extensions.end(), "cub") != extensions.end());
    REQUIRE(reader.canOpen(path));

    ParserRegistry& registry = ParserRegistry::defaultRegistry();
    IFileReader* found = registry.findForPath(path);
    REQUIRE(found != nullptr);
    REQUIRE(std::string(found->name()) == "cube");

    auto parsed = registry.parseFile(path, {});
    REQUIRE(static_cast<bool>(parsed));
    const ParseResult& result = *parsed;
    REQUIRE(result.scene != nullptr);
    REQUIRE(result.scene->instances().size() == 1u);
    REQUIRE(result.scene->structures().size() == 1u);
    // 体积数据提示警告必须存在（避免用户误以为场景里带了标量场）
    REQUIRE_FALSE(result.warnings.empty());

    const SceneStructure& scene = *result.scene;
    const MoleculeStructure& structure = scene.structures().front();
    REQUIRE(structure.elements.size() == 2u);
    REQUIRE(structure.elements[0] == ElementType::Oxygen);
    REQUIRE(structure.elements[1] == ElementType::Hydrogen);
    REQUIRE(structure.atoms.size() == 2u);
    REQUIRE(structure.atoms[0].chainId >= 0);
    REQUIRE(structure.atoms[0].residueId >= 0);
    // O–H 距离 ≈ 0.96 Å（< 共价半径和 + 容差）→ 应推断出 1 根键
    REQUIRE(structure.bonds.size() == 1u);
    REQUIRE(structure.bonds[0].a == 0u);
    REQUIRE(structure.bonds[0].b == 1u);

    // 场景坐标 = cube 头部原子坐标（Å）
    REQUIRE(scene.coordsSoA().positions.size() == 2u);
    REQUIRE(scene.coordsSoA().positions[0].x ==
            Catch::Approx(0.5f * kBohr).margin(1e-5f));
    REQUIRE(scene.coordsSoA().positions[1].x ==
            Catch::Approx(-1.0f * kBohr).margin(1e-5f));

    // 单帧轨迹
    REQUIRE(scene.instances().front().trajectory != nullptr);
    REQUIRE(scene.instances().front().trajectory->frames.size() == 1u);
    REQUIRE(scene.instances().front().trajectory->frames[0].positions.size() == 2u);

    std::filesystem::remove(path);
}

// =============================================================================
// 5. 无扩展名 / 非 .cube 后缀：第 3 行数字特征嗅探（canOpen）
// =============================================================================
TEST_CASE("CubeFileReader sniffs cube content by its header when extension is unknown",
          "[io][cube][registry]") {
    const CubeSpec spec = makeStandardSpec();
    const std::filesystem::path path =
        writeCubeFile("moleculens_cube_sniff.dat", buildCubeText(spec));

    CubeFileReader reader;
    REQUIRE(reader.canOpen(path));

    ParserRegistry& registry = ParserRegistry::defaultRegistry();
    IFileReader* found = registry.findForPath(path);
    REQUIRE(found != nullptr);
    REQUIRE(std::string(found->name()) == "cube");

    auto parsed = registry.parseFile(path, {});
    REQUIRE(static_cast<bool>(parsed));
    REQUIRE(parsed->scene->structures().front().elements.size() == 2u);

    std::filesystem::remove(path);
}

// =============================================================================
// 6. 异常路径：必须返回 false + error，且 out 保持原样
// =============================================================================
TEST_CASE("Cube parser rejects malformed files with an error message", "[io][cube]") {
    std::string error;

    // 6.1 文件不存在
    {
        const std::filesystem::path path =
            std::filesystem::temp_directory_path() / "moleculens_cube_missing_xyz.cube";
        std::error_code code;
        std::filesystem::remove(path, code);
        CubeVolume volume;
        REQUIRE_FALSE(parseCubeFile(path.string(), volume, &error));
        REQUIRE_FALSE(error.empty());
        REQUIRE(volume.dims[0] == 0);
    }

    // 6.2 行数不足（只有 2 行注释）
    {
        const std::filesystem::path path = writeCubeFile(
            "moleculens_cube_bad_short.cube", "title only\ncomment only\n");
        CubeVolume volume;
        volume.dims = {7, 7, 7};   // 哨兵：失败时不得被改写
        error.clear();
        REQUIRE_FALSE(parseCubeFile(path.string(), volume, &error));
        REQUIRE_FALSE(error.empty());
        REQUIRE(volume.dims[0] == 7);
        std::filesystem::remove(path);
    }

    // 6.3 第 3 行不是 "原子数 + 原点"
    {
        const std::filesystem::path path = writeCubeFile(
            "moleculens_cube_bad_line3.cube",
            "title\ncomment\nnot-a-number 0.0 0.0 0.0\n");
        CubeVolume volume;
        error.clear();
        REQUIRE_FALSE(parseCubeFile(path.string(), volume, &error));
        REQUIRE_FALSE(error.empty());
        std::filesystem::remove(path);
    }

    // 6.4 网格定义行不足（只给 1 行）
    {
        const std::filesystem::path path = writeCubeFile(
            "moleculens_cube_bad_grid.cube",
            "title\ncomment\n1 0.0 0.0 0.0\n2 1.0 0.0 0.0\n");
        CubeVolume volume;
        error.clear();
        REQUIRE_FALSE(parseCubeFile(path.string(), volume, &error));
        REQUIRE_FALSE(error.empty());
        std::filesystem::remove(path);
    }

    // 6.5 网格点数为 0
    {
        CubeSpec spec = makeStandardSpec();
        spec.dims = {0, 3, 3};
        const std::filesystem::path path =
            writeCubeFile("moleculens_cube_bad_dims.cube", buildCubeText(spec));
        CubeVolume volume;
        error.clear();
        REQUIRE_FALSE(parseCubeFile(path.string(), volume, &error));
        REQUIRE_FALSE(error.empty());
        std::filesystem::remove(path);
    }

    // 6.6 数据个数不足（少 1 个值）
    {
        CubeSpec spec = makeStandardSpec();
        spec.data.resize(26);
        const std::filesystem::path path =
            writeCubeFile("moleculens_cube_bad_truncated.cube", buildCubeText(spec));
        CubeVolume volume;
        error.clear();
        REQUIRE_FALSE(parseCubeFile(path.string(), volume, &error));
        REQUIRE_FALSE(error.empty());
        std::filesystem::remove(path);
    }

    // 6.7 数据个数过多（多 1 个值）
    {
        CubeSpec spec = makeStandardSpec();
        spec.data.push_back(99.0);
        const std::filesystem::path path =
            writeCubeFile("moleculens_cube_bad_extra.cube", buildCubeText(spec));
        CubeVolume volume;
        error.clear();
        REQUIRE_FALSE(parseCubeFile(path.string(), volume, &error));
        REQUIRE_FALSE(error.empty());
        std::filesystem::remove(path);
    }

    // 6.8 数据区含非数字 token
    {
        const std::filesystem::path path = writeCubeFile(
            "moleculens_cube_bad_token.cube",
            "title\ncomment\n"
            "1 0.0 0.0 0.0\n"
            "2 1.0 0.0 0.0\n2 0.0 1.0 0.0\n2 0.0 0.0 1.0\n"
            "1 1.0 0.0 0.0 0.0\n"
            "0.0 0.1 abc 0.3 0.4 0.5\n0.6 0.7\n");
        CubeVolume volume;
        error.clear();
        REQUIRE_FALSE(parseCubeFile(path.string(), volume, &error));
        REQUIRE_FALSE(error.empty());
        std::filesystem::remove(path);
    }

    // 6.9 网格向量退化（行列式 0）
    {
        CubeSpec spec = makeStandardSpec();
        spec.axesBohr = {glm::dvec3(0.0), glm::dvec3(0.0), glm::dvec3(0.0)};
        const std::filesystem::path path =
            writeCubeFile("moleculens_cube_bad_axes.cube", buildCubeText(spec));
        CubeVolume volume;
        error.clear();
        REQUIRE_FALSE(parseCubeFile(path.string(), volume, &error));
        REQUIRE_FALSE(error.empty());
        std::filesystem::remove(path);
    }

    // 6.10 负原子数但缺少 DSET_IDS 记录
    {
        CubeSpec spec = makeStandardSpec();
        spec.atomCountField = -2;
        spec.dataSetIds.clear();
        const std::filesystem::path path =
            writeCubeFile("moleculens_cube_bad_dset.cube", buildCubeText(spec));
        CubeVolume volume;
        error.clear();
        REQUIRE_FALSE(parseCubeFile(path.string(), volume, &error));
        REQUIRE_FALSE(error.empty());
        std::filesystem::remove(path);
    }

    // 6.11 多数据组（CubeVolume 只支持单体积）
    {
        CubeSpec spec = makeStandardSpec();
        spec.atomCountField = -2;
        spec.dataSetIds = {1, 2};
        spec.data.resize(54, 0.25);
        const std::filesystem::path path =
            writeCubeFile("moleculens_cube_bad_multi.cube", buildCubeText(spec));
        CubeVolume volume;
        error.clear();
        REQUIRE_FALSE(parseCubeFile(path.string(), volume, &error));
        REQUIRE_FALSE(error.empty());
        std::filesystem::remove(path);
    }

    // 6.12 每体素多值（nval > 1）
    {
        CubeSpec spec = makeStandardSpec();
        spec.valuesPerVoxelField = 2;
        const std::filesystem::path path =
            writeCubeFile("moleculens_cube_bad_nval.cube", buildCubeText(spec));
        CubeVolume volume;
        error.clear();
        REQUIRE_FALSE(parseCubeFile(path.string(), volume, &error));
        REQUIRE_FALSE(error.empty());
        std::filesystem::remove(path);
    }
}

// =============================================================================
// 7. 负原子数 + DSET_IDS（单数据组）：应正常解析（跳过数据组编号）
// =============================================================================
TEST_CASE("Cube parser skips a single-data-set DSET_IDS record", "[io][cube]") {
    CubeSpec spec = makeStandardSpec();
    spec.atomCountField = -2;
    spec.comment = "electron density (synthetic, single data set)";
    spec.dataSetIds = {6};
    const std::filesystem::path path =
        writeCubeFile("moleculens_cube_dset.cube", buildCubeText(spec));

    CubeVolume volume;
    std::string error;
    REQUIRE(parseCubeFile(path.string(), volume, &error));
    REQUIRE(error.empty());
    REQUIRE(volume.atoms.size() == 2u);
    REQUIRE(volume.data.size() == 27u);
    REQUIRE(volume.data[0] == Catch::Approx(0.0f));
    REQUIRE(volume.data[26] == Catch::Approx(13.0f));

    std::filesystem::remove(path);
}

// =============================================================================
// 8. 种类启发式：关键词 / 数值特征 / 关键词冲突回退
// =============================================================================
TEST_CASE("Cube parser infers the field kind from header or values", "[io][cube]") {
    std::string error;

    // 8.1 标题含 potential → ESP（即使数据全非负也以标题为准）
    {
        CubeSpec spec = makeStandardSpec();
        spec.title = "Electrostatic potential map";
        spec.comment = "synthetic";
        const std::filesystem::path path =
            writeCubeFile("moleculens_cube_kind_esp.cube", buildCubeText(spec));
        CubeVolume volume;
        REQUIRE(parseCubeFile(path.string(), volume, &error));
        REQUIRE(volume.kind == CubeFieldKind::ElectrostaticPotential);
        std::filesystem::remove(path);
    }

    // 8.2 标题同时含 density 与 ESP（如 _esp/demo 密度 cube）→ 数值特征：非负 → 密度
    {
        CubeSpec spec = makeStandardSpec();
        spec.title = "Synthetic electron density for MolecuLens ESP-surface demo";
        spec.comment = "density in a.u. (synthetic)";
        const std::filesystem::path path =
            writeCubeFile("moleculens_cube_kind_conflict.cube", buildCubeText(spec));
        CubeVolume volume;
        REQUIRE(parseCubeFile(path.string(), volume, &error));
        REQUIRE(volume.kind == CubeFieldKind::ElectronDensity);
        std::filesystem::remove(path);
    }

    // 8.3 无关键词 + 数据同时有正负 → ESP
    {
        CubeSpec spec = makeStandardSpec();
        spec.title = "MolecuLens scalar field";
        spec.comment = "synthetic scalar field";
        for (int index = 0; index < 27; ++index) {
            spec.data[static_cast<std::size_t>(index)] =
                0.5 * static_cast<double>(index) - 6.0;
        }
        const std::filesystem::path path =
            writeCubeFile("moleculens_cube_kind_signed.cube", buildCubeText(spec));
        CubeVolume volume;
        REQUIRE(parseCubeFile(path.string(), volume, &error));
        REQUIRE(volume.kind == CubeFieldKind::ElectrostaticPotential);
        std::filesystem::remove(path);
    }

    // 8.4 无关键词 + 数据全非负 → 电子密度
    {
        CubeSpec spec = makeStandardSpec();
        spec.title = "MolecuLens scalar field";
        spec.comment = "synthetic scalar field";
        const std::filesystem::path path =
            writeCubeFile("moleculens_cube_kind_nonneg.cube", buildCubeText(spec));
        CubeVolume volume;
        REQUIRE(parseCubeFile(path.string(), volume, &error));
        REQUIRE(volume.kind == CubeFieldKind::ElectronDensity);
        std::filesystem::remove(path);
    }
}

// =============================================================================
// 9. 自带 _esp/demo 合成 cube（存在时）：多 MB 文本 + 真实头部回归
// =============================================================================
TEST_CASE("Cube parser reproduces the bundled ESP demo cubes", "[io][cube][demo]") {
    const std::filesystem::path demoDir = findDemoCubeDir();
    if (demoDir.empty()) {
        SUCCEED("_esp/demo not found; demo cube check skipped");
        return;
    }
    const std::filesystem::path densityPath = demoDir / "glycine_density.cube";
    const std::filesystem::path espPath = demoDir / "glycine_esp.cube";
    if (!std::filesystem::exists(densityPath) || !std::filesystem::exists(espPath)) {
        SUCCEED("demo cube files missing; demo cube check skipped");
        return;
    }

    CubeVolume density;
    std::string error;
    REQUIRE(parseCubeFile(densityPath.string(), density, &error));
    REQUIRE(error.empty());
    REQUIRE(density.dims[0] == 54);
    REQUIRE(density.dims[1] == 42);
    REQUIRE(density.dims[2] == 37);
    REQUIRE(density.data.size() == 54u * 42u * 37u);
    REQUIRE(density.atoms.size() == 10u);
    REQUIRE(density.kind == CubeFieldKind::ElectronDensity);
    REQUIRE(density.minValue() >= 0.0f);
    REQUIRE(density.maxValue() > 0.0f);
    // 网格间距 0.25 Å（文件 0.472432 Bohr）
    REQUIRE(density.axes[0].x == Catch::Approx(0.25f).margin(1e-3f));
    // 角点采样必须命中（数据索引布局 (ix*ny+iy)*nz+iz）
    float sampled = 0.0f;
    REQUIRE(density.sampleTrilinear(density.origin, sampled));
    REQUIRE(sampled == Catch::Approx(density.data[0]).margin(1e-6f));

    CubeVolume esp;
    error.clear();
    REQUIRE(parseCubeFile(espPath.string(), esp, &error));
    REQUIRE(error.empty());
    REQUIRE(esp.data.size() == density.data.size());
    REQUIRE(esp.atoms.size() == density.atoms.size());
    REQUIRE(esp.kind == CubeFieldKind::ElectrostaticPotential);
    REQUIRE(esp.minValue() < 0.0f);
    REQUIRE(esp.maxValue() > 0.0f);
}
