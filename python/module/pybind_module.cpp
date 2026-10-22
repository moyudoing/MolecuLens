// =============================================================================
// MolecuLens · python · module/pybind_module.cpp —— Python 绑定模块（_native）
// 规格来源：主方案 §11.1（Python 绑定导出 moleculens 模块）
//
// 设计原则：
//   · 只绑定 **entity 数据层**（解析 / 原子 / 键 / 选择语言）——不依赖 Vulkan、
//     GLFW 或窗口，可在无图形环境（CI / 服务器）中导入与测试；
//   · 渲染仍走 `moleculens-cli render`（同一份 C++ 实现），Python 侧不复制参数表；
//   · 所有返回的原子/键均为**值拷贝**（ReadOnly 快照），避免 Python 持有悬垂指针；
//   · 全局原子下标与 SceneStructure::coordsSoA() 的顺序严格一致，选择语言掩码
//     可直接用于下标。
// =============================================================================

#include <pybind11/operators.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <pybind11/stl/filesystem.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "entity/atom/atom.h"
#include "entity/element/element.h"
#include "entity/molecule/molecule.h"
#include "entity/scene/scene.h"
#include "entity/selection/parse_table.h"
#include "entity/selection/selection.h"
#include "io/registry/ifile_reader.h"
#include "io/registry/parser_registry.h"
#include "spdlog/spdlog.h"

namespace py = pybind11;

namespace moleculens::python {
namespace {

using entity::Atom;
using entity::ErrorCode;
using entity::ElementInfo;
using entity::ElementType;
using entity::FrameData;
using entity::MoleculeStructure;
using entity::ParseOptions;
using entity::ParseResult;
using entity::ParseWarning;
using entity::ParserRegistry;
using entity::SceneStructure;
using entity::Selection;

// —— 异常 ——
class MoleculensError : public std::runtime_error {
public:
    MoleculensError(ErrorCode code, std::string message)
        : std::runtime_error(format(code, message)), code_(code) {}

    [[nodiscard]] ErrorCode code() const noexcept { return code_; }

private:
    static std::string format(ErrorCode code, const std::string& message) {
        const char* name = "Unknown";
        switch (code) {
            case ErrorCode::Ok: name = "Ok"; break;
            case ErrorCode::NotFound: name = "NotFound"; break;
            case ErrorCode::ParseError: name = "ParseError"; break;
            case ErrorCode::IoError: name = "IoError"; break;
            case ErrorCode::FormatError: name = "FormatError"; break;
            case ErrorCode::VersionError: name = "VersionError"; break;
            case ErrorCode::CrcError: name = "CrcError"; break;
            case ErrorCode::InvalidState: name = "InvalidState"; break;
            case ErrorCode::OutOfMemory: name = "OutOfMemory"; break;
            case ErrorCode::Cancelled: name = "Cancelled"; break;
            case ErrorCode::Unknown: name = "Unknown"; break;
        }
        return std::string(name) + ": " + message;
    }

    ErrorCode code_;
};

// —— 只读快照（值类型）——
struct AtomData {
    uint32_t index = 0;
    uint32_t instance = 0;
    std::string name;
    std::string element;        // 元素符号（"C" / "O" / "Xx"）
    std::string element_name;   // 元素英文名（"Carbon"）
    std::string residue_name;
    std::string chain_name;
    int32_t residue_id = -1;    // SceneStructure 残基表下标（选择语言 resid 同源）
    int32_t chain_id = -1;
    uint8_t atomic_number = 0;
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float mass = 0.0f;
    float covalent_radius = 0.0f;
    float vdw_radius = 0.0f;
    float occupancy = 0.0f;     // 0–100（与选择语言一致）
    float bfactor = 0.0f;       // Å²（内部 ×100 存储，这里已还原）
    std::array<float, 3> color{0.0f, 0.0f, 0.0f};

    bool operator==(const AtomData& other) const {
        return index == other.index && instance == other.instance &&
               name == other.name && element == other.element &&
               element_name == other.element_name &&
               residue_name == other.residue_name &&
               chain_name == other.chain_name &&
               residue_id == other.residue_id && chain_id == other.chain_id &&
               atomic_number == other.atomic_number && x == other.x &&
               y == other.y && z == other.z && mass == other.mass &&
               covalent_radius == other.covalent_radius &&
               vdw_radius == other.vdw_radius &&
               occupancy == other.occupancy && bfactor == other.bfactor &&
               color == other.color;
    }
};

struct BondData {
    uint32_t a = 0;
    uint32_t b = 0;
    uint8_t order = 0;          // 0 芳香 / 1 / 2 / 3

    bool operator==(const BondData& other) const {
        return a == other.a && b == other.b && order == other.order;
    }
};

struct WarningData {
    size_t line = 0;
    std::string message;
};

struct ElementData {
    std::string symbol;
    std::string name;
    uint8_t atomic_number = 0;
    float covalent_radius = 0.0f;
    float vdw_radius = 0.0f;
    float mass = 0.0f;
    std::array<float, 3> color{0.0f, 0.0f, 0.0f};
};

ElementData toElementData(const ElementInfo& info) {
    ElementData out;
    out.symbol = info.symbol != nullptr ? info.symbol : "Xx";
    out.name = info.name != nullptr ? info.name : "Unknown";
    out.atomic_number = info.atomicNumber;
    out.covalent_radius = info.covalentRadius;
    out.vdw_radius = info.vdwRadius;
    out.mass = info.mass;
    out.color = {info.color[0], info.color[1], info.color[2]};
    return out;
}

// 全局原子下标顺序 = instances 顺序 × 结构原子顺序，与 coordsSoA 一致。
std::vector<AtomData> buildAtoms(const SceneStructure& scene) {
    std::vector<AtomData> out;
    const auto& positions = scene.coordsSoA().positions;
    size_t global = 0;
    for (size_t inst = 0; inst < scene.instances().size(); ++inst) {
        const auto& instance = scene.instances()[inst];
        if (instance.structureIndex >= scene.structures().size()) {
            continue;
        }
        const MoleculeStructure& ms = scene.structures()[instance.structureIndex];
        for (size_t i = 0; i < ms.elements.size(); ++i, ++global) {
            const ElementInfo& info = entity::elementInfo(ms.elements[i]);
            AtomData atom;
            atom.index = static_cast<uint32_t>(global);
            atom.instance = static_cast<uint32_t>(inst);
            atom.name = std::string(scene.atomName(instance.structureIndex,
                                                   static_cast<uint32_t>(i)));
            atom.element = info.symbol != nullptr ? info.symbol : "Xx";
            atom.element_name = info.name != nullptr ? info.name : "Unknown";
            atom.atomic_number = info.atomicNumber;
            atom.mass = info.mass;
            atom.covalent_radius = info.covalentRadius;
            atom.vdw_radius = info.vdwRadius;
            atom.color = {info.color[0], info.color[1], info.color[2]};

            if (i < ms.atoms.size()) {
                const Atom& raw = ms.atoms[i];
                atom.residue_id = raw.residueId;
                atom.chain_id = raw.chainId;
                atom.occupancy = static_cast<float>(raw.occupancy);
                atom.bfactor = static_cast<float>(raw.bfactor) / 100.0f;
            }
            atom.residue_name = std::string(scene.residueName(atom.residue_id));
            atom.chain_name = std::string(scene.chainName(atom.chain_id));

            if (global < positions.size()) {
                atom.x = positions[global].x;
                atom.y = positions[global].y;
                atom.z = positions[global].z;
            }
            out.push_back(std::move(atom));
        }
    }
    return out;
}

std::vector<BondData> buildBonds(const SceneStructure& scene) {
    std::vector<BondData> out;
    uint32_t base = 0;
    for (const auto& instance : scene.instances()) {
        if (instance.structureIndex >= scene.structures().size()) {
            continue;
        }
        const MoleculeStructure& ms = scene.structures()[instance.structureIndex];
        for (const auto& bond : ms.bonds) {
            BondData b;
            b.a = base + bond.a;
            b.b = base + bond.b;
            b.order = bond.order;
            out.push_back(b);
        }
        base += static_cast<uint32_t>(ms.elements.size());
    }
    return out;
}

std::vector<bool> evaluateSelection(const SceneStructure& scene,
                                    const std::string& expression) {
    Selection selection;
    if (!selection.parse(expression, entity::ParseTable::defaultTable())) {
        throw py::value_error("invalid selection expression: '" + expression + "'");
    }
    FrameData frame;
    frame.positions = &scene.coordsSoA().positions;
    selection.evaluate(scene, frame, 0);
    return selection.mask();
}

struct Structure {
    Structure(std::shared_ptr<SceneStructure> scene,
              std::string source,
              uint64_t frameCount,
              float dt,
              std::vector<WarningData> warnings)
        : scene_(std::move(scene)),
          source_(std::move(source)),
          frameCount_(frameCount),
          dt_(dt),
          warnings_(std::move(warnings)) {
        atoms_ = buildAtoms(*scene_);
        bonds_ = buildBonds(*scene_);
        if (atoms_.size() != scene_->coordsSoA().atomIds.size()) {
            // 选择语言掩码长度取自 coordsSoA；数量不一致会让下标错位，宁可显式失败。
            throw MoleculensError(
                ErrorCode::InvalidState,
                "atom count and coordinate segment size disagree ("
                    + std::to_string(atoms_.size()) + " vs "
                    + std::to_string(scene_->coordsSoA().atomIds.size()) + ")");
        }
    }

    [[nodiscard]] const std::vector<AtomData>& atoms() const { return atoms_; }
    [[nodiscard]] const std::vector<BondData>& bonds() const { return bonds_; }
    [[nodiscard]] const std::string& source() const { return source_; }
    [[nodiscard]] uint64_t frameCount() const { return frameCount_; }
    [[nodiscard]] float dt() const { return dt_; }
    [[nodiscard]] const std::vector<WarningData>& warnings() const { return warnings_; }
    [[nodiscard]] const SceneStructure& scene() const { return *scene_; }
    [[nodiscard]] size_t structureCount() const { return scene_->structures().size(); }
    [[nodiscard]] size_t instanceCount() const { return scene_->instances().size(); }

    [[nodiscard]] std::vector<uint32_t> select(const std::string& expression) const {
        const std::vector<bool> mask = evaluateSelection(*scene_, expression);
        std::vector<uint32_t> indices;
        for (size_t i = 0; i < mask.size(); ++i) {
            if (mask[i]) {
                indices.push_back(static_cast<uint32_t>(i));
            }
        }
        return indices;
    }

    [[nodiscard]] py::dict summary() const {
        py::dict out;
        out["source"] = source_;
        out["structure_count"] = structureCount();
        out["instance_count"] = instanceCount();
        out["atom_count"] = atoms_.size();
        out["bond_count"] = bonds_.size();
        out["frame_count"] = frameCount_;
        out["dt"] = dt_;
        std::map<std::string, size_t> composition;
        for (const auto& atom : atoms_) {
            ++composition[atom.element];
        }
        out["composition"] = composition;
        py::list warnings;
        for (const auto& warning : warnings_) {
            py::dict w;
            w["line"] = warning.line;
            w["message"] = warning.message;
            warnings.append(std::move(w));
        }
        out["warnings"] = std::move(warnings);
        return out;
    }

    [[nodiscard]] std::vector<std::tuple<float, float, float>> positions() const {
        std::vector<std::tuple<float, float, float>> out;
        out.reserve(atoms_.size());
        for (const auto& atom : atoms_) {
            out.emplace_back(atom.x, atom.y, atom.z);
        }
        return out;
    }

private:
    std::shared_ptr<SceneStructure> scene_;
    std::string source_;
    uint64_t frameCount_ = 1;
    float dt_ = 2.0f;
    std::vector<WarningData> warnings_;
    std::vector<AtomData> atoms_;
    std::vector<BondData> bonds_;
};

struct SelectionView {
    SelectionView(std::string expression, std::vector<bool> mask)
        : expression_(std::move(expression)), mask_(std::move(mask)) {
        for (size_t i = 0; i < mask_.size(); ++i) {
            if (mask_[i]) {
                indices_.push_back(static_cast<uint32_t>(i));
            }
        }
    }

    [[nodiscard]] const std::string& expression() const { return expression_; }
    [[nodiscard]] const std::vector<bool>& mask() const { return mask_; }
    [[nodiscard]] const std::vector<uint32_t>& indices() const { return indices_; }
    [[nodiscard]] size_t count() const { return indices_.size(); }
    [[nodiscard]] bool contains(int64_t index) const {
        return index >= 0 && static_cast<size_t>(index) < mask_.size() &&
               mask_[static_cast<size_t>(index)];
    }

private:
    std::string expression_;
    std::vector<bool> mask_;
    std::vector<uint32_t> indices_;
};

std::shared_ptr<Structure> loadFile(const std::filesystem::path& path,
                                    bool inferBonds,
                                    bool strict) {
    ParseOptions options;
    options.inferBonds = inferBonds;
    options.strict = strict;

    auto result = ParserRegistry::defaultRegistry().parseFile(path, options);
    if (!result) {
        throw MoleculensError(result.error().code, result.errorMessage());
    }
    if (!result->scene) {
        throw MoleculensError(ErrorCode::InvalidState,
                              "parser returned no scene: " + path.u8string());
    }

    std::vector<WarningData> warnings;
    warnings.reserve(result->warnings.size());
    for (const ParseWarning& warning : result->warnings) {
        warnings.push_back(WarningData{warning.line, warning.message});
    }
    // path.string() 在 Windows 上返回 ANSI（GBK）字节串，含中文的路径会在
    // Python 侧解码失败；u8string() 始终是 UTF-8。
    return std::make_shared<Structure>(std::move(result->scene),
                                       path.u8string(),
                                       result->frameCount,
                                       result->dt,
                                       std::move(warnings));
}

ElementData findElement(const std::string& symbol) {
    // 元素符号不区分大小写地接受（"c" / "CL" → "C" / "Cl"）。
    std::string normalized = symbol;
    while (!normalized.empty() &&
           std::isspace(static_cast<unsigned char>(normalized.front()))) {
        normalized.erase(normalized.begin());
    }
    while (!normalized.empty() &&
           std::isspace(static_cast<unsigned char>(normalized.back()))) {
        normalized.pop_back();
    }
    if (!normalized.empty()) {
        normalized[0] = static_cast<char>(
            std::toupper(static_cast<unsigned char>(normalized[0])));
        for (size_t i = 1; i < normalized.size(); ++i) {
            normalized[i] = static_cast<char>(
                std::tolower(static_cast<unsigned char>(normalized[i])));
        }
    }
    const ElementType type = entity::elementFromSymbol(normalized.c_str());
    if (type == ElementType::Unknown) {
        throw py::value_error("unknown element symbol: '" + symbol + "'");
    }
    return toElementData(entity::elementInfo(type));
}

ElementData elementByNumber(int atomicNumber) {
    if (atomicNumber < 1 || atomicNumber > 118) {
        throw py::value_error("atomic number out of range [1, 118]: "
                              + std::to_string(atomicNumber));
    }
    return toElementData(
        entity::elementInfo(static_cast<ElementType>(atomicNumber)));
}

std::vector<ElementData> allElements() {
    std::vector<ElementData> out;
    out.reserve(118);
    for (int z = 1; z <= 118; ++z) {
        out.push_back(toElementData(
            entity::elementInfo(static_cast<ElementType>(z))));
    }
    return out;
}

} // namespace
} // namespace moleculens::python

PYBIND11_MODULE(_native, m) {
    using namespace moleculens::python;

    m.doc() =
        "MolecuLens native data API: parse structures and evaluate the "
        "selection language without a GPU or a window.";

#ifdef MOLECULENS_PY_VERSION
    m.attr("__version__") = MOLECULENS_PY_VERSION;
#else
    m.attr("__version__") = "0.0.0";
#endif

    py::register_exception<MoleculensError>(m, "MoleculensError");

    // 与 moleculens-cli 一致：默认只报 warning/error，避免每次 load 都打印
    // 两行 info 日志；需要排查时设 MOLECULENS_LOG_LEVEL=debug/info。
    spdlog::set_level(spdlog::level::warn);
    if (const char* level = std::getenv("MOLECULENS_LOG_LEVEL")) {
        spdlog::set_level(spdlog::level::from_str(level));
    }

    py::class_<ElementData>(m, "Element")
        .def_readonly("symbol", &ElementData::symbol)
        .def_readonly("name", &ElementData::name)
        .def_readonly("atomic_number", &ElementData::atomic_number)
        .def_readonly("covalent_radius", &ElementData::covalent_radius)
        .def_readonly("vdw_radius", &ElementData::vdw_radius)
        .def_readonly("mass", &ElementData::mass)
        .def_readonly("color", &ElementData::color)
        .def("__repr__", [](const ElementData& e) {
            return "<moleculens.Element " + e.symbol + " (Z=" +
                   std::to_string(e.atomic_number) + ")>";
        });

    py::class_<AtomData>(m, "Atom")
        .def_readonly("index", &AtomData::index)
        .def_readonly("instance", &AtomData::instance)
        .def_readonly("name", &AtomData::name)
        .def_readonly("element", &AtomData::element)
        .def_readonly("element_name", &AtomData::element_name)
        .def_readonly("atomic_number", &AtomData::atomic_number)
        .def_readonly("residue_name", &AtomData::residue_name)
        .def_readonly("residue_id", &AtomData::residue_id)
        .def_readonly("chain_name", &AtomData::chain_name)
        .def_readonly("chain_id", &AtomData::chain_id)
        .def_readonly("x", &AtomData::x)
        .def_readonly("y", &AtomData::y)
        .def_readonly("z", &AtomData::z)
        .def_readonly("mass", &AtomData::mass)
        .def_readonly("covalent_radius", &AtomData::covalent_radius)
        .def_readonly("vdw_radius", &AtomData::vdw_radius)
        .def_readonly("occupancy", &AtomData::occupancy)
        .def_readonly("bfactor", &AtomData::bfactor)
        .def_readonly("color", &AtomData::color)
        .def_property_readonly("position", [](const AtomData& a) {
            return std::make_tuple(a.x, a.y, a.z);
        })
        .def(py::self == py::self)
        .def("__hash__", [](const AtomData& a) {
            return static_cast<size_t>(
                (static_cast<uint64_t>(a.instance) << 32) ^ a.index);
        })
        .def("__repr__", [](const AtomData& a) {
            return "<moleculens.Atom " + std::to_string(a.index) + " " +
                   a.name + " (" + a.element + ") " + a.residue_name + ">";
        });

    py::class_<BondData>(m, "Bond")
        .def_readonly("a", &BondData::a)
        .def_readonly("b", &BondData::b)
        .def_readonly("order", &BondData::order)
        .def(py::self == py::self)
        .def("__hash__", [](const BondData& b) {
            return static_cast<size_t>(
                (static_cast<uint64_t>(b.a) << 32) ^ b.b ^ b.order);
        })
        .def("__repr__", [](const BondData& b) {
            return "<moleculens.Bond " + std::to_string(b.a) + "-" +
                   std::to_string(b.b) + " order=" + std::to_string(b.order) + ">";
        });

    py::class_<WarningData>(m, "Warning")
        .def_readonly("line", &WarningData::line)
        .def_readonly("message", &WarningData::message)
        .def("__repr__", [](const WarningData& w) {
            return "<moleculens.Warning line=" + std::to_string(w.line) + ": " +
                   w.message + ">";
        });

    py::class_<SelectionView, std::shared_ptr<SelectionView>>(m, "Selection")
        .def_property_readonly("expression", &SelectionView::expression)
        .def_property_readonly("mask", &SelectionView::mask)
        .def_property_readonly("indices", &SelectionView::indices)
        .def_property_readonly("count", &SelectionView::count)
        .def("__len__", &SelectionView::count)
        .def("__contains__", &SelectionView::contains)
        .def("__repr__", [](const SelectionView& s) {
            return "<moleculens.Selection '" + s.expression() + "' -> " +
                   std::to_string(s.count()) + " atoms>";
        });

    py::class_<Structure, std::shared_ptr<Structure>>(m, "Structure")
        .def_property_readonly("source", &Structure::source)
        .def_property_readonly("path", &Structure::source)
        .def_property_readonly("frame_count", &Structure::frameCount)
        .def_property_readonly("dt", &Structure::dt)
        .def_property_readonly("structure_count", &Structure::structureCount)
        .def_property_readonly("instance_count", &Structure::instanceCount)
        .def_property_readonly("atom_count",
                               [](const Structure& s) { return s.atoms().size(); })
        .def_property_readonly("bond_count",
                               [](const Structure& s) { return s.bonds().size(); })
        .def_property_readonly("atoms", &Structure::atoms)
        .def_property_readonly("bonds", &Structure::bonds)
        .def_property_readonly("warnings", &Structure::warnings)
        .def_property_readonly("positions", &Structure::positions)
        .def("atom", [](const Structure& s, int64_t index) {
            int64_t i = index;
            if (i < 0) {
                i += static_cast<int64_t>(s.atoms().size());
            }
            if (i < 0 || static_cast<size_t>(i) >= s.atoms().size()) {
                throw py::index_error("atom index out of range");
            }
            return s.atoms()[static_cast<size_t>(i)];
        }, py::arg("index"))
        .def("__getitem__", [](const Structure& s, int64_t index) {
            int64_t i = index;
            if (i < 0) {
                i += static_cast<int64_t>(s.atoms().size());
            }
            if (i < 0 || static_cast<size_t>(i) >= s.atoms().size()) {
                throw py::index_error("atom index out of range");
            }
            return s.atoms()[static_cast<size_t>(i)];
        })
        .def("__len__", [](const Structure& s) { return s.atoms().size(); })
        .def("__iter__", [](const Structure& s) {
            return py::make_iterator(s.atoms().begin(), s.atoms().end());
        }, py::keep_alive<0, 1>())
        .def("select", &Structure::select, py::arg("expression"))
        .def("selection", [](const Structure& s, const std::string& expression) {
            return std::make_shared<SelectionView>(
                expression, evaluateSelection(s.scene(), expression));
        }, py::arg("expression"))
        .def("summary", &Structure::summary)
        .def("__repr__", [](const Structure& s) {
            return "<moleculens.Structure atoms=" + std::to_string(s.atoms().size()) +
                   " bonds=" + std::to_string(s.bonds().size()) +
                   " source='" + s.source() + "'>";
        });

    m.def("load", &loadFile,
          py::arg("path"),
          py::arg("infer_bonds") = true,
          py::arg("strict") = false,
          "Parse a structure file (PDB / GRO / SDF / cube) into a Structure.");

    m.def("element_info", py::overload_cast<const std::string&>(&findElement),
          py::arg("symbol"));
    m.def("element_info", &elementByNumber, py::arg("atomic_number"));
    m.def("elements", &allElements, "All 118 element records.");
    m.def("supported_extensions", [] {
        return ParserRegistry::defaultRegistry().allExtensions();
    }, "File extensions accepted by the built-in parsers.");
}
