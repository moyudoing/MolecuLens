// =============================================================================
// MolecuLens v2 · app/workbench/analysis/xlsx_writer.cpp
// 最小 XLSX（ZIP store + OOXML sheet）。格式细节见头文件。
// =============================================================================

#include "app/workbench/analysis/xlsx_writer.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

namespace moleculens::app::xlsx {

namespace {

// —— ZIP（store）——
struct ZipEntry {
    std::string name;
    std::string data;
    uint32_t crc = 0;
    uint32_t offset = 0;
};

uint32_t crc32Of(const std::string& data) {
    static std::array<uint32_t, 256> table{};
    static bool ready = false;
    if (!ready) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) {
                c = (c & 1u) != 0u ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            }
            table[i] = c;
        }
        ready = true;
    }
    uint32_t crc = 0xFFFFFFFFu;
    for (unsigned char ch : data) {
        crc = table[(crc ^ ch) & 0xFFu] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFFu;
}

void putU16(std::string& out, uint16_t v) {
    out.push_back(static_cast<char>(v & 0xFFu));
    out.push_back(static_cast<char>((v >> 8) & 0xFFu));
}

void putU32(std::string& out, uint32_t v) {
    out.push_back(static_cast<char>(v & 0xFFu));
    out.push_back(static_cast<char>((v >> 8) & 0xFFu));
    out.push_back(static_cast<char>((v >> 16) & 0xFFu));
    out.push_back(static_cast<char>((v >> 24) & 0xFFu));
}

bool writeZip(const std::string& path, std::vector<ZipEntry>& entries,
              std::string* outError) {
    std::string body;
    for (auto& e : entries) {
        e.crc = crc32Of(e.data);
        e.offset = static_cast<uint32_t>(body.size());
        putU32(body, 0x04034B50u);         // local file header
        putU16(body, 20);                  // version needed
        putU16(body, 0x0800);              // UTF-8 文件名
        putU16(body, 0);                   // method = store
        putU16(body, 0);                   // mod time
        putU16(body, 0);                   // mod date
        putU32(body, e.crc);
        putU32(body, static_cast<uint32_t>(e.data.size()));
        putU32(body, static_cast<uint32_t>(e.data.size()));
        putU16(body, static_cast<uint16_t>(e.name.size()));
        putU16(body, 0);                   // extra length
        body += e.name;
        body += e.data;
    }
    const uint32_t centralStart = static_cast<uint32_t>(body.size());
    for (const auto& e : entries) {
        putU32(body, 0x02014B50u);         // central directory header
        putU16(body, 20);                  // version made by
        putU16(body, 20);                  // version needed
        putU16(body, 0x0800);              // UTF-8
        putU16(body, 0);                   // method
        putU16(body, 0);                   // time
        putU16(body, 0);                   // date
        putU32(body, e.crc);
        putU32(body, static_cast<uint32_t>(e.data.size()));
        putU32(body, static_cast<uint32_t>(e.data.size()));
        putU16(body, static_cast<uint16_t>(e.name.size()));
        putU16(body, 0);                   // extra
        putU16(body, 0);                   // comment
        putU16(body, 0);                   // disk
        putU16(body, 0);                   // internal attrs
        putU32(body, 0);                   // external attrs
        putU32(body, e.offset);
        body += e.name;
    }
    const uint32_t centralSize =
        static_cast<uint32_t>(body.size()) - centralStart;
    putU32(body, 0x06054B50u);             // end of central directory
    putU16(body, 0);                       // disk
    putU16(body, 0);                       // central disk
    putU16(body, static_cast<uint16_t>(entries.size()));
    putU16(body, static_cast<uint16_t>(entries.size()));
    putU32(body, centralSize);
    putU32(body, centralStart);
    putU16(body, 0);                       // comment length

    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) {
        if (outError != nullptr) {
            *outError = "无法创建文件";
        }
        return false;
    }
    f.write(body.data(), static_cast<std::streamsize>(body.size()));
    if (!f.good()) {
        if (outError != nullptr) {
            *outError = "写入文件失败";
        }
        return false;
    }
    return true;
}

// —— XML ——
std::string xmlEscape(const std::string& in) {
    std::string out;
    out.reserve(in.size() + 16);
    for (unsigned char c : in) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            case '\'': out += "&apos;"; break;
            default:
                if (c < 0x20 && c != '\t' && c != '\n' && c != '\r') {
                    break;   // XML 1.0 不允许的控制字符：丢弃
                }
                out.push_back(static_cast<char>(c));
                break;
        }
    }
    return out;
}

std::string columnName(size_t index) {
    std::string name;
    size_t n = index + 1;
    while (n > 0) {
        const size_t rem = (n - 1) % 26;
        name.insert(name.begin(), static_cast<char>('A' + rem));
        n = (n - 1) / 26;
    }
    return name;
}

std::string numberText(double v) {
    if (!std::isfinite(v)) {
        return "0";
    }
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.10g", v);
    return buf;
}

std::string cellXml(size_t row, size_t col, const Cell& cell, bool headerStyle) {
    const std::string ref = columnName(col) + std::to_string(row);
    if (cell.numeric) {
        return "<c r=\"" + ref + "\"><v>" + numberText(cell.number) + "</v></c>";
    }
    if (cell.text.empty()) {
        return {};
    }
    const char* style = headerStyle ? " s=\"1\"" : "";
    return "<c r=\"" + ref + "\" t=\"inlineStr\"" + style + "><is><t xml:space=\"preserve\">" +
           xmlEscape(cell.text) + "</t></is></c>";
}

std::string sheetXml(const std::vector<std::string>& headers,
                     const std::vector<std::vector<Cell>>& rows) {
    std::string xml;
    xml.reserve(4096 + rows.size() * 512);
    xml += "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>";
    xml += "<worksheet xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\">";
    xml += "<sheetViews><sheetView workbookViewId=\"0\">"
           "<pane ySplit=\"1\" topLeftCell=\"A2\" activePane=\"bottomLeft\" state=\"frozen\"/>"
           "</sheetView></sheetViews>";
    xml += "<cols>";
    const size_t cols = std::max<size_t>(headers.size(), 1);
    for (size_t c = 0; c < cols; ++c) {
        const char* width = c == 0 ? "6" : (c <= 2 ? "22" : "15");
        xml += "<col min=\"" + std::to_string(c + 1) + "\" max=\"" +
               std::to_string(c + 1) + "\" width=\"" + width + "\" customWidth=\"1\"/>";
    }
    xml += "</cols><sheetData>";
    auto rowXml = [](size_t rowIndex, const std::vector<std::string>& values) {
        std::string r = "<row r=\"" + std::to_string(rowIndex) + "\">";
        for (size_t c = 0; c < values.size(); ++c) {
            r += cellXml(rowIndex, c, Cell::textCell(values[c]),
                         rowIndex == 1);
        }
        r += "</row>";
        return r;
    };
    xml += rowXml(1, headers);
    for (size_t i = 0; i < rows.size(); ++i) {
        const size_t rowIndex = i + 2;
        std::string r = "<row r=\"" + std::to_string(rowIndex) + "\">";
        const auto& cells = rows[i];
        for (size_t c = 0; c < cells.size(); ++c) {
            r += cellXml(rowIndex, c, cells[c], false);
        }
        r += "</row>";
        xml += r;
    }
    xml += "</sheetData></worksheet>";
    return xml;
}

std::string stylesXml() {
    return
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
        "<styleSheet xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\">"
        "<fonts count=\"2\">"
        "<font><sz val=\"11\"/><name val=\"Calibri\"/></font>"
        "<font><b/><sz val=\"11\"/><name val=\"Calibri\"/></font>"
        "</fonts>"
        "<fills count=\"2\"><fill><patternFill patternType=\"none\"/></fill>"
        "<fill><patternFill patternType=\"gray125\"/></fill></fills>"
        "<borders count=\"1\"><border><left/><right/><top/><bottom/><diagonal/></border></borders>"
        "<cellStyleXfs count=\"1\"><xf numFmtId=\"0\" fontId=\"0\" fillId=\"0\" borderId=\"0\"/></cellStyleXfs>"
        "<cellXfs count=\"2\">"
        "<xf numFmtId=\"0\" fontId=\"0\" fillId=\"0\" borderId=\"0\" xfId=\"0\"/>"
        "<xf numFmtId=\"0\" fontId=\"1\" fillId=\"0\" borderId=\"0\" xfId=\"0\" applyFont=\"1\"/>"
        "</cellXfs>"
        "<cellStyles count=\"1\"><cellStyle name=\"Normal\" xfId=\"0\" builtinId=\"0\"/></cellStyles>"
        "</styleSheet>";
}

std::string workbookXml(const std::string& sheetName) {
    return
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
        "<workbook xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\" "
        "xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\">"
        "<sheets><sheet name=\"" + xmlEscape(sheetName) +
        "\" sheetId=\"1\" r:id=\"rId1\"/></sheets></workbook>";
}

std::string workbookRelsXml() {
    return
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
        "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
        "<Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/worksheet\" Target=\"worksheets/sheet1.xml\"/>"
        "<Relationship Id=\"rId2\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/styles\" Target=\"styles.xml\"/>"
        "</Relationships>";
}

std::string rootRelsXml() {
    return
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
        "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
        "<Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument\" Target=\"xl/workbook.xml\"/>"
        "</Relationships>";
}

std::string contentTypesXml() {
    return
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
        "<Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\">"
        "<Default Extension=\"rels\" ContentType=\"application/vnd.openxmlformats-package.relationships+xml\"/>"
        "<Default Extension=\"xml\" ContentType=\"application/xml\"/>"
        "<Override PartName=\"/xl/workbook.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.sheet.main+xml\"/>"
        "<Override PartName=\"/xl/worksheets/sheet1.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.worksheet+xml\"/>"
        "<Override PartName=\"/xl/styles.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.styles+xml\"/>"
        "</Types>";
}

} // namespace

bool writeSheet(const std::string& path, const std::string& sheetName,
                const std::vector<std::string>& headers,
                const std::vector<std::vector<Cell>>& rows,
                std::string* outError) {
    std::vector<ZipEntry> entries;
    entries.push_back({"[Content_Types].xml", contentTypesXml(), 0, 0});
    entries.push_back({"_rels/.rels", rootRelsXml(), 0, 0});
    entries.push_back({"xl/workbook.xml", workbookXml(sheetName), 0, 0});
    entries.push_back({"xl/_rels/workbook.xml.rels", workbookRelsXml(), 0, 0});
    entries.push_back({"xl/styles.xml", stylesXml(), 0, 0});
    entries.push_back({"xl/worksheets/sheet1.xml", sheetXml(headers, rows), 0, 0});
    return writeZip(path, entries, outError);
}

} // namespace moleculens::app::xlsx
