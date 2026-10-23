// =============================================================================
// MolecuLens v2 · app/workbench/analysis/xlsx_writer.h —— 最小 XLSX 写出
//
// 为什么自己写：工程依赖里没有 zip / OpenXML 库，而为"导出一张统计表"引入
// 一个重量级依赖（libzip + pugixml）不划算。这里只实现 Excel 能打开所必需的
// 最小 OOXML 包：ZIP（store 不压缩）+ 一个 sheet + 表头加粗样式。
//
// 限制（与用途匹配）：单工作表、纯文本/数值单元格、无公式/图表/合并单元格。
// 单元格文本按 UTF-8 写入（Excel 2016+ 正常显示中文）。
// =============================================================================

#pragma once

#include <string>
#include <vector>

namespace moleculens::app::xlsx {

struct Cell {
    bool numeric = false;
    double number = 0.0;
    std::string text;

    static Cell textCell(std::string value) {
        Cell c;
        c.numeric = false;
        c.text = std::move(value);
        return c;
    }
    static Cell numberCell(double value) {
        Cell c;
        c.numeric = true;
        c.number = value;
        return c;
    }
};

// path：输出 .xlsx 路径；headers：首行（加粗）；rows：数据行（列数可与表头不同，
// 缺失的列留空）。失败时返回 false 并写 outError（若提供）。
bool writeSheet(const std::string& path, const std::string& sheetName,
                const std::vector<std::string>& headers,
                const std::vector<std::vector<Cell>>& rows,
                std::string* outError = nullptr);

} // namespace moleculens::app::xlsx
