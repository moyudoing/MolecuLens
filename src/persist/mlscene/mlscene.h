// =============================================================================
// MolecuLens · persist · mlscene.h —— 场景文件格式（v2）
// 规格来源：主方案 §12.1 / 附录 G
// 说明：MolecuLens 场景文件的唯一二进制格式定义。
// 字节序：little-endian；长度字段为 u64；各段内部 16B 对齐。
// 依赖：Result/SceneSnapshot 定义于 entity 基础设施头文件。
// 独立编译约束（主方案 §3.5.3）：只依赖 <cstdint>/<filesystem> 与标准库。
//
// ⚠️ v2 相对 v1 的**破坏性变更**（不做 v1 兼容读取，版本不匹配直接拒绝）：
//   v1 的元数据只存"元素符号 + 键表 + 实例位姿 + 表示基础字段 + 背景/环境光"，
//   导致原子名/残基/链/BFactor、体系分组（位姿+渲染模式+溶剂标记）、完整光照与雾、
//   表示的全部参数都无法还原 —— 存出来的场景重新打开会掉色、掉分组。
//   v2 补齐上述字段；轨迹帧**不内嵌**，改为记录源文件路径 + 指纹（大小/mtime）。
//
// ⚠️ 本模块**不依赖 engine**：命令流段按"已序列化的字节"原样读写，序列化/反序列化
//   由 app 层调用 engine::SceneCommandList::writeTo/readFrom 完成。
//   （此前 save() 直接调用 engine 符号，使 moleculens_entity 反向依赖 moleculens_engine，
//     只因所有消费者都同时链接两者才未暴露。）
// =============================================================================

#pragma once

#include <cstdint>
#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>
#include <array>

#include "entity/result.h"

namespace moleculens::persist::mlscene {

inline constexpr char     kMagic[6] = { 'M', 'L', 'S', 'C', 'N', 0x02 };
inline constexpr uint16_t kVersion  = 2;
inline constexpr uint32_t kFixed    = 16;   // magic(6) + version(2) + metaSize(8)

// —— 固定前缀（16B）——
struct Header {
    char     magic[6];     // "MLSCN\x02"
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

// —— 元数据 JSON 结构（meta[]，nlohmann-json 序列化，v2）——
// {
//   "version": 2,
//   "units": "angstrom",
//   "structures": [
//     { "atoms": [ { "name":"CA", "element":"C", "residueName":"ALA",
//                    "residueId":1, "chainId":0, "bfactor":0.0, "occupancy":1.0 } ],
//       "bonds": [ [0,1,1] ] }                      // [a, b, order]
//   ],
//   "systems": [ { "name":"体系 1", "visible":true, "renderMode":0,
//                  "transform": { "translation":[3], "rotation":[4], "scale":1.0 } } ],
//   "instances": [
//     { "structureIndex":0, "systemIndex":0, "name":"protein", "visible":true,
//       "isSolvent":false,
//       "transform": { "translation":[3], "rotation":[4], "scale":1.0 },
//       "trajectory": { "sourcePath":"…/run.xtc", "sourceSize":123, "sourceMtime":456,
//                       "frameCount":100, "dt":2.0, "periodic":false } }
//   ],
//   "representations": [
//     { "name":"rep0", "type":"ballstick", "selection":"*", "colorScheme":"element",
//       "radiusScale":1.0, "bondScale":1.0, "visible":true, "systemSurface":false,
//       "surfIsovalue":0.5, "surfGridSpacing":0.5,
//       "ambient":[4], "diffuse":[4], "specular":[4],
//       "shininess":32.0, "metallic":0.0, "roughness":0.4, "opacity":1.0 }
//   ],
//   "scene":  { "bgColor":[3], "fog":[2], "fogMode":0, "fogDensity":0.05,
//               "ambient":0.45, "scale":1.0,
//               "skyColor":[3], "groundColor":[3], "ambientColor":[3] },
//   "camera": { "target":[3], "distance":40.0, "yaw":0.0, "pitch":0.5,
//               "roll":0.0, "fov":45.0, "ortho":false }
// }

// —— 位姿（v2 用具名字段取代 v1 的 "9 个 float"）——
// v1 存 9 个 float 并注释为"平移+旋转(四元数)+缩放"，但那是 3+4+1=8 个数，
// 与 9 个 float 对不上（默认值 {1,0,0,0,1,0,0,0,1} 实为单位 3×3 矩阵）——
// 注释与实现不一致最容易在读写两端各自"自洽地"理解错。v2 改为具名字段。
struct TransformData {
    std::array<float, 3> translation{0.0f, 0.0f, 0.0f};
    std::array<float, 4> rotation{1.0f, 0.0f, 0.0f, 0.0f};   // 四元数 (w, x, y, z)
    float                scale = 1.0f;

    // C++17 无默认化的 operator==（C++20 才有），显式写出供往返测试比较
    bool operator==(const TransformData& o) const {
        return translation == o.translation && rotation == o.rotation &&
               scale == o.scale;
    }
    bool operator!=(const TransformData& o) const { return !(*this == o); }
};

// —— 场景快照（load 的产物 / save 的输入；无渲染依赖的纯数据）——
struct SceneSnapshot {
    // 单个原子（v2 新增：v1 只有元素符号，原子名/残基/链/BFactor 全部丢失，
    // 导致卡通表示与按残基/链/BFactor 配色无法还原）。
    // 残基名/链名**不逐原子存**——它们与领域模型一致地放在场景级名字池
    // （chainNames / residueNames，按 id 索引），逐原子存既冗余又不完整（v1 草案
    // 只存了 residueName 而漏了 chainName）。
    struct Atom {
        std::string name;
        std::string element;
        int32_t     residueId = 0;   // → SceneSnapshot::residueNames 下标
        int32_t     chainId = 0;     // → SceneSnapshot::chainNames 下标
        float       bfactor = 0.0f;  // Å²（领域模型内以 ×100 定点存储）
        float       occupancy = 1.0f;
    };
    struct Structure {
        std::vector<Atom> atoms;
        std::vector<std::array<uint32_t, 3>> bonds;   // [a, b, order]
    };

    // 轨迹引用：**不内嵌帧坐标**（多帧大体系会让文件膨胀到数百 MB）。
    // 只记录源文件路径 + 指纹；文件缺失或被替换时提示重新加载。
    struct TrajectoryRef {
        std::string sourcePath;
        uint64_t    sourceSize = 0;
        int64_t     sourceMtime = 0;
        uint64_t    frameCount = 1;
        float       dt = 2.0f;
        bool        periodic = false;
    };

    struct System {
        std::string name;
        bool visible = true;
        TransformData transform;
        uint8_t renderMode = 0;   // 0 实心 / 1 线框 / 2 透明 / 3 点
    };

    struct Instance {
        uint32_t structureIndex = 0;
        int32_t  systemIndex = -1;      // → systems[]；-1 = 未归组
        std::string name;
        bool visible = true;
        bool isSolvent = false;
        TransformData transform;
        TrajectoryRef trajectory;
    };

    struct Representation {
        std::string name;
        std::string type;
        std::string selection;
        std::string colorScheme;
        float radiusScale = 1.0f;
        float bondScale = 1.0f;
        bool  visible = true;
        bool  systemSurface = false;
        float surfIsovalue = 0.5f;
        float surfGridSpacing = 0.5f;
        std::array<float, 4> ambient{0.60f, 0.60f, 0.60f, 1.0f};
        std::array<float, 4> diffuse{0.90f, 0.90f, 0.90f, 1.0f};
        std::array<float, 4> specular{0.30f, 0.30f, 0.30f, 1.0f};
        float shininess = 32.0f;
        float metallic = 0.0f;
        float roughness = 0.40f;
        float opacity = 1.0f;
    };

    std::vector<Structure> structures;
    // 场景级名字池（与 SceneStructure 的 chainNames_/residueNames_ 一致）：
    // Atom::chainId / residueId 是这两张表的下标；解析器按顺序分配，故 id 稠密。
    std::vector<std::string> chainNames;
    std::vector<std::string> residueNames;
    std::vector<System> systems;
    std::vector<Instance> instances;
    std::vector<Representation> representations;

    // 场景属性（v2 补齐雾与全局光三色）
    std::array<float, 3> bgColor{0.1f, 0.1f, 0.15f};
    std::array<float, 2> fog{0.0f, 0.0f};
    float fogMode = 0.0f;
    float fogDensity = 0.05f;
    float ambient = 0.45f;
    float scale = 1.0f;
    std::array<float, 3> skyColor{1.0f, 1.0f, 1.0f};
    std::array<float, 3> groundColor{0.35f, 0.4f, 0.5f};
    std::array<float, 3> ambientColor{0.55f, 0.6f, 0.75f};
    std::string units = "angstrom";

    // 相机（v2 补齐 roll/fov/ortho）
    std::array<float, 3> cameraTarget{0, 0, 0};
    float cameraDistance = 40.0f;
    float cameraYaw = 0.0f;
    float cameraPitch = 0.5f;
    float cameraRoll = 0.0f;
    float cameraFov = 45.0f;
    bool  cameraOrtho = false;

    // 命令流与坐标（可选段）
    std::vector<uint8_t> commandStream;    // SceneCommandList 序列化字节（原样读写）
    std::vector<float> coordsPositions;    // vec3 × n
    std::vector<float> coordsColors;       // vec4 × n
    std::vector<uint32_t> coordsAtomIds;
};

// —— 读写接口 ——
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
