// =============================================================================
// MolecuLens · plugins/plugin.h —— 插件 ABI
// 规格来源：主方案 §10.1（MOLECULENS_PLUGIN_HEAD；IPlugin；extern "C" 导出：
//           init/register/fini；双模式：静态 + dlopen）
// =============================================================================

#pragma once

#include <cstdint>

#define MOLECULENS_PLUGIN_ABI_VERSION 1

// 插件头字段（主方案 §10.1）
#define MOLECULENS_PLUGIN_HEAD \
    int         abi;          /* = MOLECULENS_PLUGIN_ABI_VERSION */ \
    const char* type;         /* "file reader" | "file writer" | "render backend" */ \
    const char* name;         /* "pdb" | "gro" | "obj" | ... */ \
    const char* prettyname;   \
    int major; int minor;

namespace moleculens::plugin {

struct IPlugin {
    MOLECULENS_PLUGIN_HEAD
};

// 注册回调（register_cb：逐个 cb(app, &plugin)；校验 abi/type/name/去重）
using RegisterCb = int (*)(void* app, const IPlugin* plugin);

// 插件导出符号（extern "C"）：
//   extern "C" int init(void);
//   extern "C" int register_plugin(void* app, moleculens::plugin::RegisterCb cb);
//   extern "C" int fini(void);

// 静态插件注册宏（编译期 MOLECULENS_REGISTER_ALL 展开；主方案 §10.3）
#if defined(MOLECULENS_PLUGIN_STATIC)
#define MOLECULENS_REGISTER_PLUGIN(pluginPtr) \
    { (pluginPtr) }
#endif

} // namespace moleculens::plugin
