// =============================================================================
// Moleculens · app · i18n/language.h —— 语言管理模块（界面与渲染模型名的中英双语）
//
// 设计：**以中文源串本身作为键** —— `tr("背景色")` 在英文下返回 "Background"。
//   为什么不另造 key（如 "ui.bgcolor"）：
//     · 不必给每条文案起名、也不必维护 key 与文案的对应关系；
//     · 调用点就是可读的中文，代码不必为了 i18n 变得难读；
//     · 可以**增量补齐**：漏翻时原样返回中文（不崩、不空白），
//       覆盖率用 tools/check_i18n_coverage.py 扫出来，不会静默。
//   代价：同一句文案在两处含义不同时就只能共用一条翻译（本工程无此情况）。
//
// 覆盖范围：
//   · 全部界面文案（面板标签/按钮/提示/菜单）；
//   · **渲染模型名**：样式名（线状/球棍/卡通…）、配色方案（元素 CPK/按链…）
//     —— 引擎侧继续返回中文（style_registry / representation.h），
//     界面用 tr() 包一层即可翻译，引擎无需感知语言。
//   · 材质名保持 VMD 原名（专有名词，中英文界面下都不译）。
// =============================================================================

#pragma once

#include <cstdint>
#include <initializer_list>
#include <string>

namespace moleculens::app::i18n {

enum class Language : uint8_t {
    Zh = 0,   // 中文
    En,       // English
    Count,
};

// 当前语言（默认中文，与历史行为一致）
[[nodiscard]] Language current();
void setLanguage(Language lang);

// 稳定键：settings.json 存 "zh" / "en"。
// 与主题键同一取舍 —— 存字符串而非枚举序号，日后增删语言不会让旧配置指错。
[[nodiscard]] const char* languageKey(Language lang);
[[nodiscard]] bool languageFromKey(const char* key, Language* out);

// 语言自身的名字（下拉用**各自语言**书写：切换语言时用户至少能认出自己的语言）
[[nodiscard]] const char* languageNativeName(Language lang);

// 翻译。zhSource = 中文源串；未收录时**原样返回**（英文界面下会显示中文，
// 由覆盖率工具兜住，不会崩也不会空白）。
[[nodiscard]] const char* tr(const char* zhSource);

} // namespace moleculens::app::i18n

namespace moleculens::app {

// 转发到 i18n::tr，使界面代码可以直接写 `tr("背景色")` —— 面板代码遍布
// app 的多个子命名空间，每处都写 i18n::tr 只会增加噪声。
inline const char* tr(const char* zhSource) { return i18n::tr(zhSource); }

// ImGui 多选项串（以 '\0' 分隔，如 Combo 的 "A\0B\0C\0"）的翻译辅助：
// **逐项** tr() 后用 '\0' 重新连接。
//
// ⚠ 为什么不把整串交给 tr()：`tr(const char*)` 是**指针**查找，而 std::string_view
//   从 const char* 构造时在**第一个 NUL 处就截断** —— 多选项串的键会退化成第一项，
//   永远匹配不上（表现为"整串原样返回"，且不会报错）。
// 用法：`ImGui::Combo("##x", &i, trItems({"当前背景", "白色", "透明"}).c_str())`
// 返回的临时串在整条表达式内有效，可直接取 c_str()。
inline std::string trItems(std::initializer_list<const char*> items) {
    std::string out;
    for (const char* s : items) {
        out += tr(s);
        out.push_back('\0');
    }
    return out;
}

} // namespace moleculens::app
