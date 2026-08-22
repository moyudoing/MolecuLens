// =============================================================================
// MolecuLens · engine · scene/scene_node.h —— 场景图节点
// 规格来源：主方案 §5.1（节点只承载 变换/可见性/绘制回调，不承载分子几何；
//           覆盖物/标签/脚本几何写命令流；内置 Axes/Highlight/BBox/Label 节点）
// =============================================================================

#pragma once

#include <memory>
#include <vector>

#include "entity/scene/transform.h"

namespace moleculens::engine {

class SceneCommandList;

// 命令流消费者（SceneNode::emit 的目标抽象）
class CommandSink {
public:
    virtual ~CommandSink() = default;
    virtual SceneCommandList& list() = 0;
};

class SceneNode {
public:
    virtual ~SceneNode() = default;

    entity::Transform tm;                       // 合成变换 GT·S·R·CT
    bool visible = true;
    SceneNode* parent = nullptr;
    std::vector<std::unique_ptr<SceneNode>> children;

    virtual void update(float /*dt*/) {}        // 每帧：变换合成 + 动画
    virtual void emit(CommandSink&) {}          // 覆盖物/标签/脚本几何写命令流
    void markDirty() { dirty_ = true; }

    void addChild(std::unique_ptr<SceneNode> child) {
        child->parent = this;
        children.push_back(std::move(child));
    }

protected:
    bool dirty_ = false;
};

} // namespace moleculens::engine
