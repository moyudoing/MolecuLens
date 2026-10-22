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
//   v2 补齐上述字段。轨迹支持两种模式：**链接式**（默认，只记源文件路径 + 指纹）
//   与**嵌入式**（帧坐标写进 CRC 之前的可选二进制段），二者共用同一版本号。
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
//            [u64 trajSize]            **可选**内嵌轨迹段字节数（仅嵌入式保存时存在）
//            [u8  traj[trajSize]]
//            u32 crc32                 CRC32(0 .. filesize-4)
// 写入：临时文件 + fsync + rename（原子性）；读取按 lenient 容错。
//
// —— 内嵌轨迹段（traj[]；可选，链接式保存时不写，字节布局与旧版完全一致）——
// u32 trajCount
// for each trajectory:
//   u64 frameCount
//   u32 atomCount
//   u32 flags                bit0 = hasBoxes
//   u8  positions[frameCount * atomCount * 12]   float32 × [f][atom][xyz]，帧优先
//   [u8 boxes[frameCount * 36]]                  float32 × [f][9]（仅 hasBoxes）
// 读侧判据：坐标段之后若剩余字节 ≥ 8 + 4(CRC)，先视作 trajSize；越界 strict 报错、
// lenient 跳过（旧文件在坐标段后直接就是 CRC，因此不会误读）。

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
//                       "frameCount":100, "dt":2.0, "periodic":false,
//                       // 可选键（旧文件缺省 → 链接式）：
//                       "mode":"linked"|"embedded", "embeddedIndex":0,
//                       "trajAtomOffset":0 } } ],
//   "embeddedTrajectories": [   // 可选；仅嵌入式保存时出现，顺序即 embeddedIndex
//     { "frameCount":100, "atomCount":6452, "dt":2.0, "periodic":true,
//       "hasBoxes":true } ],    // 帧坐标本体在二进制 traj 段，JSON 只存形状
//   "representations": [
//     { "name":"rep0", "type":"ballstick", "selection":"*", "colorScheme":"element",
//       "radiusScale":1.0, "bondScale":1.0, "visible":true, "systemSurface":false,
//       "surfIsovalue":0.5, "surfGridSpacing":0.5,
//       "ambient":[4], "diffuse":[4], "specular":[4],
//       "shininess":32.0, "metallic":0.0, "roughness":0.4, "opacity":1.0 }
//   ],
//   "scene":  { "bgColor":[3], "fog":[2], "fogMode":0, "fogDensity":0.05,
//               "ambient":0.45, "scale":1.0,
//               "skyColor":[3], "groundColor":[3], "ambientColor":[3],
//               // P6 新增（可选键）：方向光
//               "dirLightDir":[3], "dirLightColor":[3],
//               "dirLightIntensity":0.35, "dirLightEnabled":true },
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

// —— 场景标注（v2.3，可选段）——
// 视图叠加层（水盒 / 文字标签 / 箭头 / 水印）随场景一起保存：重开 .mlscene 就能
// 直接复现做好的示意图，不必重新摆一遍。它是**表现层数据**，但不依赖 ImGui/engine
// 类型，故放在 persist（纯数据）里，由 app 层做转换。
// 缺省键 → 空标注：旧文件完全不受影响。
struct AnnotationBox {
    bool visible = false;
    std::array<float, 3> origin{0.0f, 0.0f, 0.0f};
    std::array<float, 9> axes{};   // a1(3) + a2(3) + a3(3)：支持三斜盒
    std::array<float, 4> color{0.45f, 0.62f, 0.85f, 0.9f};
    float lineWidth = 1.4f;
};

struct AnnotationLabel {
    std::string text;
    std::array<float, 3> pos{0.0f, 0.0f, 0.0f};
    std::array<float, 4> color{1.0f, 0.92f, 0.55f, 1.0f};
    float pixelSize = 18.0f;
    bool leader = true;
    bool visible = true;
};

struct AnnotationArrow {
    std::array<float, 3> from{0.0f, 0.0f, 0.0f};
    std::array<float, 3> to{0.0f, 0.0f, 0.0f};
    std::array<float, 4> color{1.0f, 0.85f, 0.30f, 1.0f};
    float width = 2.2f;
    float headScale = 1.0f;
    bool doubleHead = false;
    int32_t style = 0;   // 0 普通 / 1 波浪（光）
    bool visible = true;
};

struct AnnotationWatermark {
    bool visible = false;
    std::string text;
    std::array<float, 4> color{1.0f, 1.0f, 1.0f, 0.16f};
    float scale = 0.10f;
    int32_t anchor = 0;   // 0 右下 / 1 左下 / 2 居中 / 3 右上
};

struct Annotations {
    AnnotationBox box;
    std::vector<AnnotationLabel> labels;
    std::vector<AnnotationArrow> arrows;
    AnnotationWatermark watermark;

    [[nodiscard]] bool empty() const {
        return !box.visible && labels.empty() && arrows.empty() && !watermark.visible;
    }
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

    // 轨迹引用。两种存储模式：
    //   "linked"   —— 只记录源文件路径 + 指纹（默认；多帧大体系文件小，但源文件
    //                 被移动/替换后需重新加载）。
    //   "embedded" —— 帧坐标写进同文件的二进制 traj 段（单文件可分发），
    //                 embeddedIndex 指向 SceneSnapshot::embeddedTrajectories。
    // 旧文件没有 mode 键 → 读侧取默认值 "linked"，行为与之前完全一致。
    struct TrajectoryRef {
        std::string mode = "linked";      // "linked" / "embedded"
        int32_t     embeddedIndex = -1;   // → embeddedTrajectories 下标；-1 = 无
        // 体系级共享轨迹时，本实例第 0 个原子在该轨迹整体系帧坐标里的偏移。
        // 单实例私有轨迹恒为 0（与历史行为一致）。
        uint32_t    trajAtomOffset = 0;
        std::string sourcePath;
        uint64_t    sourceSize = 0;
        int64_t     sourceMtime = 0;
        uint64_t    frameCount = 1;
        float       dt = 2.0f;
        bool        periodic = false;
    };

    // 内嵌轨迹的**形状 + 帧坐标本体**。
    // 为什么单独一张表：多实例体系（md.gro 是 2042 个实例共享一条整体系轨迹）
    // 若逐实例存一份帧坐标，文件体积会按实例数翻倍；表 + 实例 ref 的
    // embeddedIndex 使"只内嵌一次"成为格式上的必然。
    // positions 布局：帧优先 [f][atom][xyz]，长度 = frameCount*atomCount*3。
    // boxes 可为空；非空时长度 = frameCount*9。
    struct EmbeddedTrajectory {
        uint64_t frameCount = 0;
        uint32_t atomCount = 0;
        float    dt = 2.0f;
        bool     periodic = false;
        std::vector<float> positions;
        std::vector<float> boxes;
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
        // 表面平滑次数（0–3）：vdW 标量场盒式平滑；旧文件缺键 → 1。
        int   surfSmooth = 1;
        // ESP 映射表面（type == "espsurface"）：cube 数据源 + 色标参数（v3 新增，
        // 旧文件缺键 → 默认值，不影响载入）。
        std::string espDensityCubePath;
        std::string espPotentialCubePath;
        float espIsovalue = 0.001f;
        float espVminKcal = -45.0f;
        float espVmaxKcal = 45.0f;
        bool  espAutoRange = true;
        bool  espInvert = false;
        int32_t espColormap = 0;
        // 材质名（kMaterialPresets 里的名字），空串 = 自定义/未指定。
        // 存**名字**而非下标：材质表日后增删/重排时，旧文件的下标会静默指向
        // 另一个材质 —— 名字是稳定键（与 UITheme 的 theme 键同一取舍）。
        // v2 可选键：旧文件缺键 → 空串 → 载入时按数值走"自定义"或容差匹配。
        std::string materialName;
        std::array<float, 4> ambient{0.60f, 0.60f, 0.60f, 1.0f};
        std::array<float, 4> diffuse{0.90f, 0.90f, 0.90f, 1.0f};
        std::array<float, 4> specular{0.30f, 0.30f, 0.30f, 1.0f};
        float shininess = 32.0f;
        float metallic = 0.0f;
        float roughness = 0.40f;
        float opacity = 1.0f;
        // 表面绘制模式（v2.19，可选键）：0 实心 / 1 线框 / 2 半透明。
        // 旧文件缺键 → 0，与旧版"体系表面恒定实心"的观感一致。
        uint8_t surfaceDrawMode = 0;
        // 实例级类型范围（v2.19，可选键）：按"分子类型 + 比例/步长"过滤实例，
        // 用于"只渲染一半溶剂分子"这类需求。缺键 → 全部实例（旧行为）。
        // 语义与 engine/rep/representation.h 的 rangeMode 完全一致（持久化层是
        // 纯数据镜像，不重复实现过滤逻辑）。
        uint8_t rangeMode = 0;          // 0 全部 / 1 按比例 / 2 每隔 N 个
        std::string rangeType;          // 分子类型（实例名 "#" 前的 token）；空 = 全部
        float rangeFraction = 1.0f;     // 0..1
        uint32_t rangeStride = 1;
        uint32_t rangeOffset = 0;
    };

    std::vector<Structure> structures;
    // 场景级名字池（与 SceneStructure 的 chainNames_/residueNames_ 一致）：
    // Atom::chainId / residueId 是这两张表的下标；解析器按顺序分配，故 id 稠密。
    std::vector<std::string> chainNames;
    std::vector<std::string> residueNames;
    std::vector<System> systems;
    std::vector<Instance> instances;
    // 内嵌轨迹表（索引即 Instance::trajectory.embeddedIndex）。
    // 链接式保存时为空 → 文件不写 traj 段，字节布局与旧版一致。
    std::vector<EmbeddedTrajectory> embeddedTrajectories;
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
    // 方向光（P6 新增）。以**可选键**加入 v2 的 "scene" 对象：旧 v2 文件缺这些键时
    // 读侧取默认值 → 仍可正常加载（纯增量，不构成破坏性变更，故版本号保持 2）。
    std::array<float, 3> dirLightDir{0.40f, 0.80f, 0.60f};
    std::array<float, 3> dirLightColor{1.0f, 0.98f, 0.95f};
    float dirLightIntensity = 0.35f;
    bool  dirLightEnabled = true;
    bool  shadowsEnabled = true;
    float shadowStrength = 0.65f;
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

    // 场景标注（v2.3，可选 JSON 键 "annotations"；旧文件缺键 → 空）
    Annotations annotations;
};

// —— 读写接口 ——
struct LoadOptions {
    bool strict     = false;   // true=格式错误即抛；false=lenient 跳过并记警告
    bool lazyCoords = false;   // true=坐标段延迟读（大场景内存优化）
};
struct SaveOptions {
    bool writeCoords = true;   // false=仅结构+命令流（坐标留空）
    int  jsonIndent  = 2;
    // true 且 snapshot.embeddedTrajectories 非空时，在 CRC 前追加内嵌轨迹段
    // （链接式保存保持 false → 与旧版文件字节布局一致）。
    bool embedTrajectories = false;
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
