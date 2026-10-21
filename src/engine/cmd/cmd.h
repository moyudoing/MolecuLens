// =============================================================================
// MolecuLens · engine · cmd.h —— 命令流协议
// 规格来源：主方案 §5.3 / 附录 F；状态：协议定稿 v1（本文档为唯一权威）
// 说明：引擎层"设备无关命令流"的唯一协议定义；所有后端（Vulkan 实时/离屏、
//       OBJ/POV/STL/MLScene 导出）以此协议消费命令。
// 字节序：little-endian；每条命令 16B 对齐；不足处显式 pad 填充。
// 依赖：ByteSink/ByteSource 定义于本头；Result 定义于 entity/result.h。
// 独立编译约束（主方案 §3.5.3）：只依赖 <cstdint>/<glm> 与标准库。
// =============================================================================

#pragma once

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <string_view>
#include <vector>
#include <glm/vec2.hpp>
#include <glm/vec4.hpp>
#include <glm/mat4x4.hpp>

#include "entity/result.h"

namespace moleculens::engine {

inline constexpr uint32_t kCmdProtocolVersion = 1;        // 协议版本
inline constexpr uint32_t kCmdAlign           = 16;       // 载荷 16B 对齐
inline constexpr uint32_t kCmdMaxPayload      = 1u << 24; // 单命令载荷上限（16 MiB）

// —— 命令类型（0–12，共 13 种）——
enum class Cmd : uint8_t {
    SphereInst    = 0,   // 原子球（空间填充 / 球棍）
    CylinderInst  = 1,   // 键 / 棍圆柱
    LineInst      = 2,   // 线框 / 坐标轴 / 高亮
    TriMesh       = 3,   // 三角网格（卡通 / 表面 / 导出降级）
    Label         = 4,   // 保留：v1 协议槽位（Label 表示已移除，不再发射/消费）
    MaterialBlock = 5,   // 材质状态
    Transform     = 6,   // 矩阵变换
    PushMatrix    = 7,   // 变换栈压栈（无载荷）
    PopMatrix     = 8,   // 变换栈弹栈（无载荷）
    ClipPlane     = 9,   // 裁剪面
    SetBlend      = 10,  // 混合开关
    SetDepthWrite = 11,  // 深度写开关
    RenderMode    = 12,  // 体系渲染模式（实心/线框/透明/点）→ 后端切换 VkPipeline
    Count         = 13,
};

// —— 命令头（8B）——
struct CmdHeader {
    Cmd      code;    // 命令类型
    uint8_t  pad[3];  // 保留，置 0
    uint32_t size;    // 载荷字节数（不含 CmdHeader；无载荷命令为 0）
};
static_assert(sizeof(CmdHeader) == 8, "CmdHeader must be 8 bytes");

// —— 载荷结构（全部 16B 对齐；static_assert 约束布局，防止实现期漂移）——

// 原子球：48B（stride = 16×3，满足 GPU 实例属性对齐）
struct SphereInst {
    glm::vec4  posRadius;   // xyz=中心(Å)，w=半径
    glm::vec4  color;       // RGBA（sRGB 预计算，alpha 供透明通道）
    glm::uvec2 pickId;      // 拾取 ID（回查 CoordsSoA::atomIds）
    glm::uvec2 pad;         // 0
};
static_assert(sizeof(SphereInst) == 48);

// 键圆柱：64B
struct CylinderInst {
    glm::vec4  aPos;        // xyz=端点A(Å)，w=保留
    glm::vec4  bPos;        // xyz=端点B(Å)，w=保留
    float      radius;      // 半径
    float      pad;         // 0
    glm::vec4  color;       // RGBA
    glm::uvec2 pickId_pad;  // x=拾取 ID，y=0
};
static_assert(sizeof(CylinderInst) == 64);

// 线段：48B
struct LineInst {
    glm::vec4  aPos;        // xyz=端点A(Å)，w=保留
    glm::vec4  bPos;        // xyz=端点B(Å)，w=保留
    glm::vec4  color;       // RGBA
};
static_assert(sizeof(LineInst) == 48);

// 三角网格：载荷 = 头(8B) + 顶点流 + 索引流（尾部 16B 对齐）
struct TriMeshHeader {
    uint32_t vertexCount;   // 顶点数
    uint32_t indexCount;    // 索引数
};
static_assert(sizeof(TriMeshHeader) == 8);

// 顶点：48B/顶点（12 floats，16B 对齐）
struct TriMeshVertex {
    glm::vec4  pos;         // xyz=位置(Å)，w=保留
    glm::vec4  normal;      // xyz=法线（单位），w=保留
    glm::vec4  color;       // RGBA
};
static_assert(sizeof(TriMeshVertex) == 48);
// 索引流：uint32_t[indexCount]

// 材质块：80B（v2.1 起由 64B 扩为 80B，新增 shadowMask + 3 个对齐填充）
//
// ⚠ 每个字段都带**默认值**（与材质库的 Opaque 一致，见 render_parameter.h）。
//   原因：`MaterialBlock mat;` 这种声明（测试与手写命令流里很常见）在无默认值时会
//   留下**未初始化**的 opacity/metallic/roughness。而 opacity 现在决定**选哪条管线**
//   （<1 → 混合管线，见 draw_geometry.cpp 的 drawRun），未初始化的垃圾值会让同一个
//   场景在不同次运行里走不同管线 —— 症状是冒烟测试随机失败、且看起来与改动无关。
//   给默认值后，忘写字段得到的是可用材质，而不是未定义行为。
struct MaterialBlock {
    glm::vec4  ambient{0.60f, 0.60f, 0.60f, 1.0f};   // 环境色
    glm::vec4  diffuse{0.90f, 0.90f, 0.90f, 1.0f};   // 漫反射色
    glm::vec4  specular{0.30f, 0.30f, 0.30f, 1.0f};  // 高光色
    float      shininess = 32.0f;   // 光泽度
    float      metallic = 0.0f;     // 金属度（PBR 扩展）
    float      roughness = 0.40f;   // 粗糙度（PBR 扩展）
    float      opacity = 1.0f;      // 不透明度（<1 进入透明通道）
    // 阴影接收开关（0 = 不接收动态阴影，1 = 接收）。
    // 半透明表面（ESP/等值面/快速表面）用 0：否则壳内的球棍会把影子投到壳上，
    // 形成与分子同形的黑色"幽灵"——用户反馈的"表面光线透视问题"就是它。
    // 放在这里而不是靠 opacity 判断：透明网格的 opacity 已被烘焙进顶点 alpha，
    // 推送的 MaterialBlock.opacity 恒为 1，片元里无法再区分透明与不透明。
    float      shadowMask = 1.0f;
    float      pad[3] = {0.0f, 0.0f, 0.0f};
};
static_assert(sizeof(MaterialBlock) == 80);

// 变换：64B，列主序
using TransformMat = glm::mat4;

// 裁剪面：16B，平面方程 ax+by+cz+d=0
struct ClipPlane {
    glm::vec4  eq;
};
static_assert(sizeof(ClipPlane) == 16);

// 状态开关：4B
struct StateFlag {
    uint8_t    on;          // 0=关，1=开
    uint8_t    pad[3];
};
static_assert(sizeof(StateFlag) == 4);

// 体系渲染模式：4B（与 SystemRenderMode 对应：0 实心/1 线框/2 透明/3 点）
struct RenderModeInst {
    uint8_t    mode;        // 0 实心 / 1 线框 / 2 透明 / 3 点
    uint8_t    pad[3];      // 0
};
static_assert(sizeof(RenderModeInst) == 4);

// —— 字节流基础设施（序列化用）——

class ByteSink {
public:
    explicit ByteSink(std::vector<uint8_t>& out) : out_(out) {}

    void write(const void* data, size_t bytes) {
        const auto* p = static_cast<const uint8_t*>(data);
        out_.insert(out_.end(), p, p + bytes);
    }
    template <typename T>
    void writeValue(const T& v) {
        write(&v, sizeof(T));
    }
    void alignTo(size_t alignment) {
        while (out_.size() % alignment != 0) {
            out_.push_back(0);
        }
    }

private:
    std::vector<uint8_t>& out_;
};

class ByteSource {
public:
    ByteSource(const uint8_t* data, size_t size) : data_(data), size_(size) {}

    bool read(void* dst, size_t bytes) {
        if (pos_ + bytes > size_) {
            return false;
        }
        memcpy(dst, data_ + pos_, bytes);
        pos_ += bytes;
        return true;
    }
    template <typename T>
    bool readValue(T& v) {
        return read(&v, sizeof(T));
    }
    bool skip(size_t bytes) {
        if (pos_ + bytes > size_) {
            return false;
        }
        pos_ += bytes;
        return true;
    }
    void alignTo(size_t alignment) {
        while (pos_ % alignment != 0) {
            ++pos_;
        }
    }
    size_t position() const { return pos_; }
    size_t remaining() const { return size_ - pos_; }

private:
    const uint8_t* data_;
    size_t size_;
    size_t pos_ = 0;
};

// —— 命令流容器 ——

class SceneCommandList;   // 前向声明（CommandIterator 引用）

// 命令迭代器（方法体定义于 command_list.cpp，SceneCommandList 完整定义之后）
class CommandIterator {
public:
    explicit CommandIterator(const SceneCommandList& list);

    bool done() const;
    Cmd code() const;
    const uint8_t* payload() const;
    uint32_t payloadSize() const;
    void next();

private:
    void readHeader();

    const SceneCommandList* list_ = nullptr;   // 指针成员（支持赋值，first() 复用）
    CmdHeader header_{};
    const uint8_t* payload_ = nullptr;
    size_t pos_ = 0;
    bool done_ = true;
};

// 位置相关命令判定（坐标重放用）：载荷里烘焙了坐标的命令。
// 变换栈/材质/渲染模式等状态命令不在此列——它们不含坐标，重放时无须重写。
inline bool isCoordCommand(Cmd code) {
    switch (code) {
        case Cmd::SphereInst:
        case Cmd::CylinderInst:
        case Cmd::LineInst:
        case Cmd::TriMesh:
            return true;
        default:
            return false;
    }
}

class SceneCommandList {
public:
    void   append(Cmd code, const void* payload, uint32_t bytes);
    uint32_t appendString(std::string_view text);   // 追加到字符串池，返回偏移

    // 追加三角网格（卡通 / 表面 / QuickSurf）。
    //
    // **自动按 kCmdMaxPayload 切块** —— 必须的原因：单命令载荷超上限时
    // CommandIterator::readHeader 会置 done_ 直接结束遍历，于是整条命令流在
    // 该命令处**被静默截断**：解码得到 0 个网格、commandStats 报 0、几何完全
    // 不可见，而命令池里明明有几十 MB 数据。实测 md.gro 的整体系 vdW/QuickSurf
    // 表面单块就 19–36 MB（> 16 MiB 上限），正是撞在这条上。
    // 多块对绘制无害：网格绘制按渲染模式把所有块合并成一次绘制（见 draw_geometry.cpp）。
    void appendTriMesh(const std::vector<TriMeshVertex>& verts,
                       const std::vector<uint32_t>& indices);

    bool     empty() const;
    uint32_t size() const;                          // 命令池字节数
    // 修订号：**每次重建必须单调递增**。它是后端解码缓存与视图层重绘门控的键：
    //   后端 decodeCommandList 以 (命令流对象, serial) 为键跳过重复解码；
    //   MolScene 用它与 lastSerial_ 比较决定是否重绘。
    // ⚠ 因此绝不能让 serial **回退或复位** —— 一旦两次不同的内容拿到同一个
    //   serial，后端会误判"缓存命中"而继续用旧的绘制列表（表现为：打开文件后
    //   界面正常、视口却一直是空白/旧内容）。见 Renderer::rebuildCommandList。
    uint64_t serial() const;                        // 重建即递增（缓存键，P5）
    void     bumpSerial() { ++serial_; }            // 重建完成时调用（Renderer）
    // 显式设值：整表换入（move 自一个新建的空列表）时必须先把旧 serial 搬过来，
    // 否则新建列表的 serial_(0) 会随 move 覆盖掉，bump 后恒等于 1。
    void     setSerial(uint64_t s) { serial_ = s; }

    bool first(CommandIterator&) const;
    bool next(CommandIterator&) const;

    // —— 坐标重放（帧增量上传，主方案 §5.8.2 / P4）——
    //
    // 帧切（仅 DIRTY_COORDS）时坐标变了、命令序列不变。重建整条命令流要重新
    // 枚举所有 (表示 × 实例)、重算颜色/半径、并重新分配整块命令池；重放只把
    // 坐标类命令的载荷**写回原偏移**，池大小与命令序列逐字节保持。
    //
    // 用法：全量重建时 recordReplay(true) 边生成边记下每条坐标命令的
    //       池偏移/长度/类型；之后帧切调 beginReplay() → 再次生成（样式代码
    //       无须改动，append 按游标改道）→ endReplay() 校验。
    //
    // 安全网：重放期间任何一条 append 与记录不一致（类型/长度不符、条数超界）
    //         即置失败；endReplay() 返回 false，调用方回退到全量重建。
    struct ReplayEntry {
        uint32_t offset;   // 载荷起始在 pool_ 内的绝对偏移
        uint32_t size;     // 载荷字节数
        Cmd      code;
    };

    // 全量重建期间：开始/结束记录坐标命令偏移
    void recordReplay(bool on);
    [[nodiscard]] const std::vector<ReplayEntry>& replayPlan() const { return replay_; }
    // 命令流中含 TriMesh（表面/卡通/QuickSurf）——其顶点数随坐标变化，无法重放
    [[nodiscard]] bool hasMeshCommands() const { return hasMesh_; }

    // allowMesh = true 时允许重放含 TriMesh 的命令流（仅限"顶点/索引数不随坐标
    // 变化"的样式，如 Diamond/Plate/卡通；Surface/QuickSurf 的 MC 拓扑会变，
    // 尺寸不符时 append 会置 replayFailed_，调用方回退全量重建）。
    bool beginReplay(bool allowMesh = false);   // 进入重放模式（计划非空且不含 TriMesh）
    bool endReplay();     // 校验重放完整；成功则 bumpSerial 并返回 true

    // 序列化 / 反序列化（配合 .mlscene 命令流段与 RPC 传帧）
    static void writeTo(ByteSink& out, const SceneCommandList&);
    static bool readFrom(ByteSource& in, SceneCommandList& out);

private:
    friend class CommandIterator;
    std::vector<uint8_t> pool_;     // 16B 对齐内存池
    std::vector<uint8_t> strings_;  // 字符串池（UTF-8）
    std::vector<ReplayEntry> replay_;   // 坐标命令偏移记录（见上方说明）
    size_t replayCursor_ = 0;           // 重放游标（replay_ 下标）
    bool   recording_ = false;          // 记录模式
    bool   replaying_ = false;          // 重放模式
    bool   replayFailed_ = false;       // 重放期间发现不一致
    bool   hasMesh_ = false;            // 曾发射 TriMesh（禁用重放）
    uint64_t serial_ = 0;
};

// —— 命令流序列化字节布局（.mlscene 命令流段 / RPC 传帧共用）——
// [0]    u64 commandCount         命令条数
// [8]    u64 poolSize             命令池字节数
// [16]   u8  pool[poolSize]       命令池（每条 = CmdHeader 8B + 载荷，整体 16B 对齐）
//        u64 stringPoolSize       字符串池字节数
//        u8  stringPool[...]      UTF-8（字符串偏移相对其起始，见 appendString）
//        尾部按 16B 对齐填充

} // namespace moleculens::engine
