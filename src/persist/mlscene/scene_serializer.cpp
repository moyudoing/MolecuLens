// =============================================================================
// MolecuLens · persist · mlscene/scene_serializer.cpp —— .mlscene 序列化实现（v2）
// 规格来源：主方案 §12.1/附录 G（固定前缀 + JSON 元数据 + 命令流 + 坐标 SoA +
//           CRC32；临时文件 + rename 原子写）
//
// 职责边界：本文件只负责"文件格式 ↔ SceneSnapshot 纯数据"，**不依赖 engine**——
//   命令流段按已序列化的字节原样读写（序列化由 app 层调用
//   engine::SceneCommandList::writeTo/readFrom 完成）。
//   元素符号也以字符串存放（v1 曾在此处查元素表，使 persist 依赖 entity 细节）。
// =============================================================================

#include "persist/mlscene/scene_serializer.h"

#include <array>
#include <cstring>
#include <fstream>

#include <nlohmann/json.hpp>

#include "spdlog/spdlog.h"

namespace moleculens::persist::mlscene {

namespace {

// 极简小端字节写入（替代 engine::ByteSink —— 使 persist 不依赖 engine）。
// 与 cmd.h 的 ByteSink 语义一致：长度字段 u64、其余按原生小端（x86）追加。
struct ByteSinkLike {
    explicit ByteSinkLike(std::vector<uint8_t>& out) : out_(out) {}

    void u64(uint64_t v) { bytes(&v, sizeof(v)); }

    void bytes(const void* data, size_t n) {
        if (data == nullptr || n == 0) {
            return;
        }
        const auto* p = static_cast<const uint8_t*>(data);
        out_.insert(out_.end(), p, p + n);
    }

private:
    std::vector<uint8_t>& out_;
};

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

// 定长 float 数组 ↔ JSON 数组（避免各处手写 for 循环）
template <size_t N>
nlohmann::json toJson(const std::array<float, N>& a) {
    nlohmann::json j = nlohmann::json::array();
    for (float v : a) {
        j.push_back(v);
    }
    return j;
}

template <size_t N>
void fromJson(const nlohmann::json& j, std::array<float, N>& a) {
    if (!j.is_array()) {
        return;
    }
    const size_t n = j.size() < N ? j.size() : N;
    for (size_t k = 0; k < n; ++k) {
        a[k] = j[k].get<float>();
    }
}

// 位姿：具名字段（v2；v1 的 9 个 float 与"平移+四元数+缩放"的注释对不上）
nlohmann::json toJson(const TransformData& t) {
    return {{"translation", toJson(t.translation)},
            {"rotation", toJson(t.rotation)},
            {"scale", t.scale}};
}

void fromJson(const nlohmann::json& j, TransformData& t) {
    if (!j.is_object()) {
        return;   // v1 风格的裸数字数组：忽略（v2 不兼容 v1）
    }
    if (j.contains("translation")) {
        fromJson(j["translation"], t.translation);
    }
    if (j.contains("rotation")) {
        fromJson(j["rotation"], t.rotation);
    }
    t.scale = j.value("scale", 1.0f);
}

} // namespace

entity::Result<void> MlSceneSerializer::save(const SceneSnapshot& snap,
                                             const std::filesystem::path& path,
                                             const SaveOptions& opts) {
    // —— 元数据 JSON（附录 G，v2）——
    nlohmann::json meta;
    meta["version"] = kVersion;
    meta["units"] = snap.units;
    {
        nlohmann::json structures = nlohmann::json::array();
        for (const auto& st : snap.structures) {
            nlohmann::json s;
            nlohmann::json atoms = nlohmann::json::array();
            for (const auto& a : st.atoms) {
                atoms.push_back({{"name", a.name},
                                 {"element", a.element},
                                 {"residueId", a.residueId},
                                 {"chainId", a.chainId},
                                 {"bfactor", a.bfactor},
                                 {"occupancy", a.occupancy}});
            }
            s["atoms"] = atoms;
            nlohmann::json bonds = nlohmann::json::array();
            for (const auto& b : st.bonds) {
                bonds.push_back({b[0], b[1], b[2]});
            }
            s["bonds"] = bonds;
            structures.push_back(s);
        }
        meta["structures"] = structures;
    }
    // 场景级名字池（Atom::chainId / residueId 是这两张表的下标）
    meta["chainNames"] = snap.chainNames;
    meta["residueNames"] = snap.residueNames;
    {
        nlohmann::json systems = nlohmann::json::array();
        for (const auto& sys : snap.systems) {
            systems.push_back({{"name", sys.name},
                               {"visible", sys.visible},
                               {"transform", toJson(sys.transform)},
                               {"renderMode", sys.renderMode}});
        }
        meta["systems"] = systems;
    }
    {
        nlohmann::json instances = nlohmann::json::array();
        for (const auto& inst : snap.instances) {
            instances.push_back(
                {{"structureIndex", inst.structureIndex},
                 {"systemIndex", inst.systemIndex},
                 {"name", inst.name},
                 {"visible", inst.visible},
                 {"isSolvent", inst.isSolvent},
                 {"transform", toJson(inst.transform)},
                 {"trajectory",
                  {{"sourcePath", inst.trajectory.sourcePath},
                   {"sourceSize", inst.trajectory.sourceSize},
                   {"sourceMtime", inst.trajectory.sourceMtime},
                   {"frameCount", inst.trajectory.frameCount},
                   {"dt", inst.trajectory.dt},
                   {"periodic", inst.trajectory.periodic}}}});
        }
        meta["instances"] = instances;
    }
    {
        nlohmann::json reps = nlohmann::json::array();
        for (const auto& r : snap.representations) {
            reps.push_back({{"name", r.name},
                            {"type", r.type},
                            {"selection", r.selection},
                            {"colorScheme", r.colorScheme},
                            {"radiusScale", r.radiusScale},
                            {"bondScale", r.bondScale},
                            {"visible", r.visible},
                            {"systemSurface", r.systemSurface},
                            {"surfIsovalue", r.surfIsovalue},
                            {"surfGridSpacing", r.surfGridSpacing},
                            {"ambient", toJson(r.ambient)},
                            {"diffuse", toJson(r.diffuse)},
                            {"specular", toJson(r.specular)},
                            {"shininess", r.shininess},
                            {"metallic", r.metallic},
                            {"roughness", r.roughness},
                            {"opacity", r.opacity}});
        }
        meta["representations"] = reps;
    }
    meta["scene"] = {{"bgColor", toJson(snap.bgColor)},
                     {"fog", toJson(snap.fog)},
                     {"fogMode", snap.fogMode},
                     {"fogDensity", snap.fogDensity},
                     {"ambient", snap.ambient},
                     {"scale", snap.scale},
                     {"skyColor", toJson(snap.skyColor)},
                     {"groundColor", toJson(snap.groundColor)},
                     {"ambientColor", toJson(snap.ambientColor)}};
    meta["camera"] = {{"target", toJson(snap.cameraTarget)},
                      {"distance", snap.cameraDistance},
                      {"yaw", snap.cameraYaw},
                      {"pitch", snap.cameraPitch},
                      {"roll", snap.cameraRoll},
                      {"fov", snap.cameraFov},
                      {"ortho", snap.cameraOrtho}};
    const std::string metaStr = meta.dump(opts.jsonIndent);

    // —— 命令流段：写调用方提供的**已序列化字节**（v1 曾在此处新建空 SceneCommandList
    //    喂给 writeTo，导致 snap.commandStream 被完全忽略、永远存不进文件）——
    const std::vector<uint8_t>& cmdBytes = snap.commandStream;

    // —— 坐标 SoA 段（附录 G 布局）——
    std::vector<uint8_t> coordsBytes;
    if (opts.writeCoords) {
        ByteSinkLike sink(coordsBytes);
        const uint64_t posCount = snap.coordsPositions.size() / 3;
        const uint64_t colorCount = snap.coordsColors.size() / 4;
        const uint64_t idCount = snap.coordsAtomIds.size();
        sink.u64(posCount);
        sink.bytes(snap.coordsPositions.data(),
                   snap.coordsPositions.size() * sizeof(float));
        sink.u64(colorCount);
        sink.bytes(snap.coordsColors.data(), snap.coordsColors.size() * sizeof(float));
        sink.u64(idCount);
        sink.bytes(snap.coordsAtomIds.data(),
                   snap.coordsAtomIds.size() * sizeof(uint32_t));
    }

    // —— 组装文件（临时文件 + fsync + rename，主方案 §12.1）——
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
    spdlog::info("[persist] .mlscene saved: {} ({} structures, {} systems, {} instances)",
                 path.string(), snap.structures.size(), snap.systems.size(),
                 snap.instances.size());
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
                std::to_string(kVersion) + "（v2 不兼容 v1，请用旧版本另存）");
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
            if (st.contains("atoms")) {
                for (const auto& a : st["atoms"]) {
                    SceneSnapshot::Atom atom;
                    atom.name = a.value("name", "");
                    atom.element = a.value("element", "");
                    atom.residueId = a.value("residueId", 0);
                    atom.chainId = a.value("chainId", 0);
                    atom.bfactor = a.value("bfactor", 0.0f);
                    atom.occupancy = a.value("occupancy", 1.0f);
                    s.atoms.push_back(std::move(atom));
                }
            }
            for (const auto& b : st["bonds"]) {
                s.bonds.push_back({b[0].get<uint32_t>(), b[1].get<uint32_t>(),
                                   b[2].get<uint32_t>()});
            }
            snap.structures.push_back(std::move(s));
        }
        if (meta.contains("chainNames")) {
            snap.chainNames = meta["chainNames"].get<std::vector<std::string>>();
        }
        if (meta.contains("residueNames")) {
            snap.residueNames = meta["residueNames"].get<std::vector<std::string>>();
        }
        if (meta.contains("systems")) {
            for (const auto& y : meta["systems"]) {
                SceneSnapshot::System sys;
                sys.name = y.value("name", "");
                sys.visible = y.value("visible", true);
                if (y.contains("transform")) {
                    fromJson(y["transform"], sys.transform);
                }
                // nlohmann::value 返回 int，显式窄化避免 C4244
                sys.renderMode = static_cast<uint8_t>(y.value("renderMode", 0));
                snap.systems.push_back(std::move(sys));
            }
        }
        for (const auto& i : meta["instances"]) {
            SceneSnapshot::Instance inst;
            inst.structureIndex = i.value("structureIndex", 0u);
            inst.systemIndex = i.value("systemIndex", -1);
            inst.name = i.value("name", "");
            inst.visible = i.value("visible", true);
            inst.isSolvent = i.value("isSolvent", false);
            if (i.contains("transform")) {
                fromJson(i["transform"], inst.transform);
            }
            if (i.contains("trajectory")) {
                const auto& t = i["trajectory"];
                inst.trajectory.sourcePath = t.value("sourcePath", "");
                inst.trajectory.sourceSize = t.value("sourceSize", 0ull);
                inst.trajectory.sourceMtime = t.value("sourceMtime", static_cast<int64_t>(0));
                inst.trajectory.frameCount = t.value("frameCount", 1ull);
                inst.trajectory.dt = t.value("dt", 2.0f);
                inst.trajectory.periodic = t.value("periodic", false);
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
                rep.bondScale = r.value("bondScale", 1.0f);
                rep.visible = r.value("visible", true);
                rep.systemSurface = r.value("systemSurface", false);
                rep.surfIsovalue = r.value("surfIsovalue", 0.5f);
                rep.surfGridSpacing = r.value("surfGridSpacing", 0.5f);
                if (r.contains("ambient")) fromJson(r["ambient"], rep.ambient);
                if (r.contains("diffuse")) fromJson(r["diffuse"], rep.diffuse);
                if (r.contains("specular")) fromJson(r["specular"], rep.specular);
                rep.shininess = r.value("shininess", 32.0f);
                rep.metallic = r.value("metallic", 0.0f);
                rep.roughness = r.value("roughness", 0.40f);
                rep.opacity = r.value("opacity", 1.0f);
                snap.representations.push_back(std::move(rep));
            }
        }
        if (meta.contains("scene")) {
            const auto& sc = meta["scene"];
            if (sc.contains("bgColor")) fromJson(sc["bgColor"], snap.bgColor);
            if (sc.contains("fog")) fromJson(sc["fog"], snap.fog);
            if (sc.contains("skyColor")) fromJson(sc["skyColor"], snap.skyColor);
            if (sc.contains("groundColor")) fromJson(sc["groundColor"], snap.groundColor);
            if (sc.contains("ambientColor")) fromJson(sc["ambientColor"], snap.ambientColor);
            snap.fogMode = sc.value("fogMode", 0.0f);
            snap.fogDensity = sc.value("fogDensity", 0.05f);
            snap.ambient = sc.value("ambient", 0.45f);
            snap.scale = sc.value("scale", 1.0f);
        }
        if (meta.contains("camera")) {
            const auto& c = meta["camera"];
            if (c.contains("target")) fromJson(c["target"], snap.cameraTarget);
            snap.cameraDistance = c.value("distance", 40.0f);
            snap.cameraYaw = c.value("yaw", 0.0f);
            snap.cameraPitch = c.value("pitch", 0.5f);
            snap.cameraRoll = c.value("roll", 0.0f);
            snap.cameraFov = c.value("fov", 45.0f);
            snap.cameraOrtho = c.value("ortho", false);
        }
    } catch (const std::exception& e) {
        return entity::Result<SceneSnapshot>::error(
            entity::ErrorCode::ParseError, std::string("meta JSON: ") + e.what());
    }

    // 命令流段（原样字节；反序列化由 app 层用 SceneCommandList::readFrom 完成）
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
    spdlog::info("[persist] .mlscene loaded: {} ({} structures, {} systems, {} instances)",
                 path.string(), snap.structures.size(), snap.systems.size(),
                 snap.instances.size());
    return entity::Result<SceneSnapshot>::ok(std::move(snap));
}

} // namespace moleculens::persist::mlscene
