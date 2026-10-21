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
    void u32(uint32_t v) { bytes(&v, sizeof(v)); }
    void u8(uint8_t v) { bytes(&v, sizeof(v)); }

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
            // 轨迹引用：可选键只在非默认值时写 —— 链接式保存的 JSON 与旧版
            // 逐字节一致（旧文件没有这些键，读侧按默认 linked/-1/0 处理）。
            nlohmann::json traj = {{"sourcePath", inst.trajectory.sourcePath},
                                   {"sourceSize", inst.trajectory.sourceSize},
                                   {"sourceMtime", inst.trajectory.sourceMtime},
                                   {"frameCount", inst.trajectory.frameCount},
                                   {"dt", inst.trajectory.dt},
                                   {"periodic", inst.trajectory.periodic}};
            if (inst.trajectory.mode != "linked") {
                traj["mode"] = inst.trajectory.mode;
            }
            if (inst.trajectory.embeddedIndex >= 0) {
                traj["embeddedIndex"] = inst.trajectory.embeddedIndex;
            }
            if (inst.trajectory.trajAtomOffset != 0) {
                traj["trajAtomOffset"] = inst.trajectory.trajAtomOffset;
            }
            instances.push_back(
                {{"structureIndex", inst.structureIndex},
                 {"systemIndex", inst.systemIndex},
                 {"name", inst.name},
                 {"visible", inst.visible},
                 {"isSolvent", inst.isSolvent},
                 {"transform", toJson(inst.transform)},
                 {"trajectory", traj}});
        }
        meta["instances"] = instances;
    }
    // 内嵌轨迹段是否写入：**保存选项与快照两张表同时满足**才写。
    // 只写 JSON 形状而不写二进制段会产生"能打开但没有帧"的半截文件，故二者同源。
    const bool writeTraj =
        opts.embedTrajectories && !snap.embeddedTrajectories.empty();
    if (writeTraj) {
        // JSON 只存形状（帧数/原子数/dt/periodic/是否有盒子），帧坐标本体在
        // 二进制段 —— 逐帧 6452×3 个 JSON 数字会让元数据膨胀到数十 MB。
        nlohmann::json et = nlohmann::json::array();
        for (const auto& e : snap.embeddedTrajectories) {
            et.push_back({{"frameCount", e.frameCount},
                          {"atomCount", e.atomCount},
                          {"dt", e.dt},
                          {"periodic", e.periodic},
                          {"hasBoxes", !e.boxes.empty()}});
        }
        meta["embeddedTrajectories"] = et;
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
                            {"espDensityCubePath", r.espDensityCubePath},
                            {"espPotentialCubePath", r.espPotentialCubePath},
                            {"espIsovalue", r.espIsovalue},
                            {"espVminKcal", r.espVminKcal},
                            {"espVmaxKcal", r.espVmaxKcal},
                            {"espAutoRange", r.espAutoRange},
                            {"espInvert", r.espInvert},
                            {"espColormap", r.espColormap},
                            {"materialName", r.materialName},
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
                     {"ambientColor", toJson(snap.ambientColor)},
                     // 方向光：v2 内的可选新增键（旧文件缺键 → 读侧取默认值）
                     {"dirLightDir", toJson(snap.dirLightDir)},
                     {"dirLightColor", toJson(snap.dirLightColor)},
                     {"dirLightIntensity", snap.dirLightIntensity},
                     {"dirLightEnabled", snap.dirLightEnabled},
                     {"shadowsEnabled", snap.shadowsEnabled},
                     {"shadowStrength", snap.shadowStrength}};
    meta["camera"] = {{"target", toJson(snap.cameraTarget)},
                      {"distance", snap.cameraDistance},
                      {"yaw", snap.cameraYaw},
                      {"pitch", snap.cameraPitch},
                      {"roll", snap.cameraRoll},
                      {"fov", snap.cameraFov},
                      {"ortho", snap.cameraOrtho}};
    // 场景标注（v2.3，可选键）：空标注不写，旧文件的 JSON 键集合因此保持不变。
    if (!snap.annotations.empty()) {
        const auto& an = snap.annotations;
        nlohmann::json ann;
        ann["box"] = {{"visible", an.box.visible},
                      {"origin", toJson(an.box.origin)},
                      {"axes", toJson(an.box.axes)},
                      {"color", toJson(an.box.color)},
                      {"lineWidth", an.box.lineWidth}};
        nlohmann::json labels = nlohmann::json::array();
        for (const auto& l : an.labels) {
            labels.push_back({{"text", l.text},
                              {"pos", toJson(l.pos)},
                              {"color", toJson(l.color)},
                              {"size", l.pixelSize},
                              {"leader", l.leader},
                              {"visible", l.visible}});
        }
        ann["labels"] = labels;
        nlohmann::json arrows = nlohmann::json::array();
        for (const auto& a : an.arrows) {
            arrows.push_back({{"from", toJson(a.from)},
                              {"to", toJson(a.to)},
                              {"color", toJson(a.color)},
                              {"width", a.width},
                              {"headScale", a.headScale},
                              {"doubleHead", a.doubleHead},
                              {"style", a.style},
                              {"visible", a.visible}});
        }
        ann["arrows"] = arrows;
        ann["watermark"] = {{"visible", an.watermark.visible},
                            {"text", an.watermark.text},
                            {"color", toJson(an.watermark.color)},
                            {"scale", an.watermark.scale},
                            {"anchor", an.watermark.anchor}};
        meta["annotations"] = ann;
    }
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

    // —— 内嵌轨迹段（可选；布局见 mlscene.h 顶部注释）——
    std::vector<uint8_t> trajBytes;
    if (writeTraj) {
        ByteSinkLike sink(trajBytes);
        sink.u32(static_cast<uint32_t>(snap.embeddedTrajectories.size()));
        for (size_t ti = 0; ti < snap.embeddedTrajectories.size(); ++ti) {
            const auto& e = snap.embeddedTrajectories[ti];
            const uint64_t wantPos =
                e.frameCount * static_cast<uint64_t>(e.atomCount) * 3ull;
            const bool hasBoxes = !e.boxes.empty();
            if (e.positions.size() != wantPos ||
                (hasBoxes && e.boxes.size() != e.frameCount * 9ull)) {
                // 形状与数据长度不一致时拒绝写出：否则读侧只能截断或静默丢帧，
                // 保存"成功"但重开少帧比保存失败更难排查。
                return entity::Result<void>::error(
                    entity::ErrorCode::FormatError,
                    "embedded trajectory " + std::to_string(ti) +
                        " inconsistent: frameCount*atomCount*3=" +
                        std::to_string(wantPos) + ", positions=" +
                        std::to_string(e.positions.size()));
            }
            sink.u64(e.frameCount);
            sink.u32(e.atomCount);
            sink.u32(hasBoxes ? 1u : 0u);
            sink.bytes(e.positions.data(), e.positions.size() * sizeof(float));
            if (hasBoxes) {
                sink.bytes(e.boxes.data(), e.boxes.size() * sizeof(float));
            }
        }
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

        // 可选内嵌轨迹段：链接式保存时不写 → 旧版文件布局（坐标段后直接 CRC）
        if (writeTraj) {
            const uint64_t trajSize = trajBytes.size();
            out.write(reinterpret_cast<const char*>(&trajSize), 8);
            out.write(reinterpret_cast<const char*>(trajBytes.data()),
                      static_cast<std::streamsize>(trajBytes.size()));
        }

        // CRC32 占位 4 字节：写完文件后重新打开、回填 [0, filesize-4) 的 CRC
        // （见下方"回填 CRC"块；读侧在 load() 里校验，篡改 1 字节即加载失败）。
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
                // 可选键：旧文件没有 → 默认 linked / -1 / 0（行为与旧版一致）
                inst.trajectory.mode = t.value("mode", std::string("linked"));
                if (inst.trajectory.mode != "linked" &&
                    inst.trajectory.mode != "embedded") {
                    inst.trajectory.mode = "linked";   // 未知值按旧语义处理
                }
                inst.trajectory.embeddedIndex = t.value("embeddedIndex", -1);
                inst.trajectory.trajAtomOffset = t.value("trajAtomOffset", 0u);
            }
            snap.instances.push_back(std::move(inst));
        }
        if (meta.contains("embeddedTrajectories")) {
            for (const auto& e : meta["embeddedTrajectories"]) {
                SceneSnapshot::EmbeddedTrajectory et;
                et.frameCount = e.value("frameCount", 0ull);
                et.atomCount = e.value("atomCount", 0u);
                et.dt = e.value("dt", 2.0f);
                et.periodic = e.value("periodic", false);
                // 这里只取形状（dt/periodic 只存在 JSON 里）；帧坐标本体在二进制
                // traj 段，由下面统一填充。**不按 JSON 计数预分配**，避免损坏文件
                // 用一个巨大的 frameCount*atomCount 让加载时分配失控。
                snap.embeddedTrajectories.push_back(std::move(et));
            }
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
                rep.espDensityCubePath = r.value("espDensityCubePath", std::string());
                rep.espPotentialCubePath = r.value("espPotentialCubePath", std::string());
                rep.espIsovalue = r.value("espIsovalue", 0.001f);
                rep.espVminKcal = r.value("espVminKcal", -45.0f);
                rep.espVmaxKcal = r.value("espVmaxKcal", 45.0f);
                rep.espAutoRange = r.value("espAutoRange", true);
                rep.espInvert = r.value("espInvert", false);
                rep.espColormap = r.value("espColormap", 0);
                rep.materialName = r.value("materialName", std::string());
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
            if (sc.contains("dirLightDir")) fromJson(sc["dirLightDir"], snap.dirLightDir);
            if (sc.contains("dirLightColor")) {
                fromJson(sc["dirLightColor"], snap.dirLightColor);
            }
            snap.fogMode = sc.value("fogMode", 0.0f);
            snap.fogDensity = sc.value("fogDensity", 0.05f);
            snap.ambient = sc.value("ambient", 0.45f);
            snap.scale = sc.value("scale", 1.0f);
            snap.dirLightIntensity = sc.value("dirLightIntensity", 0.35f);
            snap.dirLightEnabled = sc.value("dirLightEnabled", true);
            snap.shadowsEnabled = sc.value("shadowsEnabled", true);
            snap.shadowStrength = sc.value("shadowStrength", 0.65f);
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
        // 场景标注（v2.3，可选键）：缺键 = 空标注
        if (meta.contains("annotations")) {
            const auto& an = meta["annotations"];
            if (an.contains("box")) {
                const auto& bx = an["box"];
                snap.annotations.box.visible = bx.value("visible", false);
                if (bx.contains("origin")) fromJson(bx["origin"], snap.annotations.box.origin);
                if (bx.contains("axes")) fromJson(bx["axes"], snap.annotations.box.axes);
                if (bx.contains("color")) fromJson(bx["color"], snap.annotations.box.color);
                snap.annotations.box.lineWidth = bx.value("lineWidth", 1.4f);
            }
            if (an.contains("labels")) {
                for (const auto& l : an["labels"]) {
                    AnnotationLabel label;
                    label.text = l.value("text", "");
                    if (l.contains("pos")) fromJson(l["pos"], label.pos);
                    if (l.contains("color")) fromJson(l["color"], label.color);
                    label.pixelSize = l.value("size", 18.0f);
                    label.leader = l.value("leader", true);
                    label.visible = l.value("visible", true);
                    if (!label.text.empty()) {
                        snap.annotations.labels.push_back(std::move(label));
                    }
                }
            }
            if (an.contains("arrows")) {
                for (const auto& a : an["arrows"]) {
                    AnnotationArrow arrow;
                    if (a.contains("from")) fromJson(a["from"], arrow.from);
                    if (a.contains("to")) fromJson(a["to"], arrow.to);
                    if (a.contains("color")) fromJson(a["color"], arrow.color);
                    arrow.width = a.value("width", 2.2f);
                    arrow.headScale = a.value("headScale", 1.0f);
                    arrow.doubleHead = a.value("doubleHead", false);
                    arrow.style = a.value("style", 0);
                    arrow.visible = a.value("visible", true);
                    snap.annotations.arrows.push_back(std::move(arrow));
                }
            }
            if (an.contains("watermark")) {
                const auto& wm = an["watermark"];
                snap.annotations.watermark.visible = wm.value("visible", false);
                snap.annotations.watermark.text = wm.value("text", std::string());
                if (wm.contains("color")) {
                    fromJson(wm["color"], snap.annotations.watermark.color);
                }
                snap.annotations.watermark.scale = wm.value("scale", 0.10f);
                snap.annotations.watermark.anchor = wm.value("anchor", 0);
            }
        }
    } catch (const std::exception& e) {
        return entity::Result<SceneSnapshot>::error(
            entity::ErrorCode::ParseError, std::string("meta JSON: ") + e.what());
    }

    // 尾部 4 字节是 CRC 自身，可选段只允许出现在它之前
    const size_t bodyEnd = all.size() - 4;

    // 命令流段（原样字节；反序列化由 app 层用 SceneCommandList::readFrom 完成）
    if (pos + 8 <= bodyEnd) {
        uint64_t cmdSize = 0;
        std::memcpy(&cmdSize, all.data() + pos, 8);
        pos += 8;
        if (cmdSize <= bodyEnd - pos) {
            snap.commandStream.assign(all.data() + pos, all.data() + pos + cmdSize);
            pos += static_cast<size_t>(cmdSize);
        } else if (opts.strict) {
            return entity::Result<SceneSnapshot>::error(
                entity::ErrorCode::FormatError, "truncated command stream");
        } else {
            pos = bodyEnd;   // 无法定位后续段，按"无坐标/无轨迹"处理
        }
    }

    // 坐标 SoA 段（可选；lazyCoords 只跳过**解析**，仍须跨过该段以定位内嵌轨迹段）
    if (pos + 8 <= bodyEnd) {
        uint64_t coordsSize = 0;
        std::memcpy(&coordsSize, all.data() + pos, 8);
        pos += 8;
        if (coordsSize <= bodyEnd - pos) {
            if (!opts.lazyCoords) {
                const uint8_t* cp = all.data() + pos;
                const uint8_t* end = cp + coordsSize;
                uint64_t posCount = 0, colorCount = 0, idCount = 0;
                if (cp + 8 <= end) {
                    std::memcpy(&posCount, cp, 8);
                    cp += 8;
                }
                if (posCount <= static_cast<uint64_t>(end - cp) / 12) {
                    snap.coordsPositions.resize(static_cast<size_t>(posCount) * 3);
                    std::memcpy(snap.coordsPositions.data(), cp,
                                static_cast<size_t>(posCount) * 12);
                    cp += static_cast<size_t>(posCount) * 12;
                }
                if (cp + 8 <= end) {
                    std::memcpy(&colorCount, cp, 8);
                    cp += 8;
                }
                if (colorCount <= static_cast<uint64_t>(end - cp) / 16) {
                    snap.coordsColors.resize(static_cast<size_t>(colorCount) * 4);
                    std::memcpy(snap.coordsColors.data(), cp,
                                static_cast<size_t>(colorCount) * 16);
                    cp += static_cast<size_t>(colorCount) * 16;
                }
                if (cp + 8 <= end) {
                    std::memcpy(&idCount, cp, 8);
                    cp += 8;
                }
                if (idCount <= static_cast<uint64_t>(end - cp) / 4) {
                    snap.coordsAtomIds.resize(static_cast<size_t>(idCount));
                    std::memcpy(snap.coordsAtomIds.data(), cp,
                                static_cast<size_t>(idCount) * 4);
                }
                // 约束：posCount == colorCount == idCount（附录 G）
                if (posCount != colorCount || posCount != idCount) {
                    return entity::Result<SceneSnapshot>::error(
                        entity::ErrorCode::FormatError, "coords SoA count mismatch");
                }
            }
            // ★ 必须推进 pos：此前漏了这一行，坐标段之后的可选段永远读不到
            //   （旧文件没有后续段时只是"少读"，加入内嵌轨迹段后就是功能失效）。
            pos += static_cast<size_t>(coordsSize);
        } else if (opts.strict) {
            return entity::Result<SceneSnapshot>::error(
                entity::ErrorCode::FormatError, "truncated coords SoA");
        } else {
            pos = bodyEnd;
        }
    }

    // —— 可选内嵌轨迹段（只有嵌入式保存才存在；旧文件此处直接是 CRC）——
    // 判据：至少还能读出 u64 trajSize，且该长度不越过 CRC。
    if (pos + 8 <= bodyEnd) {
        uint64_t trajSize = 0;
        std::memcpy(&trajSize, all.data() + pos, 8);
        const size_t trajPos = pos + 8;
        if (trajSize <= bodyEnd - trajPos) {
            const uint8_t* tp = all.data() + trajPos;
            const uint8_t* tend = tp + trajSize;
            bool ok = true;
            std::vector<SceneSnapshot::EmbeddedTrajectory> parsed;
            uint32_t trajCount = 0;
            if (tp + 4 <= tend) {
                std::memcpy(&trajCount, tp, 4);
                tp += 4;
            } else {
                ok = false;
            }
            for (uint32_t ti = 0; ok && ti < trajCount; ++ti) {
                SceneSnapshot::EmbeddedTrajectory e;
                if (tp + 16 > tend) {
                    ok = false;
                    break;
                }
                std::memcpy(&e.frameCount, tp, 8);
                tp += 8;
                std::memcpy(&e.atomCount, tp, 4);
                tp += 4;
                uint32_t flags = 0;
                std::memcpy(&flags, tp, 4);
                tp += 4;
                const bool hasBoxes = (flags & 1u) != 0u;
                // 先做乘法溢出检查，再用"剩余字节/元素字节"判界
                if (e.frameCount != 0 &&
                    e.atomCount > UINT64_MAX / e.frameCount) {
                    ok = false;
                    break;
                }
                const uint64_t atomsPerFrame = e.frameCount * e.atomCount;
                if (atomsPerFrame > UINT64_MAX / 3ull) {
                    ok = false;
                    break;
                }
                const uint64_t floatCount = atomsPerFrame * 3ull;
                if (floatCount > static_cast<uint64_t>(tend - tp) / 4) {
                    ok = false;
                    break;
                }
                e.positions.resize(static_cast<size_t>(floatCount));
                if (floatCount > 0) {
                    std::memcpy(e.positions.data(), tp,
                                static_cast<size_t>(floatCount) * 4);
                }
                tp += static_cast<size_t>(floatCount) * 4;
                if (hasBoxes) {
                    if (e.frameCount > static_cast<uint64_t>(tend - tp) / 36) {
                        ok = false;
                        break;
                    }
                    e.boxes.resize(static_cast<size_t>(e.frameCount * 9ull));
                    if (!e.boxes.empty()) {
                        std::memcpy(e.boxes.data(), tp, e.boxes.size() * 4);
                    }
                    tp += static_cast<size_t>(e.frameCount * 9ull) * 4;
                }
                // dt / periodic 只存在 JSON 形状表里（同下标）；二进制段是帧坐标的
                // 唯一事实来源（包含 frameCount/atomCount/是否带盒子）。
                if (ti < snap.embeddedTrajectories.size()) {
                    e.dt = snap.embeddedTrajectories[ti].dt;
                    e.periodic = snap.embeddedTrajectories[ti].periodic;
                }
                parsed.push_back(std::move(e));
            }
            if (ok) {
                if (snap.embeddedTrajectories.size() != parsed.size()) {
                    spdlog::warn("[persist] 内嵌轨迹形状表({}) 与二进制段({}) 数量不一致，"
                                 "以二进制段为准",
                                 snap.embeddedTrajectories.size(), parsed.size());
                }
                snap.embeddedTrajectories = std::move(parsed);
            } else if (opts.strict) {
                return entity::Result<SceneSnapshot>::error(
                    entity::ErrorCode::FormatError, "invalid embedded trajectories");
            } else {
                spdlog::warn("[persist] 内嵌轨迹段损坏，已按 lenient 跳过（{}）",
                             path.string());
                // 保留 JSON 形状但不给帧坐标 → applySnapshot 按"无内嵌帧"处理，
                // 不会把零坐标当成真实轨迹播放。
                for (auto& e : snap.embeddedTrajectories) {
                    e.positions.clear();
                    e.boxes.clear();
                }
            }
            pos = trajPos + static_cast<size_t>(trajSize);
        } else if (opts.strict) {
            return entity::Result<SceneSnapshot>::error(
                entity::ErrorCode::FormatError, "truncated embedded trajectories");
        }
    }
    spdlog::info("[persist] .mlscene loaded: {} ({} structures, {} systems, {} instances)",
                 path.string(), snap.structures.size(), snap.systems.size(),
                 snap.instances.size());
    return entity::Result<SceneSnapshot>::ok(std::move(snap));
}

} // namespace moleculens::persist::mlscene
