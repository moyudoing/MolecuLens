// =============================================================================
// MolecuLens · app · serialize/scene_bridge.cpp —— 场景 ↔ .mlscene 快照桥接实现
// =============================================================================

#include "engine/serialize/scene_bridge.h"

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "engine/camera/camera.h"
#include "engine/cmd/cmd.h"
#include "engine/render/render_parameter.h"   // kMaterialPresets / materialPresetAt
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
// 兼容说明：旧 .mlscene 可能含已移除的 Label 表示键 "label" → repTypeFromKey()
// 未命中时回退 BallAndStick（见下方默认返回），旧文件仍可正常打开。
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
    {engine::RepresentationType::Vdw, "vdw"},
    {engine::RepresentationType::QuickSurf, "quicksurf"},
    {engine::RepresentationType::Licorice, "licorice"},
    {engine::RepresentationType::Tube, "tube"},
    {engine::RepresentationType::NewCartoon, "newcartoon"},
    {engine::RepresentationType::Ribbon, "ribbon"},
    {engine::RepresentationType::EspSurface, "espsurface"},
    {engine::RepresentationType::Plate, "plate"},
    {engine::RepresentationType::Diamond, "diamond"},
    {engine::RepresentationType::DotCloud, "dotcloud"},
    {engine::RepresentationType::Polyhedron, "polyhedron"},
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
    {engine::ColorScheme::AtomName, "name"},
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

// 从整体系轨迹里切出某个实例的原子段，复制成一帧数相同的**独立**轨迹。
// 只在无法用 setSystemTrajectory 统一挂载（体系成员混合了别的轨迹）时兜底 ——
// 正常路径（整体系共享）不复制，避免把整体系坐标按实例数翻倍。
std::shared_ptr<entity::Trajectory> sliceTrajectory(
    const std::shared_ptr<entity::Trajectory>& src, uint32_t offset, uint32_t atomCount) {
    auto out = std::make_shared<entity::Trajectory>();
    if (!src) {
        out->frames.resize(1);
        return out;
    }
    out->dt = src->dt;
    out->periodic = src->periodic;
    out->frames.resize(src->frames.size());
    for (size_t f = 0; f < src->frames.size(); ++f) {
        const auto& sp = src->frames[f].positions;
        auto& dp = out->frames[f];
        dp.box = src->frames[f].box;
        dp.positions.resize(atomCount, glm::vec3(0.0f));
        for (uint32_t k = 0; k < atomCount; ++k) {
            const size_t s = static_cast<size_t>(offset) + k;
            if (s < sp.size()) {
                dp.positions[k] = sp[s];
            }
        }
    }
    return out;
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
    // 内嵌收集表：**同一个 shared_ptr<Trajectory> 只内嵌一次**。
    // 实测 data/md.gro 一个体系由 2042 个实例共享同一条整体系轨迹，逐实例复制
    // 会把 1001 帧 × 6452 原子的坐标存 2042 份（数百 MB → 数十 GB）。
    std::unordered_map<const entity::Trajectory*, int32_t> embeddedIndexByTraj;
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
        // 轨迹：链接式只记源文件引用（帧坐标不内嵌）；嵌入式把帧坐标收进快照
        if (inst.trajectory) {
            o.trajectory.frameCount = inst.trajectory->frames.size();
            o.trajectory.dt = inst.trajectory->dt;
            o.trajectory.periodic = inst.trajectory->periodic;
        }
        o.trajectory.sourcePath = in.trajectorySourcePath;
        o.trajectory.sourceSize = in.trajectorySourceSize;
        o.trajectory.sourceMtime = in.trajectorySourceMtime;
        if (in.embedTrajectories && inst.trajectory &&
            !inst.trajectory->frames.empty() &&
            !inst.trajectory->frames[0].positions.empty()) {
            // 首帧没有坐标 = 没有可内嵌的帧数据（静态实例），保持链接式语义，
            // 避免写出 atomCount=0 的空轨迹条目。
            const entity::Trajectory* key = inst.trajectory.get();
            auto it = embeddedIndexByTraj.find(key);
            if (it == embeddedIndexByTraj.end()) {
                SceneSnapshot::EmbeddedTrajectory e;
                const auto& frames = inst.trajectory->frames;
                e.frameCount = frames.size();
                // atomCount 取首帧坐标数：体系级共享轨迹时即体系总原子数
                e.atomCount = static_cast<uint32_t>(frames[0].positions.size());
                e.dt = inst.trajectory->dt;
                e.periodic = inst.trajectory->periodic;
                e.positions.reserve(static_cast<size_t>(e.frameCount) * e.atomCount * 3u);
                bool anyBox = false;
                for (const auto& fr : frames) {
                    for (const auto& p : fr.positions) {
                        e.positions.push_back(p.x);
                        e.positions.push_back(p.y);
                        e.positions.push_back(p.z);
                    }
                    for (float v : fr.box) {
                        if (v != 0.0f) {
                            anyBox = true;
                        }
                    }
                }
                // 盒子可空：只有真的带盒子的轨迹才存（逐帧 9 个 float）
                if (anyBox) {
                    e.boxes.reserve(static_cast<size_t>(e.frameCount) * 9u);
                    for (const auto& fr : frames) {
                        for (float v : fr.box) {
                            e.boxes.push_back(v);
                        }
                    }
                }
                const int32_t idx =
                    static_cast<int32_t>(snap.embeddedTrajectories.size());
                snap.embeddedTrajectories.push_back(std::move(e));
                it = embeddedIndexByTraj.emplace(key, idx).first;
            }
            o.trajectory.mode = "embedded";
            o.trajectory.embeddedIndex = it->second;
            // 体系级共享轨迹：本实例在整体系帧坐标里的原子偏移
            o.trajectory.trajAtomOffset = inst.trajAtomOffset;
        }
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
            o.espDensityCubePath = r.espDensityCubePath;
            o.espPotentialCubePath = r.espPotentialCubePath;
            o.espIsovalue = r.espIsovalue;
            o.espVminKcal = r.espVminKcal;
            o.espVmaxKcal = r.espVmaxKcal;
            o.espAutoRange = r.espAutoRange;
            o.espInvert = r.espInvert;
            o.espColormap = r.espColormap;
            // 材质名（稳定键）：仅当确实是某个预设时才写名字；自定义留空，
            // 载入时按数值原样恢复（见反向映射）
            o.materialName = (r.materialPreset >= 0 &&
                              r.materialPreset < engine::kMaterialPresetCount)
                                 ? engine::materialPresetAt(r.materialPreset).name
                                 : std::string();
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
    snap.dirLightDir = {p.dirLightDir[0], p.dirLightDir[1], p.dirLightDir[2]};
    snap.dirLightColor = {p.dirLightColor[0], p.dirLightColor[1], p.dirLightColor[2]};
    snap.dirLightIntensity = p.dirLightIntensity;
    snap.dirLightEnabled = p.dirLightEnabled;
    snap.shadowsEnabled = p.shadowsEnabled;
    snap.shadowStrength = p.shadowStrength;
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
    // —— 场景标注（v2.3）：app 层提供的纯数据，本层只做拷贝 ——
    if (in.annotations != nullptr) {
        snap.annotations = *in.annotations;
    }
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
            rep.espDensityCubePath = r.espDensityCubePath;
            rep.espPotentialCubePath = r.espPotentialCubePath;
            rep.espIsovalue = r.espIsovalue;
            rep.espVminKcal = r.espVminKcal;
            rep.espVmaxKcal = r.espVmaxKcal;
            rep.espAutoRange = r.espAutoRange;
            rep.espInvert = r.espInvert;
            rep.espColormap = r.espColormap;
            for (int k = 0; k < 4; ++k) {
                rep.ambient[k] = r.ambient[static_cast<size_t>(k)];
                rep.diffuse[k] = r.diffuse[static_cast<size_t>(k)];
                rep.specular[k] = r.specular[static_cast<size_t>(k)];
            }
            rep.shininess = r.shininess;
            rep.metallic = r.metallic;
            rep.roughness = r.roughness;
            rep.opacity = r.opacity;
            // 材质名 → 预设下标；名字未知/为空（自定义或旧文件）→ -1，
            // 数值已由上面逐字段恢复，观感不变，下拉显示"自定义"
            rep.materialPreset = engine::materialPresetIndexByName(
                r.materialName.empty() ? nullptr : r.materialName.c_str());
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

    // 4) 实例。
    // 链接式：格式只存源文件引用 → 恢复成**单帧静态实例**（重放需按 sourcePath
    //         惰性加载，行为与旧版一致）。
    // 嵌入式：先从快照内嵌表重建 Trajectory（同一 embeddedIndex 只重建一次），
    //         再按引用数决定挂载方式：单实例 → 直接挂；多实例共享 → 全部加完后
    //         用 setSystemTrajectory 统一挂载，使"整体系同步播放"成为结构必然。
    std::vector<std::shared_ptr<entity::Trajectory>> embeddedTrajs(
        snap.embeddedTrajectories.size());
    for (size_t k = 0; k < snap.embeddedTrajectories.size(); ++k) {
        const auto& e = snap.embeddedTrajectories[k];
        const size_t want = static_cast<size_t>(e.frameCount) * e.atomCount * 3u;
        if (e.frameCount == 0 || e.atomCount == 0 || e.positions.size() != want) {
            // 形状与数据不一致（旧文件半截/损坏）：不重建，实例退回静态单帧
            continue;
        }
        auto traj = std::make_shared<entity::Trajectory>();
        traj->dt = e.dt;
        traj->periodic = e.periodic;
        traj->frames.resize(static_cast<size_t>(e.frameCount));
        for (size_t f = 0; f < static_cast<size_t>(e.frameCount); ++f) {
            auto& fr = traj->frames[f];
            fr.positions.resize(e.atomCount);
            const size_t base = f * static_cast<size_t>(e.atomCount) * 3u;
            for (uint32_t a = 0; a < e.atomCount; ++a) {
                const size_t o = base + static_cast<size_t>(a) * 3u;
                fr.positions[a] =
                    glm::vec3(e.positions[o], e.positions[o + 1], e.positions[o + 2]);
            }
            if (e.boxes.size() >= (f + 1) * 9u) {
                for (int b = 0; b < 9; ++b) {
                    fr.box[static_cast<size_t>(b)] = e.boxes[f * 9u + static_cast<size_t>(b)];
                }
            }
        }
        embeddedTrajs[k] = std::move(traj);
    }
    // 每个 embeddedIndex 被多少实例引用（单实例 vs 体系级共享）
    std::vector<int32_t> embedRefCount(snap.embeddedTrajectories.size(), 0);
    for (const auto& i : snap.instances) {
        const int32_t ei = i.trajectory.embeddedIndex;
        if (ei >= 0 && static_cast<size_t>(ei) < embeddedTrajs.size() &&
            embeddedTrajs[static_cast<size_t>(ei)] != nullptr) {
            ++embedRefCount[static_cast<size_t>(ei)];
        }
    }

    // 体系级共享轨迹的挂载要等**全部实例**加完（setSystemTrajectory 按几何顺序
    // 重新分配偏移，必须看到完整成员表）。先记录待处理系统。
    struct PendingSystem {
        struct Member {
            uint32_t instanceIndex = 0;
            uint32_t atomOffset = 0;
            std::shared_ptr<entity::Trajectory> traj;   // 该实例自己的共享轨迹
        };
        uint32_t systemId = UINT32_MAX;
        std::shared_ptr<entity::Trajectory> traj;       // 首个成员的内嵌轨迹
        std::vector<Member> members;
    };
    std::vector<PendingSystem> pending;
    std::unordered_map<uint32_t, size_t> pendingBySystem;

    for (const auto& i : snap.instances) {
        if (i.structureIndex >= structureIndex.size()) {
            continue;
        }
        const uint32_t sid = (i.systemIndex >= 0 &&
                              static_cast<size_t>(i.systemIndex) < systemIds.size())
                                 ? systemIds[static_cast<size_t>(i.systemIndex)]
                                 : UINT32_MAX;
        const size_t instAtoms = snap.structures[i.structureIndex].atoms.size();

        const int32_t ei = i.trajectory.embeddedIndex;
        const bool hasEmbedded =
            ei >= 0 && static_cast<size_t>(ei) < embeddedTrajs.size() &&
            embeddedTrajs[static_cast<size_t>(ei)] != nullptr;
        std::shared_ptr<entity::Trajectory> traj;
        bool waitForSystem = false;
        if (hasEmbedded) {
            const auto& src = embeddedTrajs[static_cast<size_t>(ei)];
            const bool shared = embedRefCount[static_cast<size_t>(ei)] > 1;
            if (shared && sid != UINT32_MAX) {
                // 暂用整体系轨迹挂上（此刻偏移都是 0），待全部实例加完后由
                // setSystemTrajectory 统一修正 —— 临时坐标段随后被 rebuild 覆盖。
                traj = src;
                waitForSystem = true;
            } else if (i.trajectory.trajAtomOffset == 0 &&
                       src->frames[0].positions.size() == instAtoms) {
                traj = src;   // 单实例私有轨迹：直接共享，无需复制
            } else {
                // 单实例但需要切片（或未归组的共享实例）：复制本实例的原子段
                traj = sliceTrajectory(src, i.trajectory.trajAtomOffset,
                                       static_cast<uint32_t>(instAtoms));
            }
        }
        if (!traj) {
            // 链接式（或内嵌数据不可用）：单帧静态轨迹，坐标段由场景自身维护
            traj = std::make_shared<entity::Trajectory>();
            traj->dt = i.trajectory.dt;
            traj->periodic = i.trajectory.periodic;
            traj->frames.resize(1);
        }
        const uint32_t idx =
            sc.addTrajectoryInstance(structureIndex[i.structureIndex],
                                     toTransform(i.transform), traj, sid, i.name,
                                     i.isSolvent);
        if (waitForSystem && idx != UINT32_MAX) {
            auto it = pendingBySystem.find(sid);
            if (it == pendingBySystem.end()) {
                PendingSystem ps;
                ps.systemId = sid;
                ps.traj = embeddedTrajs[static_cast<size_t>(ei)];
                it = pendingBySystem.emplace(sid, pending.size()).first;
                pending.push_back(std::move(ps));
            }
            PendingSystem::Member m;
            m.instanceIndex = idx;
            m.atomOffset = i.trajectory.trajAtomOffset;
            m.traj = embeddedTrajs[static_cast<size_t>(ei)];
            pending[it->second].members.push_back(std::move(m));
        }
    }

    for (auto& ps : pending) {
        // 只有"该体系的全部成员都共享这一条内嵌轨迹"才能用系统级挂载；
        // 否则 setSystemTrajectory 会把体系里别的轨迹也覆盖掉。
        const uint32_t sysIdx = sc.systemIndexById(ps.systemId);
        const size_t sysMembers =
            (sysIdx == UINT32_MAX) ? 0 : sc.systems()[sysIdx].instanceIndices.size();
        const auto& src = ps.traj;
        // 保存时的偏移按几何顺序累加；重建出的偏移必须与之逐成员一致，
        // 否则说明实例顺序/原子数在保存后被改过，直接用系统级挂载会错位。
        bool offsetsMatch = true;
        {
            uint32_t off = 0;
            for (const auto& m : ps.members) {
                if (m.atomOffset != off) {
                    offsetsMatch = false;
                    break;
                }
                const auto& inst = sc.instances()[m.instanceIndex];
                off += static_cast<uint32_t>(
                    sc.structures()[inst.structureIndex].elements.size());
            }
            if (off != src->frames[0].positions.size()) {
                offsetsMatch = false;
            }
        }
        const bool systemWideOk =
            sysMembers == ps.members.size() && offsetsMatch &&
            sc.setSystemTrajectory(ps.systemId, ps.traj);
        if (!systemWideOk) {
            spdlog::warn("[serialize] 体系级轨迹无法统一挂载（成员数 {} / 记录 {}，"
                         "偏移一致={}），退化为逐实例切片",
                         sysMembers, ps.members.size(), offsetsMatch);
            for (const auto& m : ps.members) {
                const auto& inst = sc.instances()[m.instanceIndex];
                const uint32_t n = static_cast<uint32_t>(
                    sc.structures()[inst.structureIndex].elements.size());
                sc.setInstanceTrajectory(
                    m.instanceIndex, sliceTrajectory(m.traj, m.atomOffset, n));
            }
        }
    }

    if (!snap.instances.empty() && !snap.instances[0].trajectory.sourcePath.empty()) {
        spdlog::info("[serialize] 轨迹源引用: {}（帧数据未内嵌，重放需按需加载）",
                     snap.instances[0].trajectory.sourcePath);
    } else if (snap.instances.size() > 0 && !snap.embeddedTrajectories.empty()) {
        spdlog::info("[serialize] 内嵌轨迹已恢复: {} 条 / {} 实例",
                     snap.embeddedTrajectories.size(), snap.instances.size());
    }

    // 5) 场景属性
    auto& p = sc.properties();
    for (int k = 0; k < 3; ++k) {
        p.bgColor[k] = snap.bgColor[static_cast<size_t>(k)];
        p.skyColor[k] = snap.skyColor[static_cast<size_t>(k)];
        p.groundColor[k] = snap.groundColor[static_cast<size_t>(k)];
        p.ambientColor[k] = snap.ambientColor[static_cast<size_t>(k)];
        p.dirLightDir[k] = snap.dirLightDir[static_cast<size_t>(k)];
        p.dirLightColor[k] = snap.dirLightColor[static_cast<size_t>(k)];
    }
    p.dirLightIntensity = snap.dirLightIntensity;
    p.dirLightEnabled = snap.dirLightEnabled;
    p.shadowsEnabled = snap.shadowsEnabled;
    p.shadowStrength = snap.shadowStrength;
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
