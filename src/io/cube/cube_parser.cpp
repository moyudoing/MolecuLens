// =============================================================================
// MolecuLens · io · cube/cube_parser.cpp —— Gaussian cube 体积/原子解析器实现
// 规格来源：Gaussian cubegen cube 格式（见 cube_volume.h 头注释）+ _esp/DESIGN.md
//   行 1–2：标题 / 注释（种类启发式的关键词来源）
//   行 3  ："natoms ox oy oz [nval]"（Bohr；natoms 为负 → 其后的原子记录之后
//            还有 DSET_IDS 记录：首个数 m 为数据组数，随后 m 个数据组编号）
//   行 4–6："n ax ay az" 网格点数 + 轴向量（Bohr；n 的符号按规范忽略）
//   行 7+ ：每原子 "Z charge x y z"（Bohr）
//   末尾  ：nx*ny*nz 个标量（每行最多 6 个；科学计数法/负数均常见）
// 实现要点：
//   · 长度 Bohr→Å（×0.529177210903f）：原点 / 网格向量 / 原子坐标；标量不换算
//   · 数据区可达数 MB 文本：整个文件一次读入内存，再用游标扫描（std::from_chars），
//     不做逐字符流或 std::stringstream 解析（大文件会慢一个数量级）
//   · CubeVolume 是单体积（每体素 1 个标量）：nval>1 或 DSET_IDS 多数据组时明确
//     报错，不静默取错数据
//   · 错误处理：不抛异常；失败返回 false 并写 *error
// =============================================================================

#include "io/cube/cube_parser.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <glm/glm.hpp>

#include "entity/element/element.h"
#include "entity/identity/identity.h"
#include "entity/search/bond_search.h"
#include "io/cube/cube_volume.h"
#include "spdlog/spdlog.h"

namespace moleculens::entity {

namespace {

// —— 常量 ——
constexpr float  kBohrToAngstrom = 0.529177210903f;  // Bohr → Å（cube 输入长度单位）
constexpr double kMinGridDeterminant = 1e-12;        // 网格向量行列式下限（Å³；退化网格）
constexpr std::size_t kMaxVoxels = 1000000000u;      // 体素总数上限（防坏头部 OOM）
constexpr std::size_t kMaxReserveVoxels = 16u * 1024u * 1024u;  // 预分配上限（超出按需增长）
constexpr int kMaxAtomCount = 2000000;               // 原子数上限（防坏头部 OOM）
constexpr int kMaxDataSetCount = 10000;              // DSET_IDS 数据组数上限

void setError(std::string* error, const std::string& message) {
    if (error != nullptr) {
        *error = message;
    }
}

bool isSpaceChar(char c) noexcept {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' || c == '\f';
}

std::string toLowerAscii(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (const char c : text) {
        if (c >= 'A' && c <= 'Z') {
            out.push_back(static_cast<char>(c - 'A' + 'a'));
        } else {
            out.push_back(c);
        }
    }
    return out;
}

bool containsAny(const std::string& haystack, std::initializer_list<const char*> needles) {
    for (const char* needle : needles) {
        if (haystack.find(needle) != std::string::npos) {
            return true;
        }
    }
    return false;
}

std::string dimsToString(const std::array<int, 3>& dims) {
    return std::to_string(dims[0]) + "x" + std::to_string(dims[1]) + "x" +
           std::to_string(dims[2]);
}

const char* cubeKindName(CubeFieldKind kind) {
    if (kind == CubeFieldKind::ElectronDensity) {
        return "electron-density";
    }
    if (kind == CubeFieldKind::ElectrostaticPotential) {
        return "electrostatic-potential";
    }
    return "unknown";
}

// —— 整文件读入（cube 数据区可达数 MB；一次读入后纯内存扫描）——
bool readWholeFile(const std::string& path, std::string& content, std::string* error) {
    std::ifstream input;
    input.open(path.c_str(), std::ios::binary | std::ios::ate);
    if (!input) {
        setError(error, "cube: cannot open file '" + path + "'");
        return false;
    }
    const std::streamoff endOffset = input.tellg();
    if (endOffset <= 0) {
        setError(error, "cube: file is empty: '" + path + "'");
        return false;
    }
    const auto fileSize = static_cast<std::size_t>(endOffset);
    content.resize(fileSize);
    input.seekg(0, std::ios::beg);
    input.read(content.data(), static_cast<std::streamsize>(fileSize));
    if (!input) {
        setError(error, "cube: read failure: '" + path + "'");
        return false;
    }
    return true;
}

// —— 数值 token 解析（locale 无关；兼容 D/d 指数与前导 '+'）——
bool parseFloatToken(std::string_view token, double& out) {
    constexpr std::size_t kMaxTokenLength = 63;
    if (token.empty() || token.size() > kMaxTokenLength) {
        return false;
    }
    char buffer[kMaxTokenLength + 1] = {};
    std::memcpy(buffer, token.data(), token.size());
    for (std::size_t index = 0; index < token.size(); ++index) {
        if (buffer[index] == 'D' || buffer[index] == 'd') {
            buffer[index] = 'E';   // Fortran 风格指数（部分量化程序输出 1.234D-02）
        }
    }
    const char* begin = buffer;
    const char* end = buffer + token.size();
    if (*begin == '+') {
        ++begin;   // from_chars 不识别前导 '+'
    }
    double value = 0.0;
    const std::from_chars_result result = std::from_chars(begin, end, value);
    if (result.ec == std::errc::result_out_of_range) {
        // 极端指数（<1e-308 / >1e308）在 cube 里基本不出现；strtod 兜底：
        // 下溢得 0、上溢得 ±HUGE_VAL，不抛异常（非有限值仍判非法）
        char* parseEnd = nullptr;
        const double fallback = std::strtod(buffer, &parseEnd);
        if (parseEnd != end || !std::isfinite(fallback)) {
            return false;
        }
        out = fallback;
        return true;
    }
    if (result.ec != std::errc{} || result.ptr != end || !std::isfinite(value)) {
        return false;
    }
    out = value;
    return true;
}

bool parseIntToken(std::string_view token, int& out) {
    constexpr std::size_t kMaxTokenLength = 31;
    if (token.empty() || token.size() > kMaxTokenLength) {
        return false;
    }
    char buffer[kMaxTokenLength + 1] = {};
    std::memcpy(buffer, token.data(), token.size());
    const char* begin = buffer;
    const char* end = buffer + token.size();
    if (*begin == '+') {
        ++begin;
    }
    int value = 0;
    const std::from_chars_result result = std::from_chars(begin, end, value);
    if (result.ec != std::errc{} || result.ptr != end) {
        return false;
    }
    out = value;
    return true;
}

// 头部整数字段也兼容写成浮点的写入器（"3.0" / "3.000000"）
bool parseIntTokenLoose(std::string_view token, int& out) {
    if (parseIntToken(token, out)) {
        return true;
    }
    double value = 0.0;
    if (!parseFloatToken(token, value)) {
        return false;
    }
    if (!(value >= -2147483648.0 && value <= 2147483647.0)) {
        return false;
    }
    const double rounded = std::round(value);
    if (std::fabs(value - rounded) > 1e-6) {
        return false;
    }
    out = static_cast<int>(rounded);
    return true;
}

// double → float 的安全收窄（超出 float 范围时失败；避免未定义行为）
bool toFloatChecked(double value, float& out) {
    if (!std::isfinite(value) ||
        std::fabs(value) > static_cast<double>(std::numeric_limits<float>::max())) {
        return false;
    }
    out = static_cast<float>(value);
    return true;
}

// —— 行游标（按 '\n' 切行；行内容引用原缓冲，不拷贝）——
class LineReader {
public:
    LineReader(const char* data, std::size_t size)
        : cursor_(data), end_(data + size) {}

    bool next(std::string_view& line) {
        if (cursor_ >= end_) {
            return false;
        }
        const char* newline = static_cast<const char*>(
            std::memchr(cursor_, '\n', static_cast<std::size_t>(end_ - cursor_)));
        const char* lineEnd = (newline != nullptr) ? newline : end_;
        line = std::string_view(cursor_, static_cast<std::size_t>(lineEnd - cursor_));
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }
        cursor_ = (newline != nullptr) ? (newline + 1) : end_;
        return true;
    }

    std::string_view remaining() const {
        return std::string_view(cursor_, static_cast<std::size_t>(end_ - cursor_));
    }

private:
    const char* cursor_ = nullptr;
    const char* end_ = nullptr;
};

// —— 空白分隔 token 游标（原子/DSET_IDS/数据区共用；不假设行边界）——
class TokenScanner {
public:
    TokenScanner(const char* data, std::size_t size) : data_(data), size_(size) {}

    bool atEnd() {
        skipSpace();
        return pos_ >= size_;
    }

    bool nextToken(std::string_view& token) {
        skipSpace();
        if (pos_ >= size_) {
            return false;
        }
        const std::size_t begin = pos_;
        while (pos_ < size_ && !isSpaceChar(data_[pos_])) {
            ++pos_;
        }
        token = std::string_view(data_ + begin, pos_ - begin);
        return true;
    }

    bool nextIntLoose(int& out) {
        std::string_view token;
        return nextToken(token) && parseIntTokenLoose(token, out);
    }

    bool nextFloat(double& out) {
        std::string_view token;
        return nextToken(token) && parseFloatToken(token, out);
    }

    bool nextFloat(float& out) {
        double value = 0.0;
        if (!nextFloat(value)) {
            return false;
        }
        return toFloatChecked(value, out);
    }

private:
    void skipSpace() {
        while (pos_ < size_ && isSpaceChar(data_[pos_])) {
            ++pos_;
        }
    }

    const char* data_ = nullptr;
    std::size_t size_ = 0;
    std::size_t pos_ = 0;
};

// —— 种类启发式 ——
// 先看标题/注释关键词；ESP 与 density 关键词同时出现时（如合成 demo 的
// "... electron density ... ESP-surface ..."）不能靠关键词二选一 → 交给数值特征。
CubeFieldKind classifyByComments(std::string_view comment1, std::string_view comment2) {
    std::string text = toLowerAscii(comment1);
    text.push_back('\n');
    text += toLowerAscii(comment2);
    const bool mentionsEsp =
        containsAny(text, {"esp", "electrostatic", "potential"});
    const bool mentionsDensity = containsAny(text, {"density", "rho"});
    if (mentionsEsp && !mentionsDensity) {
        return CubeFieldKind::ElectrostaticPotential;
    }
    if (mentionsDensity && !mentionsEsp) {
        return CubeFieldKind::ElectronDensity;
    }
    return CubeFieldKind::Unknown;
}

// 数值特征：ESP 有显著正负（表面兼有供电子/缺电子区），密度基本非负。
// 阈值取相对量级（容忍密度拟合的微小负噪声）。
CubeFieldKind classifyByStatistics(const std::vector<float>& data) {
    float lowest = 0.0f;
    float highest = 0.0f;
    bool any = false;
    for (const float value : data) {
        if (!std::isfinite(value)) {
            continue;
        }
        if (!any) {
            lowest = value;
            highest = value;
            any = true;
            continue;
        }
        lowest = std::min(lowest, value);
        highest = std::max(highest, value);
    }
    if (!any) {
        return CubeFieldKind::Unknown;
    }
    const float scale = std::max(std::fabs(lowest), std::fabs(highest));
    if (!(scale > 0.0f)) {
        return CubeFieldKind::Unknown;
    }
    const float negativeThreshold = 1.0e-3f * scale;
    if (lowest < -negativeThreshold && highest > negativeThreshold) {
        return CubeFieldKind::ElectrostaticPotential;
    }
    if (lowest >= -negativeThreshold) {
        return CubeFieldKind::ElectronDensity;
    }
    return CubeFieldKind::Unknown;
}

ElementType elementFromAtomicNumber(int atomicNumber) {
    if (atomicNumber < 1 || atomicNumber > 118) {
        return ElementType::Unknown;
    }
    return static_cast<ElementType>(static_cast<uint8_t>(atomicNumber));
}

// "int + 3x float"：cube 第 3 行与网格行共有的数字特征（canOpen 嗅探用）
bool looksLikeCubeNumberLine(std::string_view line) {
    TokenScanner scanner(line.data(), line.size());
    int head = 0;
    if (!scanner.nextIntLoose(head)) {
        return false;
    }
    for (int index = 0; index < 3; ++index) {
        double value = 0.0;
        if (!scanner.nextFloat(value)) {
            return false;
        }
    }
    return true;
}

// —— 解析主体（文件内容 → CubeVolume；不触碰 out，失败时 out 保持原样）——
bool parseCubeContent(std::string_view content, const std::string& sourceName,
                      CubeVolume& out, std::string* error) {
    CubeVolume volume;
    LineReader lines(content.data(), content.size());

    // 1) 行 1–2：标题/注释（允许空行；无长度限制）
    std::string_view comment1;
    std::string_view comment2;
    std::string_view atomCountLine;
    if (!lines.next(comment1) || !lines.next(comment2) || !lines.next(atomCountLine)) {
        setError(error, "cube: file too short (needs 2 comment lines + atom/grid header): '" +
                            sourceName + "'");
        return false;
    }

    // 2) 行 3："natoms ox oy oz [nval]"
    int atomCountField = 0;
    glm::vec3 originBohr(0.0f);
    int valuesPerVoxel = 1;
    {
        TokenScanner scanner(atomCountLine.data(), atomCountLine.size());
        if (!scanner.nextIntLoose(atomCountField)) {
            setError(error, "cube: line 3: invalid atom count in '" +
                                std::string(atomCountLine.substr(0, 40)) + "'");
            return false;
        }
        if (!scanner.nextFloat(originBohr.x) || !scanner.nextFloat(originBohr.y) ||
            !scanner.nextFloat(originBohr.z)) {
            setError(error, "cube: line 3: missing or invalid origin coordinate");
            return false;
        }
        // 可选第 5 个字段：每体素数据个数（多数文件省略 = 1；>1 不支持）
        std::string_view extra;
        int extraValue = 1;
        if (scanner.nextToken(extra) && parseIntTokenLoose(extra, extraValue)) {
            valuesPerVoxel = extraValue;
        }
    }

    // 3) 行 4–6：网格点数 + 轴向量（Bohr）
    std::array<int, 3> dims{0, 0, 0};
    std::array<glm::vec3, 3> axesBohr{glm::vec3(0.0f), glm::vec3(0.0f), glm::vec3(0.0f)};
    for (int axis = 0; axis < 3; ++axis) {
        std::string_view gridLine;
        if (!lines.next(gridLine)) {
            setError(error, "cube: file ends before grid definition "
                            "(3 grid lines must follow line 3)");
            return false;
        }
        TokenScanner scanner(gridLine.data(), gridLine.size());
        int voxelCount = 0;
        if (!scanner.nextIntLoose(voxelCount) || voxelCount == std::numeric_limits<int>::min()) {
            setError(error, "cube: grid line " + std::to_string(axis + 4) +
                                ": invalid voxel count");
            return false;
        }
        // cube 规定长度恒为 Bohr；少数写入器用负号标记"单位"（语义已过时）→ 忽略符号
        dims[axis] = (voxelCount < 0) ? -voxelCount : voxelCount;
        if (!scanner.nextFloat(axesBohr[axis].x) || !scanner.nextFloat(axesBohr[axis].y) ||
            !scanner.nextFloat(axesBohr[axis].z)) {
            setError(error, "cube: grid line " + std::to_string(axis + 4) +
                                ": missing or invalid axis vector");
            return false;
        }
    }

    // 4) 行 7+ 起全部按空白 token 流处理（原子 → DSET_IDS → 数据区）
    const std::string_view remaining = lines.remaining();
    TokenScanner scanner(remaining.data(), remaining.size());

    int atomCount = 0;
    if (atomCountField == std::numeric_limits<int>::min()) {
        setError(error, "cube: invalid atom count " + std::to_string(atomCountField));
        return false;
    }
    atomCount = (atomCountField < 0) ? -atomCountField : atomCountField;
    if (atomCount > kMaxAtomCount) {
        setError(error, "cube: atom count " + std::to_string(atomCount) +
                            " exceeds the supported limit");
        return false;
    }
    // 明显不足的原子区（坏头部声明大量原子但文件只剩几 KB）提前拒绝，
    // 避免按声明值做大块预分配
    if (remaining.size() < static_cast<std::size_t>(atomCount) * 5u) {
        setError(error, "cube: file ends before " + std::to_string(atomCount) +
                            " atom records");
        return false;
    }

    volume.atoms.reserve(static_cast<std::size_t>(atomCount));
    for (int index = 0; index < atomCount; ++index) {
        int atomicNumber = 0;
        float nuclearCharge = 0.0f;
        glm::vec3 positionBohr(0.0f);
        if (!scanner.nextIntLoose(atomicNumber) || !scanner.nextFloat(nuclearCharge) ||
            !scanner.nextFloat(positionBohr.x) || !scanner.nextFloat(positionBohr.y) ||
            !scanner.nextFloat(positionBohr.z)) {
            setError(error, "cube: atom record " + std::to_string(index + 1) + "/" +
                                std::to_string(atomCount) + " is truncated or invalid");
            return false;
        }
        CubeAtom atom;
        atom.atomicNumber = atomicNumber;
        atom.nuclearCharge = nuclearCharge;
        atom.position = positionBohr * kBohrToAngstrom;
        volume.atoms.push_back(atom);
    }

    // 5) natoms < 0 → DSET_IDS：首个数 m = 数据组数，随后 m 个数据组编号
    int dataSetCount = 1;
    if (atomCountField < 0) {
        if (!scanner.nextIntLoose(dataSetCount) || dataSetCount <= 0 ||
            dataSetCount > kMaxDataSetCount) {
            setError(error, "cube: DSET_IDS record is missing or invalid "
                            "(negative atom count in line 3)");
            return false;
        }
        for (int index = 0; index < dataSetCount; ++index) {
            int dataSetId = 0;
            if (!scanner.nextIntLoose(dataSetId)) {
                setError(error, "cube: DSET_IDS record is truncated (" +
                                    std::to_string(index + 1) + "/" +
                                    std::to_string(dataSetCount) + ")");
                return false;
            }
        }
        spdlog::info("[io] cube '{}': negative atom count → DSET_IDS with {} data set(s)",
                     sourceName, dataSetCount);
    }
    if (dataSetCount != 1 || valuesPerVoxel != 1) {
        setError(error, "cube: multiple values per voxel are not supported "
                        "(data sets = " + std::to_string(dataSetCount) +
                            ", values per voxel = " + std::to_string(valuesPerVoxel) + ")");
        return false;
    }

    // 6) 网格尺寸（溢出安全）+ 数据区
    std::size_t totalVoxels = 1;
    for (int axis = 0; axis < 3; ++axis) {
        if (dims[axis] <= 0) {
            setError(error, "cube: grid dimension " + std::to_string(axis + 1) +
                                " is not positive (" + std::to_string(dims[axis]) + ")");
            return false;
        }
        const auto voxelCount = static_cast<std::size_t>(dims[axis]);
        if (totalVoxels > kMaxVoxels / voxelCount) {
            setError(error, "cube: grid " + dimsToString(dims) + " is too large");
            return false;
        }
        totalVoxels *= voxelCount;
    }
    volume.data.reserve(std::min<std::size_t>(totalVoxels, kMaxReserveVoxels));
    for (std::size_t index = 0; index < totalVoxels; ++index) {
        float value = 0.0f;
        if (!scanner.nextFloat(value)) {
            setError(error, "cube: data section truncated or invalid at value " +
                                std::to_string(index) + " (grid " + dimsToString(dims) +
                                " needs " + std::to_string(totalVoxels) + " values)");
            return false;
        }
        volume.data.push_back(value);
    }
    if (!scanner.atEnd()) {
        setError(error, "cube: unexpected extra data after the last voxel (grid " +
                            dimsToString(dims) + " needs " + std::to_string(totalVoxels) +
                            " values)");
        return false;
    }

    // 7) 长度单位 Bohr→Å + 逆矩阵（world Å → 分数体素坐标）
    volume.dims = dims;
    volume.origin = originBohr * kBohrToAngstrom;
    for (int axis = 0; axis < 3; ++axis) {
        volume.axes[axis] = axesBohr[axis] * kBohrToAngstrom;
    }
    glm::dmat3 axesMatrix(1.0);
    for (int column = 0; column < 3; ++column) {
        axesMatrix[column] = glm::dvec3(volume.axes[column]);
    }
    const double determinant = glm::determinant(axesMatrix);
    if (!std::isfinite(determinant) || std::fabs(determinant) < kMinGridDeterminant) {
        setError(error, "cube: degenerate grid vectors (determinant = " +
                            std::to_string(determinant) + ")");
        return false;
    }
    volume.axesInverse = glm::inverse(axesMatrix);

    // 8) 种类启发式：标题/注释关键词优先；冲突或缺失时按数值特征判定
    volume.kind = classifyByComments(comment1, comment2);
    if (volume.kind == CubeFieldKind::Unknown) {
        volume.kind = classifyByStatistics(volume.data);
    }

    out = std::move(volume);
    spdlog::info("[io] cube loaded: {}, {} voxels, {} atoms, kind={} from '{}'",
                 dimsToString(out.dims), out.data.size(), out.atoms.size(),
                 cubeKindName(out.kind), sourceName);
    return true;
}

} // namespace

// =============================================================================
// CubeVolume（cube_volume.h 中声明、实现放这里的非 inline 部分）
// =============================================================================

bool CubeVolume::sampleTrilinear(const glm::vec3& world, float& out) const {
    if (!valid()) {
        return false;
    }
    // world Å → 分数体素坐标（axesInverse 由 parseCubeFile 填充）
    const glm::dvec3 local = axesInverse * glm::dvec3(world - origin);
    const double fraction[3] = {local.x, local.y, local.z};
    int base[3] = {0, 0, 0};
    double weight[3] = {0.0, 0.0, 0.0};
    constexpr double kEdgeEpsilon = 1e-6;   // 浮点误差容限（边界体素视为在界内）
    for (int axis = 0; axis < 3; ++axis) {
        const double maxIndex = static_cast<double>(dims[axis] - 1);
        if (!std::isfinite(fraction[axis]) || fraction[axis] < -kEdgeEpsilon ||
            fraction[axis] > maxIndex + kEdgeEpsilon) {
            return false;   // 出界：out 不变
        }
        const double clamped = std::clamp(fraction[axis], 0.0, maxIndex);
        int lower = static_cast<int>(std::floor(clamped));
        if (lower > dims[axis] - 2) {
            lower = dims[axis] - 2;   // valid() 保证 dims > 1
        }
        if (lower < 0) {
            lower = 0;
        }
        base[axis] = lower;
        weight[axis] = clamped - static_cast<double>(lower);
    }
    const auto corner = [this, &base](int dx, int dy, int dz) -> double {
        return static_cast<double>(
            value(base[0] + dx, base[1] + dy, base[2] + dz));
    };
    const double c000 = corner(0, 0, 0);
    const double c100 = corner(1, 0, 0);
    const double c010 = corner(0, 1, 0);
    const double c110 = corner(1, 1, 0);
    const double c001 = corner(0, 0, 1);
    const double c101 = corner(1, 0, 1);
    const double c011 = corner(0, 1, 1);
    const double c111 = corner(1, 1, 1);
    const double wx = weight[0];
    const double wy = weight[1];
    const double wz = weight[2];
    const double c00 = c000 + (c100 - c000) * wx;
    const double c01 = c001 + (c101 - c001) * wx;
    const double c10 = c010 + (c110 - c010) * wx;
    const double c11 = c011 + (c111 - c011) * wx;
    const double c0 = c00 + (c10 - c00) * wy;
    const double c1 = c01 + (c11 - c01) * wy;
    out = static_cast<float>(c0 + (c1 - c0) * wz);
    return true;
}

float CubeVolume::minValue() const {
    float lowest = 0.0f;
    bool any = false;
    for (const float value : data) {
        if (!std::isfinite(value)) {
            continue;
        }
        if (!any || value < lowest) {
            lowest = value;
            any = true;
        }
    }
    return any ? lowest : 0.0f;
}

float CubeVolume::maxValue() const {
    float highest = 0.0f;
    bool any = false;
    for (const float value : data) {
        if (!std::isfinite(value)) {
            continue;
        }
        if (!any || value > highest) {
            highest = value;
            any = true;
        }
    }
    return any ? highest : 0.0f;
}

bool parseCubeFile(const std::string& path, CubeVolume& out, std::string* error) {
    std::string content;
    if (!readWholeFile(path, content, error)) {
        return false;
    }
    return parseCubeContent(std::string_view(content), path, out, error);
}

// =============================================================================
// CubeFileReader —— cube → SceneStructure（仅头部原子；体积数据供 ESP 表面用）
// =============================================================================

bool CubeFileReader::canOpen(const std::filesystem::path& path) const {
    // 1) 扩展名（注册表通常已按扩展名命中，这里再兜底一次）
    const std::string extension = toLowerAscii(path.extension().string());
    if (extension == ".cube" || extension == ".cub") {
        return true;
    }
    // 2) 文本启发式：第 3 行 "natoms ox oy oz"，第 4–6 行 "n ax ay az"
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return false;
    }
    std::string comment1;
    std::string comment2;
    std::string atomCountLine;
    if (!std::getline(input, comment1) || !std::getline(input, comment2) ||
        !std::getline(input, atomCountLine) || !looksLikeCubeNumberLine(atomCountLine)) {
        return false;
    }
    for (int axis = 0; axis < 3; ++axis) {
        std::string gridLine;
        if (!std::getline(input, gridLine) || !looksLikeCubeNumberLine(gridLine)) {
            return false;
        }
    }
    return true;
}

Result<ParseResult> CubeFileReader::parse(const std::filesystem::path& path,
                                          const ParseOptions& opts) {
    CubeVolume volume;
    std::string error;
    if (!parseCubeFile(path.string(), volume, &error)) {
        return Result<ParseResult>::error(ErrorCode::ParseError, error);
    }
    if (volume.atoms.empty()) {
        return Result<ParseResult>::error(
            ErrorCode::ParseError,
            "cube: file contains no atom records: '" + path.string() + "'");
    }

    auto scene = std::make_shared<SceneStructure>();
    ParseResult result;
    result.sourceFile = path.string();
    result.scene = scene;
    result.warnings.push_back(
        {0, "体积数据仅用于 ESP 表面可视化；场景仅导入 cube 头部原子（坐标已由 Bohr 换算为 Å）"});

    const int32_t chainId = scene->addChain("1");
    const int32_t residueId = scene->addResidue("CUB");

    std::vector<glm::vec3> positions;
    positions.reserve(volume.atoms.size());
    MoleculeStructure structure;
    structure.elements.reserve(volume.atoms.size());
    structure.atoms.reserve(volume.atoms.size());
    structure.atomNameIds.reserve(volume.atoms.size());

    for (const CubeAtom& cubeAtom : volume.atoms) {
        const ElementType element = elementFromAtomicNumber(cubeAtom.atomicNumber);
        structure.elements.push_back(element);

        Atom atom;
        atom.element = element;
        atom.occupancy = 100;
        atom.bfactor = 0;
        atom.residueId = residueId;
        atom.chainId = chainId;
        structure.atoms.push_back(atom);

        structure.atomNameIds.push_back(
            scene->strings().intern(elementInfo(element).symbol));
        positions.push_back(cubeAtom.position);
    }

    // cube 无显式键表 → 按元素共价半径 + 距离推断（与 PDB 缺 CONECT 时同一路径）
    if (opts.inferBonds) {
        BondSearch::search(positions, structure.elements, structure.bonds);
    }

    structure.key = computeStructureKey(structure);
    const uint32_t structureIndex = scene->registerStructure(structure);

    auto trajectory = std::make_shared<Trajectory>();
    trajectory->dt = result.dt;
    Timestep timestep;
    timestep.positions = std::move(positions);
    trajectory->frames.push_back(std::move(timestep));

    std::string instanceName = path.stem().string();
    if (instanceName.empty()) {
        instanceName = "cube";
    }
    scene->addTrajectoryInstance(structureIndex, Transform{}, trajectory, UINT32_MAX,
                                 instanceName, false);
    scene->setDirty(DIRTY_ALL);
    scene->bumpSerial();

    spdlog::info("[io] cube scene imported: {} atoms, {} bonds from '{}'",
                 structure.elements.size(), structure.bonds.size(),
                 path.filename().string());
    return result;
}

} // namespace moleculens::entity
