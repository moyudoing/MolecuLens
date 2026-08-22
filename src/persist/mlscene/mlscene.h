// =============================================================================
// MolecuLens · persist · mlscene.h —— 场景文件格式
// 规格来源：主方案 §12.1 / 附录 G；状态：格式定稿 v1（本文档为唯一权威）
// 说明：MolecuLens 场景文件的唯一二进制格式定义。
// 字节序：little-endian；长度字段为 u64；各段内部 16B 对齐。
// 依赖：Result/SceneSnapshot 定义于 entity 基础设施头文件。
// 独立编译约束（主方案 §3.5.3）：只依赖 <cstdint>/<filesystem> 与标准库。
// =============================================================================

#pragma once

#include <cstdint>
#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>
#include <array>
#include <optional>

#include "entity/result.h"

namespace moleculens::persist::mlscene {

inline constexpr char     kMagic[6] = { 'M', 'L', 'S', 'C', 'N', 0x01 };
inline constexpr uint16_t kVersion  = 1;
inline constexpr uint32_t kFixed    = 16;   // magic(6) + version(2) + metaSize(8)

// —— 固定前缀（16B）——
struct Header {
    char     magic[6];     // "MLSCN\x01"
    uint16_t version;      // = kVersion
    uint64_t metaSize;     // 元数据 JSON 段字节数
};
static_assert(sizeof(Header) == 16);

// —— 文件整体布局（偏移自文件头，little-endian）——
// [0]        Header
// [16]       u8 meta[metaSize]         元数据 JSON（UTF-8）
// [16+meta]  u64 cmdSize               命令流段字节数
//            u8 cmd[cmdSize]           SceneCommandList 序列化（见 engine/cmd/cmd.h 附录 F）
//            u64 coordsSize            坐标 SoA 段字节数
//            u8 coords[coordsSize]     坐标 SoA 段
//            u32 crc32                 CRC32(0 .. filesize-4)
// 写入：临时文件 + fsync + rename（原子性）；读取按 lenient 容错。

// —— 坐标 SoA 段（coords[]）——
// [0]  u64 posCount
//      u8  positions[posCount * 12]     glm::vec3（Å，实例世界坐标）
//      u64 colorCount
//      u8  colors[colorCount * 16]      glm::vec4（预计算 RGBA）
//      u64 idCount
//      u8  atomIds[idCount * 4]         uint32（全局原子 ID，拾取回查）
// 约束：posCount == colorCount == idCount；不一致则拒绝加载。

// —— 元数据 JSON 结构（meta[]，nlohmann-json 序列化）——
// {
//   "version": 1,
//   "timestamp": "2026-08-18T00:00:00Z",
//   "units": "angstrom",
//   "structures": [
//     { "elements": ["C","N","O", ...],
//       "bonds":    [ [0,1,1], [1,2,2], ... ] }            // [a, b, order]
//   ],
//   "instances": [
//     { "structureIndex": 0, "name": "protein", "visible": true,
//       "transform": [9 个浮点：平移 + 旋转(四元数) + 缩放],
//       "trajectory": { "frameCount": 1, "dt": 2.0 } }
//   ],
//   "representations": [
//     { "name": "rep0", "type": "ballstick", "selection": "*",
//       "colorScheme": "element", "radiusScale": 1.0, "visible": true }
//   ],
//   "scene":  { "bgColor": [0.1,0.1,0.15], "ambient": 0.3, "scale": 1.0 },
//   "camera": { "target": [0,0,0], "distance": 40.0, "yaw": 0.0, "pitch": 0.5 }
// }

// —— 场景快照（load 的产物；无渲染依赖的纯数据）——
struct SceneSnapshot {
    struct Structure {
        std::vector<std::string> elements;   // 元素符号
        std::vector<std::array<uint32_t, 3>> bonds;  // [a, b, order]
    };
    struct Instance {
        uint32_t structureIndex = 0;
        std::string name;
        bool visible = true;
        std::array<float, 9> transform{1, 0, 0, 0, 1, 0, 0, 0, 1};  // 平移+旋转(四元数)+缩放
        uint64_t frameCount = 1;
        float dt = 2.0f;
    };
    struct Representation {
        std::string name;
        std::string type;
        std::string selection;
        std::string colorScheme;
        float radiusScale = 1.0f;
        bool visible = true;
    };
    std::vector<Structure> structures;
    std::vector<Instance> instances;
    std::vector<Representation> representations;
    std::array<float, 3> bgColor{0.1f, 0.1f, 0.15f};
    float ambient = 0.3f;
    float scale = 1.0f;
    std::string units = "angstrom";
    std::array<float, 3> cameraTarget{0, 0, 0};
    float cameraDistance = 40.0f;
    float cameraYaw = 0.0f;
    float cameraPitch = 0.5f;
    // 命令流与坐标（可选加载）
    std::vector<uint8_t> commandStream;    // SceneCommandList 序列化
    std::vector<float> coordsPositions;    // vec3 × n
    std::vector<float> coordsColors;       // vec4 × n
    std::vector<uint32_t> coordsAtomIds;
};

// —— 读写接口（实现签名）——
struct LoadOptions {
    bool strict     = false;   // true=格式错误即抛；false=lenient 跳过并记警告
    bool lazyCoords = false;   // true=坐标段延迟读（大场景内存优化）
};
struct SaveOptions {
    bool writeCoords = true;   // false=仅结构+命令流（坐标留空）
    int  jsonIndent  = 2;
};

class MlSceneSerializer {
public:
    static entity::Result<SceneSnapshot> load(const std::filesystem::path&,
                                              const LoadOptions& = {});
    static entity::Result<void> save(const SceneSnapshot&,
                                     const std::filesystem::path&,
                                     const SaveOptions& = {});
    // save 内部：写 <path>.tmp → fsync → rename（原子替换）
    // CRC32：IEEE 802.3（多项式 0xEDB88320），覆盖偏移 [0, filesize-4)
};

} // namespace moleculens::persist::mlscene
