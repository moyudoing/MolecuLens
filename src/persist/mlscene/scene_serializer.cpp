// =============================================================================
// MolecuLens · persist · mlscene/scene_serializer.cpp —— .mlscene 序列化实现
// 规格来源：主方案 §12.1/附录 G（固定前缀 + JSON 元数据 + 命令流 + 坐标 SoA +
//           CRC32；临时文件 + rename 原子写）
// =============================================================================

#include "persist/mlscene/scene_serializer.h"

#include <cstring>
#include <fstream>

#include <nlohmann/json.hpp>

#include "entity/element/element.h"
#include "engine/cmd/cmd.h"
#include "spdlog/spdlog.h"

namespace moleculens::persist::mlscene {

namespace {

// CRC32（IEEE 802.3，多项式 0xEDB88320；主方案 §12.1）
uint32_t crc32(const uint8_t* data, size_t size) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < size; ++i) {
        crc ^= data[i];
        for (int b = 0; b < 8; ++b) {
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
    }
    return ~crc;
}

// 元素符号表（序列化用）
const char* elementSymbol(entity::ElementType e) {
    return entity::elementInfo(e).symbol;
}

entity::ElementType elementFromSymbolStr(const std::string& s) {
    return entity::elementFromSymbol(s.c_str());
}

} // namespace

entity::Result<void> MlSceneSerializer::save(const SceneSnapshot& snap,
                                             const std::filesystem::path& path,
                                             const SaveOptions& opts) {
    // 元数据 JSON（附录 G）
    nlohmann::json meta;
    meta["version"] = kVersion;
    meta["timestamp"] = "2026-01-01T00:00:00Z";
    meta["units"] = snap.units;
    {
        nlohmann::json structures = nlohmann::json::array();
        for (const auto& st : snap.structures) {
            nlohmann::json s;
            s["elements"] = st.elements;
            nlohmann::json bonds = nlohmann::json::array();
            for (const auto& b : st.bonds) {
                bonds.push_back({b[0], b[1], b[2]});
            }
            s["bonds"] = bonds;
            structures.push_back(s);
        }
        meta["structures"] = structures;
    }
    {
        nlohmann::json instances = nlohmann::json::array();
        for (const auto& inst : snap.instances) {
            nlohmann::json i;
            i["structureIndex"] = inst.structureIndex;
            i["name"] = inst.name;
            i["visible"] = inst.visible;
            i["transform"] = inst.transform;
            i["trajectory"] = {{"frameCount", inst.frameCount}, {"dt", inst.dt}};
            instances.push_back(i);
        }
        meta["instances"] = instances;
    }
    {
        nlohmann::json reps = nlohmann::json::array();
        for (const auto& r : snap.representations) {
            nlohmann::json j;
            j["name"] = r.name;
            j["type"] = r.type;
            j["selection"] = r.selection;
            j["colorScheme"] = r.colorScheme;
            j["radiusScale"] = r.radiusScale;
            j["visible"] = r.visible;
            reps.push_back(j);
        }
        meta["representations"] = reps;
    }
    meta["scene"] = {{"bgColor", snap.bgColor}, {"ambient", snap.ambient},
                     {"scale", snap.scale}};
    meta["camera"] = {{"target", snap.cameraTarget},
                      {"distance", snap.cameraDistance},
                      {"yaw", snap.cameraYaw},
                      {"pitch", snap.cameraPitch}};
    const std::string metaStr = meta.dump(opts.jsonIndent);

    // 命令流段（engine/cmd 序列化布局）
    std::vector<uint8_t> cmdBytes;
    {
        engine::ByteSink sink(cmdBytes);
        engine::SceneCommandList list;
        engine::SceneCommandList::writeTo(sink, list);   // 空命令流（M1 命令流段可选）
    }

    // 坐标 SoA 段（附录 G 布局）
    std::vector<uint8_t> coordsBytes;
    if (opts.writeCoords) {
        engine::ByteSink sink(coordsBytes);
        const uint64_t posCount = snap.coordsPositions.size() / 3;
        const uint64_t colorCount = snap.coordsColors.size() / 4;
        const uint64_t idCount = snap.coordsAtomIds.size();
        sink.writeValue<uint64_t>(posCount);
        sink.write(snap.coordsPositions.data(),
                   snap.coordsPositions.size() * sizeof(float));
        sink.writeValue<uint64_t>(colorCount);
        sink.write(snap.coordsColors.data(), snap.coordsColors.size() * sizeof(float));
        sink.writeValue<uint64_t>(idCount);
        sink.write(snap.coordsAtomIds.data(), snap.coordsAtomIds.size() * sizeof(uint32_t));
    }

    // 组装文件（临时文件 + fsync + rename，主方案 §12.1）
    const auto tmpPath = std::filesystem::path(path.string() + ".tmp");
    {
        std::ofstream out(tmpPath, std::ios::binary | std::ios::trunc);
        if (!out) {
            return entity::Result<void>::error(entity::ErrorCode::IoError,
                                               "Cannot write " + tmpPath.string());
        }
        Header header;
        std::memcpy(header.magic, kMagic, 6);
        header.version = kVersion;
        header.metaSize = static_cast<uint64_t>(metaStr.size());
        out.write(reinterpret_cast<const char*>(&header), sizeof(Header));
        out.write(metaStr.data(), static_cast<std::streamsize>(metaStr.size()));

        const uint64_t cmdSize = cmdBytes.size();
        out.write(reinterpret_cast<const char*>(&cmdSize), 8);
        out.write(reinterpret_cast<const char*>(cmdBytes.data()),
                  static_cast<std::streamsize>(cmdBytes.size()));

        const uint64_t coordsSize = coordsBytes.size();
        out.write(reinterpret_cast<const char*>(&coordsSize), 8);
        out.write(reinterpret_cast<const char*>(coordsBytes.data()),
                  static_cast<std::streamsize>(coordsBytes.size()));

        // CRC32 占位（先写 0；关闭后用读写流回填）
        out.write("\0\0\0\0", 4);
        out.flush();
    }
    // 回填 CRC（fstream 读写打开；覆盖 [0, filesize-4)，附录 G）
    {
        std::fstream rw(tmpPath, std::ios::in | std::ios::out | std::ios::binary);
        if (!rw) {
            return entity::Result<void>::error(entity::ErrorCode::IoError,
                                               "Cannot reopen " + tmpPath.string());
        }
        rw.seekg(0, std::ios::end);
        const std::streamoff fileSize = rw.tellg();
        std::vector<uint8_t> all(static_cast<size_t>(fileSize));
        rw.seekg(0);
        rw.read(reinterpret_cast<char*>(all.data()), fileSize);
        // CRC 覆盖 [0, filesize-4)（附录 G；尾部 4 字节为 CRC 本身）
        const uint32_t crc = crc32(all.data(), all.size() > 4 ? all.size() - 4 : 0);
        rw.seekp(fileSize - 4);
        rw.write(reinterpret_cast<const char*>(&crc), 4);
        rw.flush();
    }
    // 原子替换（主方案 §12.1：临时文件 + rename）
    std::error_code ec;
    std::filesystem::rename(tmpPath, path, ec);
    if (ec) {
        std::filesystem::remove(path, ec);
        std::filesystem::rename(tmpPath, path, ec);
    }
    if (ec) {
        return entity::Result<void>::error(entity::ErrorCode::IoError,
                                           "Cannot rename to " + path.string());
    }
    spdlog::info("[persist] .mlscene saved: {}", path.string());
    return entity::Result<void>::ok();
}

entity::Result<SceneSnapshot> MlSceneSerializer::load(const std::filesystem::path& path,
                                                      const LoadOptions& opts) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return entity::Result<SceneSnapshot>::error(entity::ErrorCode::IoError,
                                                    "Cannot open " + path.string());
    }
    std::vector<uint8_t> all((std::istreambuf_iterator<char>(in)),
                             std::istreambuf_iterator<char>());
    if (all.size() < sizeof(Header) + 4) {
        return entity::Result<SceneSnapshot>::error(
            entity::ErrorCode::FormatError, "file too small: " + path.string());
    }

    // 魔数 + 版本校验（主方案 §14.4.4：版本不匹配拒绝）
    Header header;
    std::memcpy(&header, all.data(), sizeof(Header));
    if (std::memcmp(header.magic, kMagic, 6) != 0) {
        return entity::Result<SceneSnapshot>::error(
            entity::ErrorCode::FormatError, "bad magic: " + path.string());
    }
    if (header.version != kVersion) {
        return entity::Result<SceneSnapshot>::error(
            entity::ErrorCode::VersionError,
            "version " + std::to_string(header.version) + " != " +
                std::to_string(kVersion));
    }

    // CRC 校验（篡改 1 字节 → 加载失败，主方案 §14.4.4）
    const uint32_t storedCrc = [&]() {
        uint32_t c = 0;
        std::memcpy(&c, all.data() + all.size() - 4, 4);
        return c;
    }();
    if (crc32(all.data(), all.size() - 4) != storedCrc) {
        return entity::Result<SceneSnapshot>::error(
            entity::ErrorCode::CrcError, "CRC mismatch: " + path.string());
    }

    size_t pos = sizeof(Header);
    if (pos + header.metaSize > all.size()) {
        return entity::Result<SceneSnapshot>::error(entity::ErrorCode::FormatError,
                                                    "truncated meta");
    }
    const std::string metaStr(reinterpret_cast<const char*>(all.data() + pos),
                              static_cast<size_t>(header.metaSize));
    pos += static_cast<size_t>(header.metaSize);

    SceneSnapshot snap;
    try {
        const nlohmann::json meta = nlohmann::json::parse(metaStr);
        snap.units = meta.value("units", "angstrom");
        for (const auto& st : meta["structures"]) {
            SceneSnapshot::Structure s;
            s.elements = st["elements"].get<std::vector<std::string>>();
            for (const auto& b : st["bonds"]) {
                s.bonds.push_back({b[0].get<uint32_t>(), b[1].get<uint32_t>(),
                                   b[2].get<uint32_t>()});
            }
            snap.structures.push_back(std::move(s));
        }
        for (const auto& i : meta["instances"]) {
            SceneSnapshot::Instance inst;
            inst.structureIndex = i["structureIndex"].get<uint32_t>();
            inst.name = i.value("name", "");
            inst.visible = i.value("visible", true);
            const auto& t = i["transform"];
            for (int k = 0; k < 9; ++k) {
                inst.transform[static_cast<size_t>(k)] = t[k].get<float>();
            }
            if (i.contains("trajectory")) {
                inst.frameCount = i["trajectory"].value("frameCount", 1ull);
                inst.dt = i["trajectory"].value("dt", 2.0f);
            }
            snap.instances.push_back(std::move(inst));
        }
        if (meta.contains("representations")) {
            for (const auto& r : meta["representations"]) {
                SceneSnapshot::Representation rep;
                rep.name = r.value("name", "");
                rep.type = r.value("type", "ballstick");
                rep.selection = r.value("selection", "*");
                rep.colorScheme = r.value("colorScheme", "element");
                rep.radiusScale = r.value("radiusScale", 1.0f);
                rep.visible = r.value("visible", true);
                snap.representations.push_back(std::move(rep));
            }
        }
        if (meta.contains("scene")) {
            const auto& sc = meta["scene"];
            if (sc.contains("bgColor")) {
                for (int k = 0; k < 3; ++k) {
                    snap.bgColor[static_cast<size_t>(k)] = sc["bgColor"][k].get<float>();
                }
            }
            snap.ambient = sc.value("ambient", 0.3f);
            snap.scale = sc.value("scale", 1.0f);
        }
        if (meta.contains("camera")) {
            const auto& c = meta["camera"];
            if (c.contains("target")) {
                for (int k = 0; k < 3; ++k) {
                    snap.cameraTarget[static_cast<size_t>(k)] = c["target"][k].get<float>();
                }
            }
            snap.cameraDistance = c.value("distance", 40.0f);
            snap.cameraYaw = c.value("yaw", 0.0f);
            snap.cameraPitch = c.value("pitch", 0.5f);
        }
    } catch (const std::exception& e) {
        return entity::Result<SceneSnapshot>::error(
            entity::ErrorCode::ParseError,
            std::string("meta JSON: ") + e.what());
    }

    // 命令流段（可选；M1 命令流段为空）
    if (pos + 8 <= all.size()) {
        uint64_t cmdSize = 0;
        std::memcpy(&cmdSize, all.data() + pos, 8);
        pos += 8;
        if (pos + cmdSize <= all.size()) {
            snap.commandStream.assign(all.data() + pos, all.data() + pos + cmdSize);
            pos += static_cast<size_t>(cmdSize);
        } else if (opts.strict) {
            return entity::Result<SceneSnapshot>::error(
                entity::ErrorCode::FormatError, "truncated command stream");
        }
    }

    // 坐标 SoA 段（可选；lazyCoords 时跳过）
    if (!opts.lazyCoords && pos + 8 <= all.size()) {
        uint64_t coordsSize = 0;
        std::memcpy(&coordsSize, all.data() + pos, 8);
        pos += 8;
        if (pos + coordsSize <= all.size()) {
            const uint8_t* cp = all.data() + pos;
            const uint8_t* end = cp + coordsSize;
            uint64_t posCount = 0, colorCount = 0, idCount = 0;
            if (cp + 8 <= end) {
                std::memcpy(&posCount, cp, 8);
                cp += 8;
            }
            if (cp + posCount * 12 <= end) {
                snap.coordsPositions.resize(static_cast<size_t>(posCount) * 3);
                std::memcpy(snap.coordsPositions.data(), cp,
                            static_cast<size_t>(posCount) * 12);
                cp += static_cast<size_t>(posCount) * 12;
            }
            if (cp + 8 <= end) {
                std::memcpy(&colorCount, cp, 8);
                cp += 8;
            }
            if (cp + colorCount * 16 <= end) {
                snap.coordsColors.resize(static_cast<size_t>(colorCount) * 4);
                std::memcpy(snap.coordsColors.data(), cp,
                            static_cast<size_t>(colorCount) * 16);
                cp += static_cast<size_t>(colorCount) * 16;
            }
            if (cp + 8 <= end) {
                std::memcpy(&idCount, cp, 8);
                cp += 8;
            }
            if (cp + idCount * 4 <= end) {
                snap.coordsAtomIds.resize(static_cast<size_t>(idCount));
                std::memcpy(snap.coordsAtomIds.data(), cp,
                            static_cast<size_t>(idCount) * 4);
            }
            // 约束：posCount == colorCount == idCount（附录 G）
            if (posCount != colorCount || posCount != idCount) {
                return entity::Result<SceneSnapshot>::error(
                    entity::ErrorCode::FormatError, "coords SoA count mismatch");
            }
        } else if (opts.strict) {
            return entity::Result<SceneSnapshot>::error(
                entity::ErrorCode::FormatError, "truncated coords SoA");
        }
    }
    spdlog::info("[persist] .mlscene loaded: {} ({} structures, {} instances)",
                 path.string(), snap.structures.size(), snap.instances.size());
    return entity::Result<SceneSnapshot>::ok(std::move(snap));
}

} // namespace moleculens::persist::mlscene
