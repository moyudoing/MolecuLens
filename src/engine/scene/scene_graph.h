// =============================================================================
// MolecuLens · engine · scene/scene_graph.h —— 场景图
// 规格来源：主方案 §5.1（SceneNode 树；每帧 update + emit）
// =============================================================================

#pragma once

#include <memory>

#include "engine/scene/scene_node.h"

namespace moleculens::engine {

class SceneGraph {
public:
    SceneGraph() : root_(std::make_unique<SceneNode>()) {}

    SceneNode& root() { return *root_; }
    void update(float dt) { updateNode(*root_, dt); }
    void emit(CommandSink& sink) { emitNode(*root_, sink); }

private:
    static void updateNode(SceneNode& node, float dt) {
        node.update(dt);
        for (auto& c : node.children) {
            updateNode(*c, dt);
        }
    }
    static void emitNode(SceneNode& node, CommandSink& sink) {
        if (node.visible) {
            node.emit(sink);
        }
        for (auto& c : node.children) {
            emitNode(*c, sink);
        }
    }

    std::unique_ptr<SceneNode> root_;
};

} // namespace moleculens::engine
