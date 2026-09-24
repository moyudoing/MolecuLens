// =============================================================================
// MolecuLens · app · serialize/scene_bridge.cpp —— 场景 ↔ .mlscene 快照桥接实现
// =============================================================================

#include "engine/serialize/scene_bridge.h"

#include <memory>
#include <string>
#include <vector>

#include "engine/camera/camera.h"
#include "engine/cmd/cmd.h"
#include "engine/rep/representation.h"
#include "entity/element/element.h"
#include "entity/molecule/molecule.h"
#include "entity/scene/scene.h"
#include "spdlog/spdlog.h"

namespace moleculens::engine {

namespace {

using persist::mlscene::SceneSnapshot;
using persist::mlscene::TransformData;

// —— 表示类型的**稳定 ASCII 键**（文件格式用；不依赖 UI 显示名，
//    因为 representationTypeName() 返回的是中文显示名，不适合作为持久化键）——
struct RepTypeKey {
    engine::RepresentationType type;
    const char* key;
};
constexpr RepTypeKey kRepTypeKeys[] = {
    {engine::RepresentationType::Wireframe, "wireframe"},
    {engine::RepresentationType::Stick, "stick"},
    {engine::RepresentationType::BallAndStick, "ballstick"},
    {engine::RepresentationType::Spacefill, "spacefill"},
    {engine::RepresentationType::Cartoon, "cartoon"},
    {engine::RepresentationType::Surface, "surface"},
    {engine::RepresentationType::Label, "label"},
    {engine::RepresentationType::Vdw, "vdw"},
    {engine::RepresentationType::QuickSurf, "quicksurf"},
    {engine::RepresentationType::Licorice, "licorice"},
    {engine::RepresentationType::Tube, "tube"},
    {engine::RepresentationType::NewCartoon, "newcartoon"},
    {engine::RepresentationType::Ribbon, "ribbon"},
};

const char* repTypeKey(engine::RepresentationType t) {
    for (const auto& k : kRepTypeKeys) {
        if (k.type == t) {
            return k.key;
        }
    }
    return "ballstick";
}

engine::RepresentationType repTypeFromKey(const std::string& s) {
    for (const auto& k : kRepTypeKeys) {
        if (s == k.key) {
            return k.type;
        }
    }
    return engine::RepresentationType::BallAndStick;
}

struct ColorKey {
    engine::ColorScheme scheme;
    const char* key;
};
constexpr ColorKey kColorKeys[] = {
    {engine::ColorScheme::ElementCPK, "element"},
    {engine::ColorScheme::Chain, "chain"},
    {engine::ColorScheme::Residue, "residue"},
    {engine::ColorScheme::BFactor, "bfactor"},
    {engine::ColorScheme::Uniform, "uniform"},
};

const char* colorKey(engine::ColorScheme c) {
    for (const auto& k : kColorKeys) {
        if (k.scheme == c) {
            return k.key;
        }
    }
    return "element";
}

engine::ColorScheme colorFromKey(const std::string& s) {
    for (const auto& k : kColorKeys) {
        if (s == k.key) {
            return k.scheme;
        }
    }
    return engine::ColorScheme::ElementCPK;
}

TransformData toTransformData(const entity::Transform& t) {
    TransformData d;
    d.translation = {t.translation.x, t.translation.y, t.translation.z};
    d.rotation = {t.rotation.w, t.rotation.x, t.rotation.y, t.rotation.z};
    d.scale = t.scale;
    return d;
}

entity::Transform toTransform(const TransformData& d) {
    entity::Transform t;
    t.translation = glm::vec3(d.translation[0], d.translation[1], d.translation[2]);
    t.rotation = glm::quat(d.rotation[0], d.rotation[1], d.rotation[2], d.rotation[3]);
    t.scale = d.scale;
    return t;
}

} // namespace

// =============================================================================
// 保存方向
// =============================================================================

SceneSnapshot makeSnapshot(const SnapshotSource& in) {
    SceneSnapshot snap;
    const entity::SceneStructure* sc = in.scene;
    if (sc == nullptr) {
        return snap;
    }

    // —— 结构：原子级字段（v1 只有元素符号）——
    for (const auto& ms : sc->structures()) {
        SceneSnapshot::Structure s;
        const size_t n = ms.elements.size();
        s.atoms.reserve(n);
        for (size_t i = 0; i < n; ++i) {
            SceneSnapshot::Atom a;
            if (i < ms.atomNameIds.size()) {
                a.name = std::string(sc->strings().resolve(ms.atomNameIds[i]));
            }
            a.element = entity::elementInfo(ms.elements[i]).symbol;
            if (i < ms.atoms.size()) {
                const auto& at = ms.atoms[i];
                a.residueId = at.residueId;
                a.chainId = at.chainId;
                // 领域模型用定点存储：occupancy 为 0–100，bfactor 为 Å²×100
                a.occupancy = static_cast<float>(at.occupancy) / 100.0f;
                a.bfactor = static_cast<float>(at.bfactor) / 100.0f;
            }
            s.atoms.push_back(std::move(a));
        }
        for (const auto& b : ms.bonds) {
            s.bonds.push_back({b.a, b.b, b.order});
        }
        snap.structures.push_back(std::move(s));
    }

    // —— 场景级名字池 ——
    for (int32_t i = 0; i < sc->chainCount(); ++i) {
        snap.chainNames.emplace_back(sc->chainName(i));
    }
    for (int32_t i = 0; i < sc->residueCount(); ++i) {
        snap.residueNames.emplace_back(sc->residueName(i));
    }

    // —— 体系 ——
    for (const auto& sys : sc->systems()) {
        SceneSnapshot::System y;
        y.name = sys.name;
        y.visible = sys.visible;
        y.transform = toTransformData(sys.transform);
        y.renderMode = static_cast<uint8_t>(sys.renderMode);
        snap.systems.push_back(std::move(y));
    }

    // —— 实例 ——
    for (const auto& inst : sc->instances()) {
        SceneSnapshot::Instance o;
        o.structureIndex = inst.structureIndex;
        o.name = inst.name;
        o.visible = inst.visible;
        o.isSolvent = inst.isSolvent;
        o.transform = toTransformData(inst.transform);
        if (inst.systemId != UINT32_MAX) {
            const uint32_t si = sc->systemIndexById(inst.systemId);
            o.systemIndex = (si == UINT32_MAX) ? -1 : static_cast<int32_t>(si);
        }
        // 轨迹：引用源文件（不内嵌帧坐标）
        if (inst.trajectory) {
            o.trajectory.frameCount = inst.trajectory->frames.size();
            o.trajectory.dt = inst.trajectory->dt;
            o.trajectory.periodic = inst.trajectory->periodic;
        }
        o.trajectory.sourcePath = in.trajectorySourcePath;
        o.trajectory.sourceSize = in.trajectorySourceSize;
        o.trajectory.sourceMtime = in.trajectorySourceMtime;
        snap.instances.push_back(std::move(o));
    }

    // —— 表示 ——
    if (in.representations != nullptr) {
        for (const auto& r : *in.representations) {
            SceneSnapshot::Representation o;
            o.name = r.name;
            o.type = repTypeKey(r.type);
            o.selection = r.selection;
            o.colorScheme = colorKey(r.colorScheme);
            o.radiusScale = r.radiusScale;
            o.bondScale = r.bondScale;
            o.visible = r.visible;
            o.systemSurface = r.systemSurface;
            o.surfIsovalue = r.surfIsovalue;
            o.surfGridSpacing = r.surfGridSpacing;
            o.ambient = {r.ambient[0], r.ambient[1], r.ambient[2], r.ambient[3]};
            o.diffuse = {r.diffuse[0], r.diffuse[1], r.diffuse[2], r.diffuse[3]};
            o.specular = {r.specular[0], r.specular[1], r.specular[2], r.specular[3]};
            o.shininess = r.shininess;
            o.metallic = r.metallic;
            o.roughness = r.roughness;
            o.opacity = r.opacity;
            snap.representations.push_back(std::move(o));
        }
    }

    // —— 场景属性 ——
    const auto& p = sc->properties();
    snap.bgColor = {p.bgColor[0], p.bgColor[1], p.bgColor[2]};
    snap.fog = {p.fog[0], p.fog[1]};
    snap.fogMode = p.fogMode;
    snap.fogDensity = p.fogDensity;
    snap.ambient = p.ambient;
    snap.scale = p.scale;
    snap.skyColor = {p.skyColor[0], p.skyColor[1], p.skyColor[2]};
    snap.groundColor = {p.groundColor[0], p.groundColor[1], p.groundColor[2]};
    snap.ambientColor = {p.ambientColor[0], p.ambientColor[1], p.ambientColor[2]};
    snap.units = (p.units == entity::Units::Nanometer) ? "nanometer" : "angstrom";

    // —— 相机 ——
    if (in.camera != nullptr) {
        const auto& cam = *in.camera;
        snap.cameraTarget = {cam.target.x, cam.target.y, cam.target.z};
        snap.cameraDistance = cam.distance;
        snap.cameraYaw = cam.yaw;
        snap.cameraPitch = cam.pitch;
        snap.cameraRoll = cam.roll;
        snap.cameraFov = cam.fov;
        snap.cameraOrtho = cam.ortho;
    }

    // —— 命令流：由本层序列化（persist 视作不透明字节）——
    if (in.commandStream != nullptr) {
        engine::ByteSink sink(snap.commandStream);
        engine::SceneCommandList::writeTo(sink, *in.commandStream);
    }

    // —— 坐标 SoA ——
    const auto& cs = sc->coordsSoA();
    snap.coordsPositions.reserve(cs.positions.size() * 3);
    for (const auto& v : cs.positions) {
        snap.coordsPositions.push_back(v.x);
        snap.coordsPositions.push_back(v.y);
        snap.coordsPositions.push_back(v.z);
    }
    snap.coordsColors.reserve(cs.colors.size() * 4);
    for (const auto& c : cs.colors) {
        snap.coordsColors.push_back(c.x);
        snap.coordsColors.push_back(c.y);
        snap.coordsColors.push_back(c.z);
        snap.coordsColors.push_back(c.w);
    }
    snap.coordsAtomIds = cs.atomIds;
    return snap;
}

// =============================================================================
// 加载方向
// =============================================================================

void applySnapshot(const SceneSnapshot& snap, const RestoreTarget& out) {
    if (out.representations != nullptr) {
        out.representations->clear();
        for (const auto& r : snap.representations) {
            engine::Representation rep;
            rep.name = r.name;
            rep.type = repTypeFromKey(r.type);
            rep.selection = r.selection;
            rep.colorScheme = colorFromKey(r.colorScheme);
            rep.radiusScale = r.radiusScale;
            rep.bondScale = r.bondScale;
            rep.visible = r.visible;
            rep.systemSurface = r.systemSurface;
            rep.surfIsovalue = r.surfIsovalue;
            rep.surfGridSpacing = r.surfGridSpacing;
            for (int k = 0; k < 4; ++k) {
                rep.ambient[k] = r.ambient[static_cast<size_t>(k)];
                rep.diffuse[k] = r.diffuse[static_cast<size_t>(k)];
                rep.specular[k] = r.specular[static_cast<size_t>(k)];
            }
            rep.shininess = r.shininess;
            rep.metallic = r.metallic;
            rep.roughness = r.roughness;
            rep.opacity = r.opacity;
            out.representations->push_back(std::move(rep));
        }
    }

    if (out.camera != nullptr) {
        auto& cam = *out.camera;
        cam.target = glm::vec3(snap.cameraTarget[0], snap.cameraTarget[1],
                               snap.cameraTarget[2]);
        cam.distance = snap.cameraDistance;
        cam.yaw = snap.cameraYaw;
        cam.pitch = snap.cameraPitch;
        cam.roll = snap.cameraRoll;
        cam.fov = snap.cameraFov;
        cam.ortho = snap.cameraOrtho;
    }

    if (out.commandStream != nullptr && !snap.commandStream.empty()) {
        engine::ByteSource src(snap.commandStream.data(), snap.commandStream.size());
        if (!engine::SceneCommandList::readFrom(src, *out.commandStream)) {
            spdlog::warn("[serialize] 命令流段反序列化失败（场景仍按坐标段重建）");
        }
    }

    if (out.scene == nullptr) {
        return;
    }

    entity::SceneStructure& sc = *out.scene;

    // 1) 名字池：解析器按顺序分配 chainId/residueId（稠密），故按原顺序 add
    //    即可保证重建后的下标与原下标一致
    for (const auto& name : snap.chainNames) {
        sc.addChain(name);
    }
    for (const auto& name : snap.residueNames) {
        sc.addResidue(name);
    }

    // 2) 结构注册（原子名经 StringPool intern）
    std::vector<uint32_t> structureIndex(snap.structures.size(), 0);
    for (size_t si = 0; si < snap.structures.size(); ++si) {
        const auto& s = snap.structures[si];
        entity::MoleculeStructure ms;
        ms.elements.reserve(s.atoms.size());
        ms.atoms.reserve(s.atoms.size());
        ms.atomNameIds.reserve(s.atoms.size());
        for (const auto& a : s.atoms) {
            const entity::ElementType el = entity::elementFromSymbol(a.element.c_str());
            ms.elements.push_back(el);
            entity::Atom at{};
            at.element = el;
            at.occupancy = static_cast<uint8_t>(a.occupancy * 100.0f);
            at.bfactor = static_cast<int16_t>(a.bfactor * 100.0f);
            at.residueId = a.residueId;
            at.chainId = a.chainId;
            ms.atoms.push_back(at);
            ms.atomNameIds.push_back(sc.strings().intern(a.name));
        }
        for (const auto& b : s.bonds) {
            ms.bonds.push_back(entity::Bond{b[0], b[1], static_cast<uint8_t>(b[2])});
        }
        structureIndex[si] = sc.registerStructure(ms);
    }

    // 3) 体系（先建，拿到稳定 id 供实例引用）
    std::vector<uint32_t> systemIds(snap.systems.size(), UINT32_MAX);
    for (size_t yi = 0; yi < snap.systems.size(); ++yi) {
        const auto& y = snap.systems[yi];
        const uint32_t id = sc.addSystem(y.name);
        systemIds[yi] = id;
        sc.setSystemVisible(id, y.visible);
        sc.setSystemTransform(id, toTransform(y.transform));
        sc.setSystemRenderMode(id, static_cast<entity::SystemRenderMode>(y.renderMode));
    }

    // 4) 实例。**注意**：轨迹帧坐标未内嵌（格式只存源文件引用），因此此处按
    //    静态实例恢复；重放轨迹需要读取器按 sourcePath 惰性加载（见 P4 轨迹导入）。
    for (const auto& i : snap.instances) {
        if (i.structureIndex >= structureIndex.size()) {
            continue;
        }
        const uint32_t sid = (i.systemIndex >= 0 &&
                              static_cast<size_t>(i.systemIndex) < systemIds.size())
                                 ? systemIds[static_cast<size_t>(i.systemIndex)]
                                 : UINT32_MAX;
        // 单帧静态轨迹：坐标段由场景自身维护（coordsSoA），此处只登记拓扑与元数据
        auto traj = std::make_shared<entity::Trajectory>();
        traj->dt = i.trajectory.dt;
        traj->periodic = i.trajectory.periodic;
        traj->frames.resize(1);
        sc.addTrajectoryInstance(structureIndex[i.structureIndex], toTransform(i.transform),
                                 traj, sid, i.name, i.isSolvent);
    }

    if (!snap.instances.empty() && !snap.instances[0].trajectory.sourcePath.empty()) {
        spdlog::info("[serialize] 轨迹源引用: {}（帧数据未内嵌，重放需按需加载）",
                     snap.instances[0].trajectory.sourcePath);
    }

    // 5) 场景属性
    auto& p = sc.properties();
    for (int k = 0; k < 3; ++k) {
        p.bgColor[k] = snap.bgColor[static_cast<size_t>(k)];
        p.skyColor[k] = snap.skyColor[static_cast<size_t>(k)];
        p.groundColor[k] = snap.groundColor[static_cast<size_t>(k)];
        p.ambientColor[k] = snap.ambientColor[static_cast<size_t>(k)];
    }
    p.fog[0] = snap.fog[0];
    p.fog[1] = snap.fog[1];
    p.fogMode = snap.fogMode;
    p.fogDensity = snap.fogDensity;
    p.ambient = snap.ambient;
    p.scale = snap.scale;
    p.units = (snap.units == "nanometer") ? entity::Units::Nanometer
                                          : entity::Units::Angstrom;
    sc.setDirty(entity::DIRTY_ALL);
    sc.rebuildCoordsSoA();
}

} // namespace moleculens::engine
