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

// —— 命令类型（0–11，共 12 种）——
enum class Cmd : uint8_t {
    SphereInst    = 0,   // 原子球（空间填充 / 球棍）
    CylinderInst  = 1,   // 键 / 棍圆柱
    LineInst      = 2,   // 线框 / 坐标轴 / 高亮
    TriMesh       = 3,   // 三角网格（卡通 / 表面 / 导出降级）
    Label         = 4,   // billboard 文字
    MaterialBlock = 5,   // 材质状态
    Transform     = 6,   // 矩阵变换
    PushMatrix    = 7,   // 变换栈压栈（无载荷）
    PopMatrix     = 8,   // 变换栈弹栈（无载荷）
    ClipPlane     = 9,   // 裁剪面
    SetBlend      = 10,  // 混合开关
    SetDepthWrite = 11,  // 深度写开关
    Count         = 12,
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

// 标签：固定 48B + 字符串池文本（strLen 字节）
struct LabelInst {
    glm::vec4  pos;         // xyz=锚点(Å)，w=保留
    glm::vec4  color;       // RGBA
    float      size;        // 字号（屏幕空间像素）
    uint32_t   strOffset;   // 相对字符串池起始的字节偏移
    uint32_t   strLen;      // 文本字节数（不含终止符）
    uint32_t   pad;         // 0
};
static_assert(sizeof(LabelInst) == 48);

// 材质块：64B
struct MaterialBlock {
    glm::vec4  ambient;     // 环境色
    glm::vec4  diffuse;     // 漫反射色
    glm::vec4  specular;    // 高光色
    float      shininess;   // 光泽度
    float      metallic;    // 金属度（PBR 扩展）
    float      roughness;   // 粗糙度（PBR 扩展）
    float      opacity;     // 不透明度（<1 进入透明通道）
};
static_assert(sizeof(MaterialBlock) == 64);

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

class SceneCommandList {
public:
    void   append(Cmd code, const void* payload, uint32_t bytes);
    uint32_t appendString(std::string_view text);   // 追加到字符串池，返回偏移

    bool     empty() const;
    uint32_t size() const;                          // 命令池字节数
    uint64_t serial() const;                        // 重建即递增（缓存键，P5）

    bool first(CommandIterator&) const;
    bool next(CommandIterator&) const;

    // 序列化 / 反序列化（配合 .mlscene 命令流段与 RPC 传帧）
    static void writeTo(ByteSink& out, const SceneCommandList&);
    static bool readFrom(ByteSource& in, SceneCommandList& out);

private:
    friend class CommandIterator;
    std::vector<uint8_t> pool_;     // 16B 对齐内存池
    std::vector<uint8_t> strings_;  // 字符串池（UTF-8）
    uint64_t serial_ = 0;
};

// —— 命令流序列化字节布局（.mlscene 命令流段 / RPC 传帧共用）——
// [0]    u64 commandCount         命令条数
// [8]    u64 poolSize             命令池字节数
// [16]   u8  pool[poolSize]       命令池（每条 = CmdHeader 8B + 载荷，整体 16B 对齐）
//        u64 stringPoolSize       字符串池字节数
//        u8  stringPool[...]      UTF-8（Label.strOffset 相对其起始）
//        尾部按 16B 对齐填充

} // namespace moleculens::engine
