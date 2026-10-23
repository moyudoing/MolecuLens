# =============================================================================
# MolecuLens · cmake/packaging.cmake —— 安装规则与发布打包
# -----------------------------------------------------------------------------
# 目标：`cmake --install <build> --prefix <stage>` 产出一份**可直接运行**的
#       绿色目录（exe + DLL + font/ + data/ + 许可），再由 installer/moleculens.nsi
#       用 NSIS 3 编译成安装程序；`cpack -G ZIP` 产出免安装压缩包。
#
# 为什么不用 install(TARGETS ... RUNTIME_DEPENDENCIES)：本项目的依赖全部来自
#   vcpkg（glfw3/spdlog/fmt 为 shared），CMake 的依赖解析会把 system32 里的
#   DLL 一并复制进来，导致安装包体积和许可都不可控。这里显式列出运行库，
#   再加 MSVC app-local CRT（微软允许 app-local 再分发），体积最小且可审计。
# =============================================================================

set(MOLECULENS_PRODUCT_VERSION "2.19.0" CACHE STRING
    "对外发布版本号（安装包文件名、注册表 DisplayVersion）")

option(MOLECULENS_INSTALL_DEMO_DATA "安装示例分子数据（data/ 下的演示文件）" ON)

# —— 版本一致性检查：CMake 工程版本（0.1.0）与对外版本分离，但必须可解析 ——
string(REGEX MATCH "^[0-9]+\\.[0-9]+\\.[0-9]+$" _mole_ver_ok
       "${MOLECULENS_PRODUCT_VERSION}")
if(NOT _mole_ver_ok)
    message(FATAL_ERROR
        "MOLECULENS_PRODUCT_VERSION 必须是 MAJOR.MINOR.PATCH，当前为 "
        "'${MOLECULENS_PRODUCT_VERSION}'")
endif()

if(CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
    # 安装到构建树下的 stage/，避免污染 C:/Program Files
    set(CMAKE_INSTALL_PREFIX "${CMAKE_BINARY_DIR}/stage" CACHE PATH
        "Install staging directory" FORCE)
endif()

# ————————————————————————————————————————————————————————————————
# 1. 可执行文件（GUI + CLI 同目录：CLI 与 GUI 共用 font/ 资源解析规则）
# ————————————————————————————————————————————————————————————————
install(TARGETS moleculens_app
        RUNTIME DESTINATION . COMPONENT Runtime)
if(TARGET moleculens-cli)
    install(TARGETS moleculens-cli
            RUNTIME DESTINATION . COMPONENT Runtime)
endif()

# ————————————————————————————————————————————————————————————————
# 2. 字体（Noto Sans SC 子集，SIL OFL 1.1；OFL.txt 必须随字体分发）
# ————————————————————————————————————————————————————————————————
if(EXISTS "${MOLECULENS_FONT_SRC}")
    install(FILES "${MOLECULENS_FONT_SRC}" "${CMAKE_SOURCE_DIR}/data/font/OFL.txt"
            DESTINATION font COMPONENT Runtime)
else()
    message(WARNING "字体缺失：${MOLECULENS_FONT_SRC}；安装包内将回退系统字体")
endif()

# ————————————————————————————————————————————————————————————————
# 3. 文档与许可（安装根目录，安装器直接展示 LICENSE）
# ————————————————————————————————————————————————————————————————
foreach(_doc LICENSE NOTICE THIRD_PARTY_NOTICES.md README.zh-CN.md CHANGELOG.md)
    if(EXISTS "${CMAKE_SOURCE_DIR}/${_doc}")
        install(FILES "${CMAKE_SOURCE_DIR}/${_doc}" DESTINATION . COMPONENT Runtime)
    endif()
endforeach()

# 中文文件名在安装包里改成 ASCII，避免旧版解压工具/编码问题
if(EXISTS "${CMAKE_SOURCE_DIR}/docs/03-使用说明.md")
    install(FILES "${CMAKE_SOURCE_DIR}/docs/03-使用说明.md"
            DESTINATION docs RENAME UserGuide-zh.md COMPONENT Runtime)
endif()

# ————————————————————————————————————————————————————————————————
# 4. 示例数据（只挑体积合理的演示文件；不打包 image.png / md.xtc 等大文件）
# ————————————————————————————————————————————————————————————————
if(MOLECULENS_INSTALL_DEMO_DATA)
    set(_mole_demo_files "")
    foreach(_demo
            "data/EPI.pdb"
            "data/glycine.pdb"
            "data/md.gro"
            "data/EPI.gro"
            "data/API.gro"
            "data/(+)-Epicatechin.gro"
            "data/(+) - Epicatechin.sdf"
            "data/traj_multiframe.pdb"
            "data/traj_xtc_10f10a.xtc"
            "data/README.md")
        if(EXISTS "${CMAKE_SOURCE_DIR}/${_demo}")
            list(APPEND _mole_demo_files "${CMAKE_SOURCE_DIR}/${_demo}")
        endif()
    endforeach()
    if(_mole_demo_files)
        install(FILES ${_mole_demo_files} DESTINATION data COMPONENT DemoData)
    endif()
endif()

# ————————————————————————————————————————————————————————————————
# 5. 运行库：vcpkg shared DLL（glfw3 / spdlog / fmt）
#    IMPORTED_RUNTIME_ARTIFACTS 要求目标确实是 IMPORTED + SHARED，
#    静态库目标（imgui/volk/lodepng/glm）不参与。
# ————————————————————————————————————————————————————————————————
if(WIN32)
    set(_mole_shared_imports "")
    foreach(_tgt glfw spdlog::spdlog fmt::fmt)
        if(TARGET ${_tgt})
            get_target_property(_imported ${_tgt} IMPORTED)
            get_target_property(_type ${_tgt} TYPE)
            if(_imported AND _type STREQUAL "SHARED_LIBRARY")
                list(APPEND _mole_shared_imports ${_tgt})
            endif()
        endif()
    endforeach()
    if(_mole_shared_imports)
        install(IMPORTED_RUNTIME_ARTIFACTS ${_mole_shared_imports}
                RUNTIME DESTINATION . COMPONENT Runtime)
    endif()

    # 兜底：若某个依赖的导入目标名变了，按 vcpkg 安装目录补齐已知运行库
    set(_mole_vcpkg_bin "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/bin")
    if(NOT VCPKG_INSTALLED_DIR)
        set(_mole_vcpkg_bin "${CMAKE_BINARY_DIR}/vcpkg_installed/${VCPKG_TARGET_TRIPLET}/bin")
    endif()
    if(EXISTS "${_mole_vcpkg_bin}")
        set(_mole_extra_dlls "")
        foreach(_dll glfw3.dll spdlog.dll fmt.dll)
            if(EXISTS "${_mole_vcpkg_bin}/${_dll}")
                # 已通过 IMPORTED_RUNTIME_ARTIFACTS 安装的会重复，安装同文件是幂等的
                list(APPEND _mole_extra_dlls "${_mole_vcpkg_bin}/${_dll}")
            endif()
        endforeach()
        if(_mole_extra_dlls)
            install(FILES ${_mole_extra_dlls} DESTINATION . COMPONENT Runtime)
        endif()
    endif()
endif()

# ————————————————————————————————————————————————————————————————
# 6. MSVC app-local CRT：目标机未装 VC++ 运行库时也能直接运行
#    （App-local 部署是微软文档允许的再分发方式；不写注册表、不影响系统）
# ————————————————————————————————————————————————————————————————
if(MSVC)
    set(_mole_crt_dir "")
    # 统一成正斜杠路径：混用 "\" 与 "/" 会让生成的 cmake_install.cmake 出现
    # "...35112\/x64/..." 这样的拼接，CMake 会报 dev warning（虽不影响安装）
    set(_mole_redist_root "")
    if(DEFINED ENV{VCToolsRedistDir} AND EXISTS "$ENV{VCToolsRedistDir}")
        file(TO_CMAKE_PATH "$ENV{VCToolsRedistDir}" _mole_redist_root)
    else()
        # .../VC/Tools/MSVC/<ver>/bin/Hostx64/x64/cl.exe → .../VC
        get_filename_component(_cl "${CMAKE_CXX_COMPILER}" REALPATH)
        foreach(_i RANGE 5)
            get_filename_component(_cl "${_cl}" DIRECTORY)
        endforeach()
        if(EXISTS "${_cl}/Redist/MSVC")
            file(TO_CMAKE_PATH "${_cl}/Redist/MSVC" _mole_redist_root)
        endif()
    endif()
    string(REGEX REPLACE "/+$" "" _mole_redist_root "${_mole_redist_root}")
    if(_mole_redist_root AND EXISTS "${_mole_redist_root}")
        file(GLOB _mole_crt_candidates
             "${_mole_redist_root}/*/x64/Microsoft.VC14*.CRT")
    endif()
    if(_mole_crt_candidates)
        list(SORT _mole_crt_candidates)
        list(GET _mole_crt_candidates -1 _mole_crt_dir)
        file(GLOB _mole_crt_dlls "${_mole_crt_dir}/*.dll")
        if(_mole_crt_dlls)
            install(FILES ${_mole_crt_dlls} DESTINATION . COMPONENT Runtime)
            message(STATUS "MolecuLens packaging: app-local CRT from ${_mole_crt_dir}")
        endif()
    else()
        message(WARNING
            "未找到 MSVC 可再发行 CRT（VCToolsRedistDir 未设置）；"
            "安装包将依赖目标机已安装 VC++ 2015-2022 Redistributable。"
            "请在 VS 开发者命令提示符下 configure，或设置 VCToolsRedistDir。")
    endif()
endif()

# ————————————————————————————————————————————————————————————————
# 7. CPack：免安装 ZIP（NSIS 安装程序由 installer/moleculens.nsi 单独编译）
# ————————————————————————————————————————————————————————————————
set(CPACK_GENERATOR "ZIP")
set(CPACK_PACKAGE_NAME "MolecuLens")
set(CPACK_PACKAGE_VENDOR "MolecuLens Project")
set(CPACK_PACKAGE_VERSION "${MOLECULENS_PRODUCT_VERSION}")
set(CPACK_PACKAGE_FILE_NAME
    "MolecuLens-${MOLECULENS_PRODUCT_VERSION}-win64-portable")
set(CPACK_PACKAGE_INSTALL_DIRECTORY "MolecuLens")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY
    "MolecuLens 分子结构与轨迹显示引擎（Windows x64）")
set(CPACK_PACKAGE_CONTACT "https://github.com/moyudoing/MolecuLens")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_SOURCE_DIR}/LICENSE")
include(CPack)
