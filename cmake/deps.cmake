# =============================================================================
# MolecuLens · cmake/deps.cmake —— 依赖查找
# 规格来源：M0 工程脚手架 §3.1 / 主方案 §14.1
# 依赖：vcpkg manifest（vcpkg.json）→ 本文件 find_package
# =============================================================================

find_package(spdlog CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(glm CONFIG REQUIRED)
find_package(glfw3 CONFIG REQUIRED)
find_package(volk CONFIG REQUIRED)
find_package(VulkanHeaders CONFIG REQUIRED)
find_package(VulkanMemoryAllocator CONFIG REQUIRED)
find_package(imgui CONFIG REQUIRED)
find_package(CLI11 CONFIG REQUIRED)

if(MOLECULENS_BUILD_TESTS)
    find_package(Catch2 CONFIG REQUIRED)
endif()

if(MOLECULENS_BUILD_PYTHON)
    find_package(Python3 COMPONENTS Interpreter Development REQUIRED)
    find_package(pybind11 CONFIG REQUIRED)
endif()

if(MOLECULENS_ENABLE_ZMQ)
    find_package(cppzmq CONFIG REQUIRED)
endif()

# 注意：vcpkg 的 volk 目标名是 volk::volk；
#       vulkan-headers 提供 Vulkan::Headers（vulkan 目录 config）；
#       VMA 提供 GPUOpen::VulkanMemoryAllocator（v3.1）；
#       imgui(docking) 提供 imgui::imgui。
#       验证每个目标存在性，缺失时给出明确错误。
foreach(tgt volk::volk Vulkan::Headers GPUOpen::VulkanMemoryAllocator glfw imgui::imgui
        spdlog::spdlog nlohmann_json::nlohmann_json glm::glm)
    if(NOT TARGET ${tgt})
        message(FATAL_ERROR "Required dependency target '${tgt}' not found. "
                "Check vcpkg.json and rerun: vcpkg install")
    endif()
endforeach()
