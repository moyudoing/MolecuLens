// =============================================================================
// MolecuLens · engine · style/base_style.h —— 渲染样式的统一契约（接口层）
// 职责：定义渲染函数的统一原型、统一类型别名，声明基础渲染样式接口。
//       本头仅声明，不含实现（实现落在各 style/*/xxx.cpp，如代表
//       representer.h 中 kDrawTable 的分派入口）。
// 规格来源：主方案 §5.7 各类渲染样式 / §5.2（表示·样式分派）
// 关联：style_registry.h 消费本头的 StyleDrawFn 原型，构建分派注册表。
// =============================================================================

#ifndef BASE_STYLE_H
#define BASE_STYLE_H

#include "engine/cmd/cmd.h"            // SceneCommandList
#include "engine/rep/representer.h"    // RepContext（其内已含 representation.h）

namespace moleculens::engine {

// —— 统一渲染函数原型（渲染样式的公共签名）——
// 与 Representer::DrawFn（representer.h）保持同一函数指针形态：
//   输入：RepContext（每任务一份：场景/实例/帧/表示/选择掩码/全局偏移/渲染模式）
//   输出：SceneCommandList（追加设备无关命令段，见 cmd.h 协议）
using StyleDrawFn = void (*)(const RepContext&, SceneCommandList&);

// —— 内置渲染样式入口声明（对应 RepresentationType 各枚举值）——
// 以下即"渲染函数原型"清单；实现分别在各自 style/*/xxx.cpp，声明与
// style/*/*.h 中的同名 draw* 一一对应（重复声明无碍，作为统一契约收拢在此）。
void drawWireframe   (const RepContext&, SceneCommandList&);   // Wireframe
void drawStick       (const RepContext&, SceneCommandList&);   // Stick
void drawBallAndStick(const RepContext&, SceneCommandList&);   // BallAndStick
void drawSpacefill   (const RepContext&, SceneCommandList&);   // Spacefill
void drawVdw         (const RepContext&, SceneCommandList&);   // Vdw（不受 radiusScale 缩放）
void drawCartoon     (const RepContext&, SceneCommandList&);   // Cartoon
void drawSurface     (const RepContext&, SceneCommandList&);   // Surface（vdW/MC 等值面）
void drawSystemSurface(const RepContext&, SceneCommandList&);  // Surface·体系级（G5）
void drawQuickSurf   (const RepContext&, SceneCommandList&);   // QuickSurf（高斯密度面）
void drawSystemQuickSurf(const RepContext&, SceneCommandList&); // QuickSurf·体系级（G5）
void drawLicorice    (const RepContext&, SceneCommandList&);   // Licorice
void drawTube        (const RepContext&, SceneCommandList&);   // Tube
void drawNewCartoon  (const RepContext&, SceneCommandList&);   // NewCartoon
void drawRibbon      (const RepContext&, SceneCommandList&);   // Ribbon
void drawEspSurface  (const RepContext&, SceneCommandList&);   // EspSurface（cube 等值面 + ESP 着色）
void drawPlate       (const RepContext&, SceneCommandList&);   // Plate（每残基/分子一块扁平方板）
void drawDiamond     (const RepContext&, SceneCommandList&);   // Diamond（每残基/分子一颗八面体宝石）
void drawDotCloud    (const RepContext&, SceneCommandList&);   // DotCloud（每原子一个点精灵）

} // namespace moleculens::engine

#endif // BASE_STYLE_H