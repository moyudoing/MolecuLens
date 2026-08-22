// =============================================================================
// MolecuLens · app · controller/script/script_controller.cpp —— 脚本入口（M4）
// =============================================================================

#include "app/controller/script/script_controller.h"

#include "spdlog/spdlog.h"

namespace moleculens::app {

ScriptController::ScriptController(AppServices& svc) : svc_(svc) {
    if (svc_.bus) {
        svc_.bus->subscribe(this);
    }
}

bool ScriptController::runScript(const std::string& source) {
    spdlog::info("[python] script request (M4): {} bytes", source.size());
    if (svc_.bus) {
        svc_.bus->publish({EventType::ScriptRequest, 0});
    }
    return true;
}

bool ScriptController::runFile(const std::string& path) {
    spdlog::info("[python] script file (M4): {}", path);
    return false;
}

void ScriptController::onEvent(const Event&) {}

} // namespace moleculens::app
