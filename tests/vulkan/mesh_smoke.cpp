// =============================================================================
// MolecuLens · tests · vulkan/mesh_smoke.cpp —— TriMesh 渲染冒烟（表面/卡通）
// 校验：单个四边形 TriMesh 是否写入像素（定位 Surface/TriMesh 不显示问题）
// =============================================================================

#include <catch2/catch_test_macros.hpp>

#include <vector>

#include "engine/cmd/cmd.h"
#include "engine/render/renderer.h"
#include "engine/vulkan/offscreen/offscreen_renderer.h"

#include "spdlog/spdlog.h"

using namespace moleculens::engine;

namespace {

void appendQuad(SceneCommandList& out) {
    TriMeshHeader hdr;
    hdr.vertexCount = 4;
    hdr.indexCount = 6;
    std::vector<uint8_t> payload;
    const uint8_t* hdrp = reinterpret_cast<const uint8_t*>(&hdr);
    payload.insert(payload.end(), hdrp, hdrp + sizeof(TriMeshHeader));

    const glm::vec4 white(1.0f, 1.0f, 1.0f, 1.0f);
    const glm::vec3 nrm(0.0f, 0.0f, 1.0f);
    const TriMeshVertex verts[4] = {
        {glm::vec4(-5, -5, 0, 0), glm::vec4(nrm, 0), white},
        {glm::vec4(5, -5, 0, 0), glm::vec4(nrm, 0), white},
        {glm::vec4(5, 5, 0, 0), glm::vec4(nrm, 0), white},
        {glm::vec4(-5, 5, 0, 0), glm::vec4(nrm, 0), white},
    };
    for (const auto& v : verts) {
        const uint8_t* vp = reinterpret_cast<const uint8_t*>(&v);
        payload.insert(payload.end(), vp, vp + sizeof(TriMeshVertex));
    }
    const uint32_t idx[6] = {0, 1, 2, 0, 2, 3};
    const uint8_t* ip = reinterpret_cast<const uint8_t*>(idx);
    payload.insert(payload.end(), ip, ip + sizeof(idx));
    while (payload.size() % 16 != 0) {
        payload.push_back(0);
    }
    out.append(Cmd::TriMesh, payload.data(), static_cast<uint32_t>(payload.size()));
}

} // namespace

TEST_CASE("OffscreenRenderer renders a TriMesh quad", "[vulkan][smoke][mesh]") {
    RenderConfig cfg;
    cfg.debug = false;
    cfg.msaaSamples = 0;
    OffscreenRenderer renderer;
    std::string err;
    if (!renderer.init(cfg, 128, 96, &err)) {
        WARN("Skipping mesh smoke (no Vulkan device): " << err);
        return;
    }

    SceneCommandList list;
    MaterialBlock mat;
    mat.diffuse = glm::vec4(1.0f, 1.0f, 1.0f, 1.0f);
    mat.ambient = glm::vec4(0.5f, 0.5f, 0.5f, 1.0f);
    list.append(Cmd::MaterialBlock, &mat, sizeof(mat));
    appendQuad(list);

    RenderView view;
    view.camera.centerOn({0, 0, 0}, 10.0f);
    view.width = 128;
    view.height = 96;
    renderer.execute(list, view);

    std::vector<uint8_t> pixels;
    REQUIRE(renderer.readbackPixels(pixels, PixelFormat::Rgba8));
    uint32_t nonBg = 0;
    for (size_t i = 0; i < pixels.size(); i += 4) {
        if (pixels[i] > 30 || pixels[i + 1] > 30 || pixels[i + 2] > 30) {
            ++nonBg;
        }
    }
    spdlog::info("[test] mesh smoke nonBg={}", nonBg);
    REQUIRE(nonBg > 500);   // 大四边形占视口大部分
}
