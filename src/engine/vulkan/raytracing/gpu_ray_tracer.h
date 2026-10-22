// =============================================================================
// MolecuLens · engine · vulkan/raytracing/gpu_ray_tracer.h —— Vulkan KHR GPU 光追
// 规格来源：任务 E（完整 GPU 光追模块）
// 实现：VK_KHR_acceleration_structure（BLAS/TLAS）+ VK_KHR_ray_tracing_pipeline
//       （5 个阶段 + 2 个 hit group）+ SBT + 描述符 + 提交/回读。
// 输入/输出契约与 CPU 路径 engine/raytrace/ray_tracer.h **完全一致**：
//   RtScene / RtCamera / RtLight 进，RGBA8（行序 top→bottom）出。
// 无 RT 设备：init() 失败/available()==false，调用方回退 CPU（绝不加载 RT 函数）。
// =============================================================================

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <glm/glm.hpp>

namespace moleculens::engine {

class VulkanDevice;
struct RtScene;
struct RtCamera;
struct RtLight;

class GpuRayTracer {
public:
    GpuRayTracer();
    ~GpuRayTracer();

    GpuRayTracer(const GpuRayTracer&) = delete;
    GpuRayTracer& operator=(const GpuRayTracer&) = delete;

    // shared != nullptr：用既有设备（GUI 主设备 / CLI 既有离屏设备）；
    // shared == nullptr：自建 headless 设备（CLI/测试无共享设备时）。
    // 幂等：已成功初始化后重复调用直接返回 true。
    bool init(VulkanDevice* shared, std::string* outError = nullptr);

    [[nodiscard]] bool available() const;

    // 渲染一帧（RGBA8，行序 top→bottom；samplesPerPixel 与 CPU 同义）。
    bool render(const RtScene& scene, const RtCamera& camera, const RtLight& light,
                const glm::vec3& backgroundColor, int samplesPerPixel,
                std::vector<uint8_t>& outRgba);

    // 最近一次 render 的度量（日志/报告用）
    struct Stats {
        size_t triangles = 0;      // 三角数量
        size_t primitives = 0;     // 球 + 胶囊数量
        double blasMs = 0.0;       // BLAS 构建（含上传）
        double tlasMs = 0.0;       // TLAS 构建
        double traceMs = 0.0;      // 光线追踪 + 回读
    };
    [[nodiscard]] const Stats& lastStats() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace moleculens::engine
