// =============================================================================
// MolecuLens · engine · style/espsurface/esp_surface.h —— ESP（静电势）映射表面
// 参考效果：GXNU MolStudio / Avogadro 风格 —— 电子密度等值面（默认 ρ=0.001 a.u.，
//   即 vdW 表面）+ ESP 逐顶点连续着色 + 色标范围 + 表面极值点（供视口色标条/标注）。
// 数据来源：Gaussian / Multiwfn 导出的 density cube + ESP cube（见 io/cube/cube_volume.h）。
// 渲染：复用命令流 TriMesh + 逐顶点色（meshSolid / meshBlend），无需新增 Vulkan 协议。
// =============================================================================

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <glm/glm.hpp>

namespace moleculens::engine {

struct RepContext;
class SceneCommandList;

// ESP 色标方案（参考 matplotlib：RWB=bwr_r 语义，低值红=富电子/负电势）
enum class EspColormap : uint8_t {
    Rwb = 0,      // 低=红(负) 中=白 高=蓝(正)  ← 化学惯例
    Bwr,          // 低=蓝 中=白 高=红
    Coolwarm,
    Turbo,
    Viridis,
    Jet,
    Reference,    // 参考截图配色：低=黄 → 绿 → 青 → 蓝 → 紫=高（GXNU MolStudio 风格）
    Count,
};

const char* espColormapName(EspColormap) noexcept;     // 小写标识："rwb" / "bwr" ...
const char* espColormapLabelZh(EspColormap) noexcept;  // 中文显示名（UI 用）
EspColormap espColormapFromName(const std::string&) noexcept;

// t ∈ [0,1] → RGB（sRGB 0..1）。端点：Rwb(0)=红、(1)=蓝。
glm::vec3 espColormapColor(EspColormap, float t);

// Hartree → kcal/mol（1 a.u. = 627.509474 kcal/mol）
inline constexpr float kEspAuToKcal = 627.509474f;
// 电子密度等值面默认阈值（a.u.，对应 vdW 表面）
inline constexpr float kEspDefaultIsolevel = 0.001f;

struct EspSurfaceParams {
    std::string   densityCubePath;   // 电子密度 cube（几何来源；必填）
    std::string   potentialCubePath; // ESP cube（颜色来源；必填）
    float         isolevel = kEspDefaultIsolevel;  // 密度等值面阈值（a.u.）
    float         vminKcal = -45.0f; // 色标下限（kcal/mol；autoRange 时忽略）
    float         vmaxKcal = 45.0f;  // 色标上限（kcal/mol）
    bool          autoRange = true;  // 自动色标范围（发散型取 ±max|v| 的 99.5 分位）
    bool          invert = false;    // 翻转配色方向
    EspColormap   colormap = EspColormap::Rwb;
};

// 表面极值点（局部极大/极小），供色标条与数值标注
struct EspExtremum {
    glm::vec3 position{0.f};   // 世界坐标 Å
    float     valueKcal = 0.f; // 该点 ESP（kcal/mol）
    bool      isMaximum = true;
};

struct EspSurfaceMesh {
    std::vector<glm::vec3> positions;   // Å
    std::vector<glm::vec3> normals;     // 单位外向
    std::vector<glm::vec3> colors;      // 逐顶点 RGB（0..1），长度同 positions
    std::vector<uint32_t>  indices;
    std::vector<EspExtremum> extrema;   // 最多 8 极大 + 8 极小
    // 实际用于着色的色标范围（kcal/mol）
    float vminKcal = -45.0f;
    float vmaxKcal = 45.0f;
};

// 生成 ESP 表面网格（带缓存：两 cube 路径 + 参数 + 文件 mtime/size 为键）。
// 失败返回 false 并写 error（不抛异常）。
bool buildEspSurface(const EspSurfaceParams& params, EspSurfaceMesh& out,
                     std::string* error = nullptr);

// 清空 Cache（测试/热重载用）
void clearEspSurfaceCache();

// 样式入口：RepresentationType::EspSurface 的绘制函数（见 engine/style/base_style.h）。
// 参数来自 ctx.rep（densityCubePath / potentialCubePath / espIsovalue / espVminKcal /
// espVmaxKcal / espAutoRange / espInvert / espColormap，见 rep/representation.h）。
void drawEspSurface(const RepContext& ctx, SceneCommandList& out);

// —— 视口叠加信息（色标条 + 极值标签；GUI 与 PNG 导出共用）——
struct EspOverlay {
    bool                   active = false;
    EspColormap            colormap = EspColormap::Rwb;
    float                  vminKcal = -45.0f;
    float                  vmaxKcal = 45.0f;
    bool                   invert = false;   // 色标方向是否翻转（与着色一致）
    const char*            unit = "kcal/mol";
    std::vector<EspExtremum> extrema;
};

// 每次命令流重建开始时由 Renderer 调用（清空上一帧叠加信息）
void espOverlayReset();
// drawEspSurface 成功后写入（线程安全；多次调用取最后一次）
void espOverlaySet(const EspOverlay& overlay);
// 取当前叠加信息（无 ESP 表面时 active=false）
EspOverlay espOverlaySnapshot();

} // namespace moleculens::engine
