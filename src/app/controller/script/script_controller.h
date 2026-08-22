// =============================================================================
// MolecuLens · app · controller/script/script_controller.h —— 脚本控制器
// 规格来源：主方案 §11（Python/RPC 统一入口；M4 落地，M0 空壳）
// =============================================================================

#pragma once

#include <string>

#include "app/events/event_bus.h"
#include "app/shell/application.h"

namespace moleculens::app {

class ScriptController : public Subscriber {
public:
    explicit ScriptController(AppServices& svc);

    // 执行脚本（M4：嵌入式 Python；当前仅事件转发）
    bool runScript(const std::string& source);
    bool runFile(const std::string& path);

    void onEvent(const Event&) override;

private:
    AppServices& svc_;
};

} // namespace moleculens::app
