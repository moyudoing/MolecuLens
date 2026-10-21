// =============================================================================
// MolecuLens · io · cube/cube_parser.h —— Gaussian cube 文件读取器（IFileReader 插件）
// 规格来源：Gaussian cubegen cube 格式（见 cube_volume.h 头注释）+ _esp/DESIGN.md
//   · 体积 + 头部原子的解析由 entity::parseCubeFile（cube_parser.cpp）完成，
//     CubeVolume 供 ESP 表面表示使用；
//   · 本类只负责把 cube 头部的原子记录转成 SceneStructure（球棍/其它表示可直接
//     渲染），体积数据不进入场景（避免把 MB 级标量场挂在场景里）；
//   · 识别：扩展名 .cube/.cub 优先；无扩展名时按第 3 行（原子数 + 原点）与
//     第 4–6 行（网格点数 + 轴向量）的数字特征嗅探。
// =============================================================================

#pragma once

#include "io/registry/ifile_reader.h"

namespace moleculens::entity {

class CubeFileReader : public IFileReader {
public:
    const char* name() const override { return "cube"; }
    std::vector<std::string> extensions() const override { return {"cube", "cub"}; }
    bool canOpen(const std::filesystem::path&) const override;
    Result<ParseResult> parse(const std::filesystem::path&, const ParseOptions&) override;
};

} // namespace moleculens::entity
