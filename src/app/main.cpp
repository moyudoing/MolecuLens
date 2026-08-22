// =============================================================================
// MolecuLens · app · main.cpp —— 入口（M0 落地）
// 规格来源：M0 工程脚手架 §5 app/main；主方案 §14.5 崩溃处理
// =============================================================================

#include <cstdlib>
#include <exception>
#include <iostream>
#include <string>

#include "app/shell/application.h"
#include "spdlog/spdlog.h"

namespace {

// 崩溃处理（主方案 §14.5：std::set_terminate + 退出前 flush 日志写 crash 摘要）
void onTerminate() {
    FILE* f = nullptr;
    fopen_s(&f, "terminate.txt", "w");
    if (f) {
        if (std::current_exception()) {
            try {
                std::rethrow_exception(std::current_exception());
            } catch (const std::exception& e) {
                std::fprintf(f, "exception: %s\n", e.what());
            } catch (...) {
                std::fprintf(f, "exception: unknown\n");
            }
        } else {
            std::fprintf(f, "terminate (no exception)\n");
        }
        std::fclose(f);
    }
    spdlog::critical("[app] terminate");
    spdlog::shutdown();
    std::abort();
}

} // namespace

int main(int argc, char** argv) {
    std::set_terminate(onTerminate);
    try {
        moleculens::app::Application app;
        return app.run(argc, argv);
    } catch (const std::exception& e) {
        spdlog::critical("[app] fatal: {}", e.what());
        return 1;
    }
}

#ifdef _WIN32
// WIN32 子系统入口（无控制台窗口；转发到标准 main）
#include <windows.h>
int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
    return main(__argc, __argv);
}
#endif
