# MolecuLens v2 · 参考实现研究笔记

> 用途：为 Aurora（v2）重构提供"别人怎么分层、我们该抄什么、不该抄什么"的一手依据。
> 作者：参考实现研究员（ref-researcher）；只读源码，未修改 `src/`，未运行构建。
> 与 `docs/_internal/blueprint.md`（设计基线）配套阅读：本文件给**证据**，蓝图给**决策**。

## 0. 阅读范围、方法与证据等级

### 0.1 证据等级约定

全文用三种标记，避免把印象写成事实：

| 标记 | 含义 |
|---|---|
| 【源码】 | 直接读到代码，给出 `文件:行`，可核对 |
| 【推断】 | 由多个代码事实归纳（如"三套状态需手工同步"），未逐行通读全部路径 |
| 【未验证】 | 来自公开资料/记忆，本会话未取得原始材料（尤其 Multiwfn 截图），需回查 |

### 0.2 三个参考源的实际路径与规模

| 参考源 | 路径 | 规模 | 语言/框架 |
|---|---|---|---|
| VMD 1.9.4 | `H:\04_代码_项目\c_study\vmd-1.9.4a57.src\vmd-1.9.4a57\src` | 709 个文件（扁平目录） | C++（C++98 + 宏），OpenGL/GLUT/FLTK |
| GXNU-MolStudio v1.0 | `F:\MolecuLens\other\gxnu_ref\src` | 44 个文件，约 2.4 万行 | Python 3 + PyQt5 + PyOpenGL + PyMCubes |
| Multiwfn | 用户提供的界面截图（外部）；公开手册 | — | 控制台式编号菜单程序 |

下文引用 VMD 时省略前缀，只写 `src/` 下的文件名与行号；引用 GXNU 时只写 `gxnu_ref/src/` 下的文件名。

---

# A. VMD 1.9.4：显示抽象、场景与表面算法

## A.1 DisplayDevice：一个类装了"交互 + 离线 + 立体 + 拾取"

`DisplayDevice` 是整个渲染层的唯一抽象。它是**纯接口 + 状态仓库**的混合体：既定义全部虚函数（`DisplayDevice.h:86–805`），又在基类里保存投影方式、裁剪面、雾、景深、立体、缓存模式等公共状态。

### A.1.1 虚函数清单（按关注点分组，均已核对 `DisplayDevice.h`）

| 关注点 | 虚函数（行号） |
|---|---|
| 生命周期/进程 | `init(int argc, char**argv, VMDApp*, int*size, int*loc)` :257；`get_num_processes()` :86；`is_renderer_process()` :87；`supports_gui()` :269；`~DisplayDevice()` :250 |
| 事件 | `queue_events()` :281；`read_event(long&, long&)` :289；`x()`/`y()` :292–293；`shift_state()` :303；`spaceball(...)` :309；`set_cursor(int)` :315 |
| 窗口 | `do_resize_window(int,int)` :227；`do_reposition_window(int,int)` :228；`reshape()` :796；`update(int=TRUE)` :795 |
| 绘制阶段 | `prepare3D(int=TRUE)` :788；`prepareOpaque()` :789；`prepareTrans()` :790；`clear()` :791；`left()` :792；`right()` :793；`normal()` :794；`render(const VMDDisplayList*)`；`render_done()` |
| 状态开关 | `set_stereo_mode(int=0)` :571；`forced_stereo_draws()` :586；`set_cache_mode(int=0)` :604；`set_render_mode(int=0)` :622；`aa_on()/aa_off()` :422–423；`culling_on()/culling_off()` :431–432；`cueing_on()/cueing_off()` :355–363 |
| 矩阵 | `loadmatrix(const Matrix4&)` :772；`multmatrix(const Matrix4&)` :773 |
| 颜色/光照 | `do_use_colors()` :105；`do_define_light(int,float*,float*)` :195；`do_activate_light(int,int)` :196；`set_background(const float*)` :781；`set_backgradient(...)` :782 |
| 坐标换算 | `abs_screen_loc_3D(...)` :746；`abs_screen_loc_2D(...)` :749；`rel_screen_pos(float&,float&)` :752；`find_3D_from_2D(...)` :766 |
| 拾取 | `pick(int dim, const float* pos, const VMDDisplayList* cmdlist, ...)`；`do_define_clipgroup()`；`start_clipgroup()/end_clipgroup()` |
| 几何原语（20+） | `sphere / sphere_array / cylinder / cone / line / point / triangle / trimesh_* / tristrip / cube / square / text` 等（`DisplayDevice.h` 后半段） |
| 读回 | `readpixels_rgb3u(int&,int&)` :799；`readpixels_rgba4u(...)` :802；`drawpixels_rgba4u(...)` :805 |

配套枚举：`DisplayEye{NOSTEREO, LEFTEYE, RIGHTEYE}` :46；`EventCodes{WIN_REDRAW, WIN_LEFT, ...}` :55；`Projection{PERSPECTIVE, ORTHOGRAPHIC}` :126；`CueMode{CUE_LINEAR, CUE_EXP, CUE_EXP2}` :108；`ShadowMode{SHADOWS_OFF, SHADOWS_ON}` :114。

### A.1.2 为什么这套抽象在 1994–2005 年是对的

- **一个 draw 遍历、N 个后端**：场景图只调 `display->sphere(...)` 一类原语，后端负责"画"还是"写文件"。加一个后端 = 加一个子类，场景代码零改动。
- **原语层粒度足够粗**：`trimesh_c4u_n3f_v3f`（颜色 4×u8 + 法线 3×f32 + 顶点 3×f32）这种"带布局后缀"的命令，把顶点格式也纳入协议，后端可以直接 `glInterleavedArrays` 或直接 `fprintf`。
- **立体/双眼/多进程渲染进程**通过 `left()/right()/is_renderer_process()` 内建，而不是散在调用方。

### A.1.3 代价（v2 不该复制的部分）

1. **接口太宽**：60+ 虚函数，其中一半（`spaceball`、`cueing_*`、`do_reposition_window`）跟"渲染"无关，只服务于当时的 SGI/CAVE 硬件。新增后端要写 60 个空实现。
2. **基类持有大量显示状态**（`transMat`、`nearClip/farClip`、`dofFNumber`、`cueMode`、`aoAmbient/aoDirect`），导致 `Scene::draw()` 里出现"Scene 有 intimate knowledge of display 的立体实现"这种注释（`Scene.C:637` 附近），职责已经互相渗透。
3. **`prepare3D/prepareOpaque/prepareTrans` 是隐式协议**：返回 int 表示"还要不要再遍历一次"，语义靠文档和约定，不靠类型。

> **对 v2 的结论**：接口按"能力"拆小（v2 已是 `VulkanRenderer` 单一实现，无多后端诉求，不需要 VMD 式宽接口）；但**两阶段 pass 顺序必须显式化**——VMD 的 `prepareOpaque → root.draw → prepareTrans → root.draw` 是正确思路，只是靠返回值传递太隐晦。v2 应改成 `IRenderPass{prepare(), draw(cmd), finish()}` 的有序列表。

## A.2 多后端如何分层：两条正交的继承轴

VMD 的 22 个后端不是平铺的，而是分成**两个家族**，共用 `DisplayDevice` 一层：

```
DisplayDevice                       ← 抽象：原语 + 阶段 + 事件（DisplayDevice.h）
├── OpenGLRenderer                  ← 交互式家族：真·实时（OpenGLRenderer.h）
│   ├── OpenGLDisplayDevice         ← 原生 GLUT 窗口
│   ├── FltkOpenGLDisplayDevice     ← FLTK 窗口（vmd 默认 GUI）
│   ├── OpenGLPbufferDisplayDevice  ← 离屏 pbuffer
│   ├── CaveDisplayDevice           ← CAVE 多投影
│   ├── FreeVRDisplayDevice         ← FreeVR
│   └── VRJugglerDisplayDevice      ← VR Juggler
└── FileRenderer                    ← 离线家族：写场景描述文件 + 调外部渲染器（FileRenderer.h）
    ├── TachyonDisplayDevice / LibTachyonDisplayDevice   ← 内置光线追踪（默认高质量）
    ├── OSPRayDisplayDevice / OSPRay2DisplayDevice       ← Intel OSPRay（CPU/GPU 光追）
    ├── OptiXDisplayDevice                               ← NVIDIA OptiX
    ├── POV3DisplayDevice                                ← POV-Ray
    ├── R3dDisplayDevice                                 ← RenderMan/Raster3D
    ├── RenderManDisplayDevice / GelatoDisplayDevice / LibGelatoDisplayDevice / ArtDisplayDevice
    ├── RayShadeDisplayDevice / RadianceDisplayDevice
    ├── VrmlDisplayDevice / Vrml2DisplayDevice / X3DDisplayDevice / MayaDisplayDevice
    ├── WavefrontDisplayDevice（.obj）/ STLDisplayDevice / PSDisplayDevice（PostScript）
    ├── SnapshotDisplayDevice（直接截图）
    └── ANARIDisplayDevice（旧版）
```

（`FileRenderer` 的 22 个子类由 `grep "class .*: public FileRenderer"` 全部列出；`OpenGLRenderer` 的 6 个子类同理。【源码】）

### A.2.1 FileRenderer 的模板方法

`FileRenderer` 把"离线渲染"抽象成固定五步，子类只填中间三步：

```
open_file() → write_header() → （场景图遍历，每个原语写一段文本） → write_trailer() → close_file()
```

关键字段（`FileRenderer.h`）：`publicName`、`publicPrettyName`、`defaultFilename`、`defaultCommandLine`、`execCmd`、`has_aa`、`aasamples`、`aosamples`、`has_imgsize`、`imgwidth/imgheight`、`aspectratio`、`curformat`。

**设计要点**：`defaultCommandLine` 是带占位符的外部命令模板（如 `tachyon %s -format TGA ...`），`update_exec_cmd()` 会把文件名/分辨率代进去，`render()` 末尾 `system(execCmd)`。于是"新增一个外部光追器"= 写一个文本后端 + 一行命令模板，**不用碰场景代码**。这是 VMD 能支持 20+ 导出格式的真正原因。

### A.2.2 值得抄 / 不该抄

| 项 | 判断 | 理由 |
|---|---|---|
| 交互 vs 离线双家族 | **抄结构** | v2 已有导出（离屏 FBO + PNG）与交互，但二者共用 `VulkanRenderer`；建议显式区分 `IInteractiveBackend` / `IOfflineBackend`，把"分辨率/SSAA/格式/超时"归离线侧，避免交互路径背参数 |
| 命令模板 + `system()` 调外部程序 | **不抄** | v2 不允许注入外部可执行（安全 + 可测试性），且已有 CPU 光追路径（蓝图 §5.5） |
| 22 个手写文本后端 | **不抄** | 维护成本极高（VMD 每个后端都要重写一遍 20+ 原语）；v2 若要多格式，应走 PBRT/glTF 导出 + 中间表示，而不是每格式一个类 |
| `execCmd` 里拼命令行 | **不抄** | 注入面太大 |

## A.3 Scene / Renderer / DrawMolecule 的职责边界

VMD 没有单独的 `Renderer` 类，渲染驱动在 `Scene` 里（这也是老设计）。

| 类 | 职责 | 不该做的事（它做了） |
|---|---|---|
| `Scene`（`Scene.h`，345 行） | 场景图根（`Displayable` 子类）+ 全局状态（背景/渐变/光照/颜色表）+ 两阶段驱动 `draw()` + 一次性离线驱动 `filedraw()` + `prepare()` 变更检测 | 直接持有 4 盏灯的 `LightState`、背景色 `ColorId`，把"全局外观"写进场景图节点 |
| `Displayable`（`Displayable.h`） | 可绘制节点基类：`prepare()`、`draw(DisplayDevice*)`、`draw_prepare()`、颜色变更钩子、`pick_cmd_list` | `do_color_changed/do_color_scale_changed` 把"颜色定义"耦合进每个节点 |
| `DrawMolecule`（`DrawMolecule.h`） | **同时是数据容器与场景节点**：`BaseMolecule` 数据 + `Displayable` 行为；持有 `ResizeArray<Timestep*> timesteps` 与 `ResizeArray<DrawMolItem*> repList`；rep 增删改、帧增删、PBC 变更、比例因子 | 一个分子对象直接挂在场景图上（多体系/分组语义无处安放） |
| `DrawMolItem`（`DrawMolItem.h`） | **一个表示**：`AtomRep*`（几何生成策略）+ `AtomSel*`（选择）+ `MSMSInterface`/`NanoShaperInterface`/`Surf`；实现 `prepare()`、`draw()`、`pick_start/move/end` | 表面生成器（MSMS/NanoShaper）写在表示内部，导致"表面"不是独立资源而是表示的副作用 |
| `AtomRep` | 表示类型多态（CPK/Licorice/QuickSurf/…），产出命令 | — |

### A.3.1 `Scene::draw()` 的真实流程（`Scene.C:571` 起）

```
void Scene::draw(DisplayDevice *display) {
  if (!display) return;
  if (!display->is_renderer_process()) return;   // 主进程不画（多进程渲染）
  if (backgroundmode_changed)  display->set_background_mode(backgroundmode);
  if (light_changed)           for (i<DISP_LIGHTS) { do_define_light(i,...); do_activate_light(i,...); }
  display->use_colors(colorData);
  if (background_color_changed)      display->set_background(color_value(...));
  if (backgrad*_color_changed)       display->set_backgradient(top, bottom);
  display->prepare3D(TRUE);                       // 清屏 + 设置视口
  ... 立体分支：forced_stereo_draws() 时 left()/prepareOpaque/draw/prepareTrans/draw 再来一遍 right()
  else { if (stereo) left(); else normal();
         display->prepareOpaque();
         root.draw(display);
         if (display->prepareTrans()) { root.draw(display); }  // 第二次遍历
         display->update(); }
}
```

要点与教训：

1. **两阶段是"两次遍历命令流"**，不是"两套命令流"。透明物在第一遍被跳过（后端 `prepareTrans()` 返回 0 就完全不画第二遍），第二遍才画。
2. VMD 自己承认这里有问题（`Scene.C` 注释，原文）：
   > `//     allow for property-based sorting of geometry, occlusion culling`
   > `//     algorithms, etc.  This needs to be changed.`
   也就是**透明物没有按深度排序**，只有"不透明后、透明后"的粗粒度，且没有 occlusion culling。这是 v2 必须超越的点。
3. `Scene::prepare()`（`Scene.C:532`）返回"是否需要重建命令流"的布尔：`root.draw_prepare() || backgroundmode_changed || light_changed || 颜色变更`。这是 v2 `SceneCommandList` 按 serial 复用的直系祖先。
4. `draw()` 被注释要求"当成 const 方法写"，因为多进程共享内存并发调用——**没有锁的正确并发靠约定**，v2 别学。

### A.3.2 对 v2 的映射

| VMD | v2 现状（已核对 `src/`） | 建议 |
|---|---|---|
| `Scene` + `Scene::draw` 两遍 | `engine/render` 的 Renderer + 两阶段（蓝图 §5） | 保持，并把 pass 顺序写成显式列表（shadow → opaque → transparent(排序) → overlay） |
| `Displayable` 节点树 | `Representation` 列表（`engine/rep/representation.h:40`） | **不建通用场景图**：v2 是"实例 + 表示"两层，扁平列表足够；通用图只会带来生命周期与遍历成本 |
| `DrawMolecule`（数据即节点） | `entity::SceneStructure` + `Instance` | v2 的数据/呈现已分离，**这是比 VMD 更正确的地方，不要回退** |
| `DrawMolItem`（表示即节点） | `Representation` + `bindRepresentations` | 一致，且 v2 的"三元组（方法/配色/作用域）正交"比 VMD 的 `AtomRep+AtomColor+AtomSel` 更清晰 |
| `Scene::prepare()` 返回脏标志 | `Renderer::prepare` + `DIRTY_*` 位 | v2 用位标志比布尔更细，保留 |
| 表面生成器内嵌在表示里 | `engine/async/surface_jobs` 独立线程 | v2 已把表面变成异步资源，**优于 VMD**，继续 |

## A.4 命令队列与显示列表

### A.4.1 VMDDisplayList：字节池 + 定长头

```cpp
struct CommandHeader {   // VMDDisplayList.h:68
  int  code;             // 显示命令码
  long size;             // 该命令总字节数（含头）
};
void *append(int code, long size);   // VMDDisplayList.h:100，返回可写区
```

- 池化：`pool/poolsize/poolused`，一次大块分配，`append` 只移动游标；`listsize` 是逻辑长度。
- 遍历：`VMDLinkIter first()/next()`，靠 `header->size` 步进（`VMDDisplayList.h:136–139`）。
- 元数据：`serial`（全局唯一，内容摘要）、`cacheskip`、PBC/实例标志、`transX/Y/Z`、`material`（`ambient/specular/diffuse/shininess/mirror/opacity/outline/outlinewidth/transmode`）、`materialtag`、`clipplanes[6]`。

**优点**：零虚调用、缓存友好、可整块失效/复用、可序列化。
**缺点**：类型不安全（每个 `draw_*` 自己 cast 回来）、`size` 手算易错、材质是"状态"而非"属性"（顺序敏感）、`long` 在 Win64 是 4 字节而指针是 8 字节（VMD 的跨平台坑）。

### A.4.2 CommandQueue：所有状态修改都成为命令

`CommandQueue.h:51–64`：`ResizeArray<Command*> cmdlist` + `ResizeArray<UIObject*> uilist`；
`append(Command*)` 入队 / `runcommand(Command*)` 立即执行 / `execute_all()` 排空 / `check_events()` 让 UI 轮询 / `register_UI` / `unregister_UI`。
头部注释（`CommandQueue.h:23`）："There is one global instance of this class in VMDApp, called commandQueue."

**这是 VMD 最值得抄的一条架构原则**：Tk 控制台、GUI 按钮、脚本 `mol addrep` 三个入口**都只产生命令对象**，执行路径唯一 → 可宏录制、可撤销、可远程。v2 的"控制器 + 事件总线（`RepresentationChanged`/`SceneChanged`）"是同一思想，但 v2 是**通知式**（改完再广播），VMD 是**命令式**（改的动作本身是对象）。命令式的额外好处：可以排队到帧边界执行，避免 UI 线程中途改状态导致渲染读到半更新数据。

### A.4.3 OpenGL 侧的显示列表缓存

`OpenGLRenderer` 把整条 `VMDDisplayList` 编译成 GL display list：`glNewList(id, GL_COMPILE_AND_EXECUTE)`（`OpenGLRenderer.C` 约 :3045/:3049），draw 时 `glCallList(ogl_cachedid)`（约 :4501）；缓存 id 从 `ogl_cachelistbase = 2000` 起（约 :832）；球体、字体另有预编译表（约 :1023/:1033、:1049–1063）。缓存模式 `OPENGL_CACHE_OFF/ON`，共 2 种。

**对 v2**：GL display list 在 Vulkan 无对应物，但**"命令流 → 后端缓存，靠 serial 失效"**这个契约可以直接搬：v2 的几何缓存（顶点/索引缓冲）与命令缓冲重建都应以 serial/指纹为键，而不是每帧重建。

## A.5 QuickSurf 算法要点

入口签名（`QuickSurf.C:1166`）：

```cpp
int QuickSurf::calc_surf(AtomSel *atomSel, DrawMolecule *mol,
                         float *atompos, float *atomradii,
                         int quality, float radscale, float gridspacing,
                         float isovalue, int colidx, const float *cmap,
                         VMDDisplayList *cmdList);
```

流程：

1. **包围盒 + 网格划分**：`numvoxels[i] = ceil((max[i]-min[i]) / gridspacing)`；`invgridspacing = 1/gridspacing`（:210）。
2. **高斯密度累积**：对每个原子，在 `radlim = gausslim * scaledrad * invgridspacing` 的格点半径内累加 `exp(-k·d²)` 权重（`vmd_gaussdensity`，:232–247）。`scaledrad = atomradii[i] * radscale`。
3. **密度预乘 1/isovalue**（:214 `float invisovalue = 1.0f/isovalue;`）：累积时就把密度除以 iso，于是后续等值面取 1.0 即可。好处是省一次全网格除法；坏处是"密度"不再是物理量，**不可复用于其它 iso**。
4. **quality → 高斯窗口**（`QuickSurf.C:1515–1525`）：

   | quality | gausslim | 注释 |
   |---|---|---|
   | 3 | 4.0 | max quality |
   | 2 | 3.0 | high quality |
   | 1 | 2.5 | medium quality |
   | 其它/0 | 2.0 | low quality |

   同时 quality 决定体纹理格式：3/2/1 → `RGB3F`，低 → `RGB4U`（:1539–1547）。
5. **等值面提取**：`IsoSurface::compute(VolumetricData*, isovalue, step)` → `Polygonise`（marching cubes）→ `vertexfusion`（顶点合并）→ `normalize`（法线归一）；`GRIDCELL{p[8], g[8], val[8], cubeindex}`、`VertexInterp` 是标准实现。
6. **出图**：`draw_trimesh`（或带纹理的 `draw_trimesh_c4u_n3f_v3f`）把三角面直接塞进 `VMDDisplayList`；GPU 路径走 `CUDAQuickSurf`（无 CUDA 时回退 CPU），并有 `pretime/voltime/gradtime/mctime/mcverttime/reptime` 分段计时。

**借鉴**：
- `radscale / gridspacing / isovalue` 三参数语义清晰且已在 v2 对齐（`engine/rep/representation.h:25` 注释：`radscale=radiusScale, gridspacing, isovalue`）。建议**把 `quality → gausslim` 的映射也搬进 v2**，作为"质量"下拉的语义层：用户选"高/中/低"，内部换算 gausslim 并据此推导 gridspacing 下限，而不是让用户直接面对格点间距（这是 Multiwfn/GXNU 都做不好的地方——它们把底层参数直接摊给用户）。
- **分段计时**（`pretime/voltime/gradtime/mctime/mcverttime/reptime`）应照抄进 v2 的渲染统计（蓝图 §5.6 已要求 `drawCalls/triangles/uploadBytes`，再加这四个阶段耗时即可定位"表面卡在 MC 还是上传"）。

**不借鉴**：
- CUDA/CPU 双实现：v2 单后端，异步线程 + 未来 compute 已够（蓝图 §13 路线图）。
- 密度预乘 `1/isovalue`：v2 应保留物理密度，把 iso 作为采样阈值传下去，否则"改 iso 不必重建密度网格"这一优化无法实现（VMD 每改 iso 都要重跑整个 calc_surf，是它拖动 iso 卡顿的根因【推断：由 invisovalue 在累积阶段使用推出】）。

## A.6 数据模型：Timestep / QMTimestep / QMData

### A.6.1 Timestep（`Timestep.h:45–73`）

| 字段 | 说明 |
|---|---|
| `int num` | 原子数 |
| `int page_align_sz` | 页对齐要求（供 unbuffered kernel I/O 用） |
| `float *pos, *pos_ptr` | 坐标（Å）；`pos_ptr` 是非对齐指针，`pos` 是对齐后的 |
| `float *vel, *force` | 速度、力（kcal/mol/Å） |
| `float *user, *user2..user4` | 按需分配的每原子 1 float 自定义数据 |
| `QMTimestep *qm_timestep` | 量化数据（轨道、波函数） |
| `float energy[TSENERGIES]` | 各能量项（动能/势能/温度/压强/电场/Urey-Bradley/约束） |
| `int timesteps` | 已流逝步数 |
| 晶胞 | `a_length/b_length/c_length`、`alpha/beta/gamma` + `get_transforms(Matrix4&,Matrix4&,Matrix4&)`、`get_transform_from_cell(const int*, Matrix4&)`、`get_transform_vectors` |

构造：`Timestep(int n, int pagealignsz=1)`；帧复制走 `DrawMolecule::duplicate_frame(const Timestep*)`。

**评价**：`float*` 裸指针 + 手写对齐 + 深拷贝语义，是 C++98 时代的合理选择，但**每帧一次全量 malloc/free**（大体系 100 万原子 × 3 float = 12 MB/帧）在 v2 的轨迹播放里是明确的性能陷阱。v2 用 `CoordsSoA`（x/y/z 分块 + `DIRTY_COORDS` 只改坐标段，蓝图 §4）正是对此的修正。

### A.6.2 QMData / QMTimestep / Wavefunction（`QMData.h`）

`QMData` 是"一个波函数文件"的完整解析结果：

```
num_wave_f  波函数个数      num_basis 基函数数     num_types 原子类型数
num_atoms   原子数          atom_types[]           wave_offset[]
angular_momentum[]  norm_factors[]  basis_array[]  atom_basis[]
shell_types[]       basis_set[]    wavef_signa[]  nimag  nintcoords
carthessian[]       wavenumbers[]  normalmodes[]
```

配套结构：

```cpp
struct prim_t  { float expon; float coeff; };                    // 一个原始高斯
struct shell_t { int numprims; int type; int combo;              // type: S/P/D...; combo: 笛卡尔/球谐
                 int num_cart_func; float norm_fac;
                 const prim_t *basis; };                          // 指向 prim_t 数组
struct Wavefunction { int idtag, type, spin, excitation, multiplicity;
                      int num_orbitals, num_coeffs;
                      float energy; float *wave_coeffs;           // MO 系数矩阵
                      float *orb_energies, *occupancies; int *orb_ids; };
```

枚举：`QMSTATUS_*`、`SCFTYPE_*`、`RUNTYPE_*`、`QMCHARGE_{MULLIKEN,LOWDIN,ESP,NPA}`、`WAVE_{CANON,LOCALIZED,NATURAL,...}`。

**值得抄的两点**：
1. **波函数是 Timestep 的一等公民**，不是"一个 cube 文件"。v2 目前把 cube 当输入（`EspSurface` 表示吃 cube 路径），缺"体系级波函数对象 → 派生 density/ESP/MO cube"这一层。做轨道模块时若沿蓝图 §3.3 的"复用 ESP 映射表面、同一 cube 作密度与势"会**继续把波函数概念丢掉**，将来接 fchk/log 直读时会重写。
2. **`orb_ids` + `orb_id2index`/`orb_sort_map`**：解决"用户说 HOMO-2，内部是第几个 MO"（考虑开壳层 α/β、跳跃轨道、冻结芯）。GXNU 用字符串 `"ha-1"` 硬编码在命令里（见 B.3），VMD 用显式映射表——后者可维护性高一个量级。

### A.6.3 一个反例：`DrawMolecule` 里的 `VMDENABLEORBITALGRIDBACKDOOR`

`DrawMolecule.h` 有一个编译开关 `VMDENABLEORBITALGRIDBACKDOOR`，注释大意是"多个表示共用同一分子时复用轨道网格，最高 2× 加速"。它用 `#if` 把"轨道网格缓存"塞进分子对象，并在注释里自称 hack。**教训**：性能优化以宏 + 后门方式塞进数据类，会让数据类承担缓存职责、让构建配置改变行为（同一份代码两种语义）。v2 的几何缓存必须放在 `engine/render` 的独立缓存对象里，以指纹为键，不用宏。

## A.7 VMD 总评：抄什么、不抄什么

| 设计 | 结论 | 原因（要点） |
|---|---|---|
| `DisplayDevice` 单抽象 + 多后端 | **部分抄** | 抄"场景只发原语、后端决定落地方式"；不抄 60+ 宽接口（v2 单后端） |
| 交互/离线双家族 | **抄结构** | 离线侧参数（分辨率/AA/格式）不该污染交互路径 |
| 两阶段 pass（opaque→trans） | **抄并加强** | 必须补深度排序；VMD 自己在 `Scene.C` 注释里承认没排序 |
| `Scene::prepare()` 脏检测 | **抄** | 与 v2 serial 复用同构，保留 |
| 命令队列（所有修改皆命令） | **抄** | 三入口统一、可排队到帧边界、可宏录制；v2 可从"通知式"升级为"命令式" |
| 字节池命令流 | **不抄** | 类型不安全、`long` 跨平台坑；v2 用带 tag 的 variant/描述符数组 + 静态断言 |
| `DrawMolecule` 数据即节点 | **不抄** | 多体系/分组会失控；v2 已分离数据与呈现，别回退 |
| 表面生成器内嵌表示 | **不抄** | 表面应是独立异步资源（v2 已做对） |
| 材质/颜色作为命令流状态 | **抄但要显式** | 省带宽；v2 若用同样做法，需在 API 上强制"材质块成对出现"（RAII 作用域） |
| QuickSurf 三参数 + quality 映射 | **抄** | 参数语义清晰，且 v2 已对齐；补 quality 层 |
| 密度预乘 1/iso | **不抄** | 破坏"改 iso 不重建"的优化 |
| 全局单例 CommandQueue | **不抄单例** | 依赖注入即可测；v2 的 `AppServices` 已是注入式 |
| 宏开关改变语义 | **不抄** | 构建配置不应改变同一份源码的行为 |

---

# B. GXNU-MolStudio v1.0：功能最全，结构最痛

GXNU 是"一个人把 VMD 的高质量出图 + Multiwfn 的分析能力 + Qt 的界面"粘起来的产品。**功能覆盖度是三个参考里最高的，架构是三个里最差的**——它给出的价值不是"怎么分层"，而是"哪些交互细节必须做"以及"不做分层的代价"。

## B.1 规模实测（行数 / 类方法数）

| 文件 | 行数 | 说明 |
|---|---|---|
| `fchk_orbital.py` | 3775 | Tcl 生成器 + Multiwfn 调用 + VMD 进程控制 + 样式表 + CLI（**单一文件五个职责**） |
| `orbital_viewer_v53.py` | 3297 | **上一代**单体主窗口（`OrbitalVisApp` 2449 行 / 96 方法） |
| `main_window.py` | 3205 | **当代**单体主窗口（`OrbitalVisApp` 3253 行 / 110 方法，:330–3583） |
| `esp_panel.py` | 2138 | ESP 面板 + 面积分布图 + 极值解析 |
| `orbital_gl_widget.py` | 1315 | OpenGL 渲染器（`OrbitalGLWidget` 982 行 / 35 方法） |
| `molcanvas.py` | 1262 | **QPainter 软件**分子画布（`MolCanvas` 949 行 / 39 方法） |
| `igmh_panel.py` | 886 | IGMH/IRI 面板 + 复合场 + 散点图 |
| `charge_viewer.py` | 794 | 电荷 + 键级 |
| `nbo_viewer.py` | 589 | NBO |
| `orbital_viewer_lib.py` | 543 | 公共 Tcl 片段 |
| `nbo_parser.py` | 532 | NBO 输出解析 |
| `theme.py` | 429 | 全局 QSS |
| `esp_viewer.py` | 422 | ESP 表面提取 + 色标 + 独立预览器 |
| `i18n.py` | 348 | 翻译字典（约 283 条 key） |
| `glsl_shaders.py` | 223 | 深度剥离/Phong/背景着色器 |
| `marching_cubes.py` | 346 | cube 读取 + MC + 法线 |
| `workers.py` | 155 | QThread 封装 |
| `ovcanvas/_glwidget.py` | ~218 KB | 另一套 OpenGL 画布（体积最大） |
| `ovcanvas/_panel.py` | ~102 KB | 另一套面板 |

（行数由 `Get-Content | Measure-Object` 实测；`_glwidget.py/_panel.py` 为字节数。）

**类规模**（`^class` 到下一个 `^class` 计数，方法数只数 4 空格缩进的 `def`）：

| 类 | 文件 | 跨度行 | 行数 | 方法数 |
|---|---|---|---|---|
| `OrbitalVisApp(QMainWindow)` | `main_window.py:330–3583` | 3253 | 110 |
| `OrbitalVisApp(QMainWindow)`（旧版） | `orbital_viewer_v53.py:1262–3711` | 2449 | 96 |
| `EspPanel(QWidget)` | `esp_panel.py:537–1683` | 1146 | 57 |
| `AreaChartDialog(QDialog)` | `esp_panel.py:1717–2301` | 584 | 14 |
| `IgmhPanel(QWidget)` | `igmh_panel.py:459–980` | 521 | 32 |
| `NboPanel(QWidget)` | `nbo_viewer.py:180–654` | 474 | 33 |
| `ChargePanel(QWidget)` | `charge_viewer.py:316–651` | 335 | 22 |
| `OrbitalGLWidget(QOpenGLWidget)` | `orbital_gl_widget.py:553–1535` | 982 | 35 |
| `MolCanvas(QWidget)` | `molcanvas.py:431–1380` | 949 | 39 |
| `GlMesh` | `orbital_gl_widget.py:156–434` | 278 | 4 |
| `ArcballCamera` | `orbital_gl_widget.py:435–552` | 117 | 12 |

## B.2 模块划分与实现方式

| 模块 | 文件 | 实现方式 | 依赖 |
|---|---|---|---|
| 主窗口 | `main_window.py` | 单类 `OrbitalVisApp`，用 `QSplitter(Horizontal)` 左右各 620px（:434–437），右列是 `QTabWidget`（7 个 tab：setup/style/charge/bond_order/nbo/esp/igmh，:529–613） | 全部模块 |
| 轨道 | `fchk_orbital.py` + `orbital_viewer_v53.py` + `orbital_gl_widget.py` | **两条路**：(a) 生成 Tcl 脚本，拉外部 VMD 进程渲染；(b) 内置 QOpenGLWidget 自己画 | PyMCubes/numpy/PyOpenGL |
| ESP | `esp_panel.py` + `esp_viewer.py` | Multiwfn 产 cube → 密度场做几何 + ESP 场顶点着色 → 内置 GL 或 VMD | 同上 |
| IGMH/IRI | `igmh_panel.py`（移植自 IGMH_Toolbox V4） | Multiwfn 产 `dg_inter/dg_intra/dg/sl2r` 四个 cube → 复合场着色；散点图用 matplotlib 嵌入 | matplotlib |
| NBO | `nbo_viewer.py` + `nbo_parser.py` | Multiwfn 跑 NBO，正则解析文本输出成表 | 纯文本 |
| 电荷/键级 | `charge_viewer.py` | Multiwfn 输出 → 表 + 色带；`MultiwfnRunner`(:209) 统一 `subprocess` | 纯文本 |
| 主题 | `theme.py` | 单一 `LIGHT_QSS` 字符串（429 行 QSS） | — |
| 线程 | `workers.py` | `CubeWorker(QThread)`、`RenderWorker(QThread)`，`pyqtSignal` 回主线程 | Qt |
| i18n | `i18n.py` | `TR` 字典 + `tr(key, **fmt)`；语言全局变量 `_CURRENT_LANG` | — |
| 画布 | `molcanvas.py` | **QPainter 软件光栅**：`paintEvent` 里逐球画径向渐变（`_make_sphere_gradient`）、逐键画锥/柱、`_depth_factor` 做近大远小伪深度 | 纯 Qt，无 GL |

**关键观察**：同一个产品里存在**三套渲染路径**——QPainter 软件画布（`MolCanvas`）、内置 OpenGL（`OrbitalGLWidget`）、外部 VMD 进程（Tcl over socket）。三者视觉不一致、状态各自维护，主窗口必须手工同步（`_sync_canvas_to_vmd`、`_refresh_vmd_scene`、`_push_cubes_to_canvas`、`_send_vmd_cmd` 等方法都在做"把 A 的状态搬到 B"）。**这是单体重构最该避免的形态**：v2 只有一个渲染器，就不存在这种同步成本。

## B.3 fchk → cube → 渲染的数据流（含 Multiwfn 命令序列）

### B.3.1 轨道 cube

`fchk_orbital.py:2620 gen_cube(fchk_path, orbital="h", grid_quality=2, multiwfn_exe=None, work_dir=None)`：

```python
# 命令行序列（喂给 stdin）：5 → 4 → 轨道标识 → 网格质量 → 2 → 0 → q
inputs = ("\n" + fchk_name + "\n5\n4\n" + orbital + "\n"
          + str(grid_quality) + "\n2\n0\nq\n")     # :2654–2657
```

- 工作目录里先拷一份 ASCII fchk 到 `_multiwfn_tmp/`，跑完把 `MOvalue.cub` 拷成 `<stem>_MO<orbital>.cub`，再删临时目录。
- `orbital` 支持 `h/l/h-1/number/ha/hb/la/lb`（开壳层 α/β）——**用字符串编码轨道语义**，与 VMD 的 `orb_ids` 映射表形成鲜明对比。
- `timeout=600`，`capture_output=True`，无进度回调（只能等）。

### B.3.2 ESP cube 与极值

`esp_panel.py:61–68` 的三条模板：

```python
# ISO：生成 density.cub 与 totesp.cub
CMD_ESPISO = "\n5\n1\n3\n2\n0\n5\n12\n1\n2\n"
# EXT：分子表面极值点分析 → surfanalysis.pdb
CMD_ESPEXT = "\n12\n3\n0.15\n0\n5\nmol.pdb\n6\n2\n"
# Area：ESP 分区面积分布
CMD_ESP_AREA_DIST = "\n12\n0\n9\nall\n"
ESPRHOISO = "0.001"
```

（编号语义按 Multiwfn 手册对应"输出格点数据 / 定量分子表面分析"等主功能；**未逐条核对手册**，此处只保证序列字面值与源码一致。）

### B.3.3 cube → 网格 → 着色

`marching_cubes.py`：

1. `read_cube(filepath)` (:42)：两行注释 → `natoms/origin` → `nx,dx / ny,dy / nz,dz` → 原子表 → 全部浮点 `reshape((nx,ny,nz))`；维度不符直接 `ValueError(f"Expected {total} points, got {len(raw_data)}")`。
2. `marching_cubes(cube_data, isovalue, flip_normal=False)` (:166)：PyMCubes 出顶点（**索引空间**，`[0, nx-1]`）与三角形；`world_verts = origin + mc_verts @ basis`（`basis = [dx,dy,nz]` 三行，:207–209）。
3. **法线用梯度而非面法线**：`_compute_gradients` 中心差分 → `_trilinear` 在顶点处插值 → `grad @ inv(basis)` 把索引空间梯度变到世界空间（**这一步很多实现会漏，非立方/斜晶格会歪**）→ 归一化，退化向量给 `(0,0,1)`。
4. **相位翻转**：正瓣（`data > +iso`）法线取 `-grad`；负瓣（`data < -iso`）翻转绕序 `tris[:, [0,2,1]]`。注释明确说这是照 IboView `FIsoSurface::FixVolumeDataNormals`（`IvIsoSurface.cpp`）做的——**这是判断"哪个面朝外"的正确做法，v2 若自己实现 MC 必须照抄这段语义**，否则轨道两侧会一面黑一面亮。
5. `extract_orbital_surfaces(cube_file, isovalue, pos_color, neg_color)` (:259)：一次读 cube、两次提取 ±，默认色 `+ (0.1,0.8,0.1)` / `- (0.9,0.25,0.25)`。
6. `relative_iso_threshold(cube_data, percent=80.0)` (:295)：**IboView 的"相对阈值"语义**——按 `|data|` 从大到小累积，取占总权重 `percent%` 处的值作为 iso。它不是绝对等值，所以"80%"在不同体系下视觉密度一致。v2 的轨道模块若只给绝对 iso 滑杆，用户会不断手调；建议加一个"相对阈值 %"输入。
7. `compute_bounding_sphere` (:329)：有原子就用原子坐标均值 + 1.5× 最大距离；否则用 8 个角点。

### B.3.4 到屏幕的两条出图路径

- **内置 GL**：`extract_esp_surface(...)` 逐顶点着色 → `GlMesh.upload(surface)` → 深度剥离 → Phong。
- **外部 VMD**：`_style_tcl(cube_name, isovalue, style_name, shade_mode, keep_h_indices)` 生成 Tcl（`mol new` / `mol addfile ... type cube` / `mol modstyle` / `mol modmaterial` / `mol modcolor` / `volume ... step` / `display ...`），通过 `_socket_server_tcl(port, label)` 的 Tcl socket 持续发命令；`render_current_view(port, render_dir, output_png, ...)` 调 Tachyon 出图。

## B.4 一键样式：四个预设的真实参数

样式定义在 `fchk_orbital.py:136` 起的 `STYLES` 字典（每条含 `desc / tachyon_options / lights / shadows / ao / aoambient / aodirect / surface_mat / surface_mat_b / pos_color / neg_color / atom_cpk / atom_mat / c_color / c_rgb / display_distance`）。另有 5 份 JSON 资源：`sob-art.json`、`HoukMol.json`、`IQMOL.json`、`gxnu_style.json`、`ring_style.json`。

| 预设 | 关键参数（实测值） | 视觉意图 |
|---|---|---|
| **sob-art** | `light_count=3`；`light_glow=0.51`，`light_glows=[0.88,0.88,0.88,0.51]`；`surface_mat=[0.1,0.6,1.0,1.0,0,0.75,0,0,1.0]`（ambient .1 / diffuse .6 / specular 1.0 / shininess 1.0 / mirror 0 / opacity .75）；`aoambient=0.8, aodirect=0.3`；`atom_cpk="0.6 0.4 30 30"` | 柔和三点光 + 主体 75% 不透明，轨道"果冻感" |
| **HoukMol** | `mol_style=HoukMol`；`gradient=gau_default`；`shininess="reasonably shiny"`；`light_count=1`；`light_dirs=[[-0.577,0.577,0.577],...]`（归一化方向光）；`light_glow=1.25`；**`atom_scale=1.68`、`bond_scale=2.0`**；`atom_outline=[true,[0,0,0],0.35]`；`orb_outline=[false,...,0.3]`；`orb_opacity=0.3`；`phase_pos=[0.667,1.0,0.498]`（黄绿）、`phase_neg=[0,0.667,1.0]`（天蓝）；`bg=[1,1,1,1]`；`crosshair=true` | 论文插图风：白底、单灯、粗键、橙黄/天蓝相位、加轮廓线 |
| **IQMOL** | `light_count=3`；`light_glow=1.0`；`light_glows=[0.1,0.28,0.53,1.0]`；`surface_mat=[0.15,0.55,1.0,1.0,0,0.72,...]`；`pos_color=[31,0.05,0.35,0.80]`、`neg_color=[32,0.90,0.25,0.25]` | 冷调、蓝/红相位、稍亮的高光 |
| **gxnu_style** | `atom_scale=1.68, bond_scale=2.0, orb_opacity=0.3`；`phase_pos=[0.48,0.78,0.92]`（青）、`phase_neg=[0.65,0.55,0.88]`（紫）；`fade=true` | 品牌色：青/紫 |
| **iboview-\***（6 变体） | `tachyon_options="-trans_vmd"`（半透）或 `-fullshade`；`surface_mat` 随变体变；`spec_mul` 说明：**1 = IboView 原样，0 = 哑光，>1 = 更亮** | IboView 的 6 套配色（crystal/dark/green-pink/purple-blue/cyan-yellow/rainbow） |
| **vmwfn0/1、morandi-*、ao-shiny、ao-chalky、white-green、white-red** | 其余变体，均同一套字段 | 多场景覆盖 |

**这张表对 v2 的直接价值**：底部快捷条的四个一键样式（蓝图 §3.2）应当**照抄这四组数值**，而不是重新调参——尤其 `atom_scale=1.68 / bond_scale=2.0`（HoukMol 风粗壮感）、`orb_opacity=0.3~0.75`、`phase_pos/neg` 的 RGB 三元组、`atom_outline=[on,[0,0,0],0.35]`。这些是反复试出来的"看起来对"的数字，重调一遍是浪费。

**注意**：`atom_cpk="0.600000 0.400000 30.000000 30.000000"` 四个数是 VMD `mol modstyle CPK <sphere> <bond> <res> <res>` 的参数语义（球半径系数/键半径系数/球细分/键细分），v2 自己的 CPK 参数不该照搬这个四元组，但 0.6/0.4 的比例可参考。

## B.5 ESP 表面与色标

**核心思想**：**几何来自密度场，颜色来自势场**——两者共用同一套网格索引。

`esp_viewer.py`：

| 常量 | 值 | 含义 |
|---|---|---|
| `AU_TO_KCAL` | `627.509` | Hartree → kcal/mol |
| `DEFAULT_ISOLEVEL` | `0.001` | 电子密度等值面 = vdW 表面 |
| `DEFAULT_VMIN/VMAX` | `-0.03 / +0.03` | ESP 色标范围（a.u.） |
| `ESP_CMAPS` | `{"BWR (蓝-白-红)": ("bwr", True), ...}` | 名字 → (matplotlib colormap, 是否发散型) |

`extract_esp_surface(density_cube, esp_cube, isolevel=0.001, vmin=-0.03, vmax=0.03, au=1.0, cmap="BWR (蓝-白-红)", auto_range=False)`（:154）：

1. 对 density cube 跑 marching cubes → 顶点 + 法线；
2. 把每个顶点**映射回 ESP 网格索引**，三线性采样 `ESP(vertex)`；
3. `t = (esp - vmin) / (vmax - vmin)` 夹到 [0,1]，用 colormap 取色（`_apply_colormap`，:140；matplotlib 不可用时回退内置 `_bwr`）；
4. 输出带逐顶点颜色的 `IsoSurface`。

`esp_panel.py` 的 UI 侧：

- 单位双显：`_display_to_au / _au_to_display / _on_unit_changed`（a.u. ↔ kcal/mol，用 627.509 换算），`_sync_color_scale` 同步色标条刻度。
- `_detect_esp_range` 自动范围（对应 `auto_range`）。
- 四种模式（`esp_panel.py:13–17` 注释）：**ISO**（等值面网格）、**PT**（顶点着色点云）、**EXT**（极值点标注：金=极大、浅蓝=极小）、**ALL**（ISO+EXT）。
- 表面参数：iso、vmin/vmax、cmap、opacity、`chk_transparent`、DPI、`chk_peel`（深度剥离）、材质（`_on_iso_mat`，`SHININESS_PRESETS`）、原子/键缩放、`sld_thinning`（键细化）、极值球/标签/描边/字号/半径/距离。
- `_vmd_cmap_name(cmap_name)`：把 matplotlib 色标名翻译成 VMD 的 `ColorScale` 名——说明"色标"在两个渲染器里是两套定义，需要映射表。

**对 v2 的启示**：
1. v2 的 `EspSurface` 表示（`representation.h:30`，注释"电子密度等值面 + 静电势逐顶点着色"）与这里**完全同构**，可直接沿用 `iso=0.001` 默认值与 ±0.03 色标范围。
2. **色标必须是"数据"而不是"代码"**：`(name, stops[], diverging, unit, scale)` 五元组，由 UI 选择、由渲染器采样。GXNU 用 matplotlib colormap + VMD 名字翻译，是两套系统硬桥接；v2 应定义自己的色标表（至少 BWR/BGR/viridis 三套），离屏导出与视口共用。
3. **自动范围**要能追溯：`auto_range` 打开时应在 UI 上显示"自动：[-0.028, +0.026] a.u."，否则用户不知道色标被改了（GXNU 只显示滑杆值，未验证是否显示自动探测结果）。

## B.6 深度剥离透明（Depth Peeling）

GXNU 把 IboView 的 GLSL 移植了过来，`glsl_shaders.py` 里四个关键着色器：

| 着色器 | 行 | 作用 |
|---|---|---|
| `FRAGMENT_ORBITAL_DP` | :114 | 读上一层的深度纹理 `DepthPrev`，**只保留比上一层更近的片元**：`if (gl_FragCoord.z < prevDepth \|\| prevDepth <= 0.0) out_Color = calc_base_color(...); else discard;` |
| `FRAGMENT_COMPOSITE` | :142 | 把本层颜色 `LayerColor` 合成到累积缓冲；`abs(layer.a) < 1e-2` 的片元丢弃（避免空层被合成成黑） |
| `FRAGMENT_DEPTH_COPY` | :179 | 把深度缓冲复制成纹理，供下一层用 |
| `FULLSCREEN_VERTEX` | :163 | 合成/拷贝用的全屏四边形 |

运行时结构（`orbital_gl_widget.py`）：

- `DEPTH_PEEL_LAYERS = 4`（:556，类属性默认）；
- `_ensure_fbos(w,h)`（:880）：为剥离层建 **2 个 ping-pong FBO**（各带 color+depth 纹理）+ 1 个累积 FBO（`_acc_fbo`），尺寸变化时全部重建；
- `paintGL()`（:851）：`_ensure_fbos` → `_render_scene(w,h)` → `glBlitFramebuffer` 把累积缓冲拷到默认 FBO；
- ESP 面板把它做成开关：`chk_peel`（默认勾选，文字"启用深度剥离（4 层）"，层数来自 `IBOVIEW_DEFAULTS["DepthPeelingLayers"]`，`esp_panel.py:627–628`），`_on_peel(on) → glw.set_depth_peeling(bool(on))`（:1148–1150）。
- `orbital_gl_viewer.py:197–204` 还允许用户选层数（"Depth Peeling:" 下拉），`DEPTH_PEEL_LAYERS = int(...)`（:456）。

**算法要点（v2 可直接用）**：
- 第 0 层用正常深度测试画最近面；之后每层"1 次深度预 pass（拷贝当前深度）+ 1 次颜色 pass"，用上一层的深度纹理做 `discard`，得到第 2 近面，逐层叠加。N 层 = O(N) 次几何提交，**代价随层数线性增长**。
- 4 层对两个半透明轨道（正/负瓣交叉）通常够用；层数不足时表现为"某层突然消失"。
- **v2 的更好选择**：Vulkan 有 `VK_KHR_fragment_shader_barycentric` / OIT 的 WBOIT（weighted-blended OIT，单 pass、无层数限制、近似正确）。建议 v2 保留"OIT 开关 + 层数"两档：WBOIT 做默认（快），depth peeling 做"精确模式"（可对照 GXNU 效果）。

## B.7 IGMH 复合场与散点图

`igmh_panel.py` 文件头注释（:1–17）把管线写得非常清楚：

```
1. 定义片段（片段1/片段2；左画布 Shift+左键框选原子归属）
2. 后台调 Multiwfn（IGMH: 20→11→2→片段→网格→2→3；IRI: 20→4→…）
   产出 dg_inter.cub / dg_intra.cub / dg.cub / sl2r.cub
3. 用 dg_inter 定几何等值面，sl2r（sign(λ₂)ρ）按 BGR 色标逐顶点着色
   （蓝=吸引、绿=弱、红=排斥）
4. IGM Map 散点图（sign(λ₂)ρ vs δg）
方法：Tian Lu, Qinxue Chen, J. Comput. Chem. 2022, 43, 539-553.
```

实现要点：

- `IGMHCompositeField`（:93）：`load(geo_path, map_path, iso)` → `_rebuild()` 用 geo cube 跑 MC 定几何、按 map cube 采样着色；`set_iso / set_crange / set_strength_alpha` 都是**只改色不改几何**或**只改 iso 重建**；`_recolor()` / `push()` 分离"重算颜色"与"上传 GPU"。
- `bgr_color(scalars, cmin, cmax, mid=0.5, strength_alpha=False, ...)`（:62）：BGR 三点插值；`strength_alpha=True` 时 **alpha 随 |sign(λ₂)ρ| 增强**（NCIPLOT 风格，弱相互作用区域更透明，突出强作用）。
- `sample_volume(cube, world_xyz)` / `_grid_index`（:40–60）：世界坐标 → 网格索引 → 三线性采样（与 ESP 同一套代码思路）。
- 散点图 `IgmScatterDialog`（:307）：matplotlib `FigureCanvasQTAgg` 嵌入，可选 δg 列（`combo_col`），`_draw`/`_save`；数据源是 Multiwfn 的 `output.txt` 解析。
- 片段选择：`_set_target(frag)` / `_on_box_select(indices)` / `_apply_fragment_colors`——**在画布上框选原子来定义片段**，这是很好的交互（比手输原子序号好得多），v2 若做 IGMH 应保留。

**对 v2**：IGMH 在蓝图里是"路线图"（analysis 模块标注"电荷/NBO/IGMH 标注为路线图"），正确。但**复合场（geometry from one cube, color from another）这个抽象应该现在就在 `engine/render` 里预留**：v2 的 `EspSurface` 已经是它的特例（密度定几何 + 势着色），把"几何 cube / 颜色 cube / 色标 / 色标区间 / alpha 规则"抽成通用的 `MappedSurface`，ESP、轨道（± 相位）、IGMH 都能复用。

## B.8 i18n 的实际做法

`i18n.py`：

```python
_CURRENT_LANG = "zh"
def tr(key, **fmt):
    s = TR.get(key, {}).get(_CURRENT_LANG, key)
    return s.format(**fmt) if fmt else s
```

- `TR` 是 `{key: {"zh": ..., "en": ...}}` 的嵌套字典，约 283 个 key。
- 缺 key 时**回退显示 key 本身**（不是回退另一语言）——好处是漏翻一眼可见（界面上出现 `iso_peel` 这种 snake_case 就是漏了），坏处是发布版会出现裸 key。
- 每个面板自带 `_t()` 包装（`esp_panel.py:585`、`igmh_panel.py:521`），`set_lang()` 后 `_apply_lang()` 逐控件 setText 刷新——**没有信号驱动的自动刷新**，新增控件必须记得加一行刷新代码，这是典型漏翻来源（grep 一下 `_apply_lang` 就知道它有多长）。
- 参数化用 `{n}` 占位 + `**fmt`（`"启用深度剥离（{n} 层）"`，`esp_panel.py:475/544`）。

**对 v2**：v2 已有 i18n 覆盖率脚本（蓝图 §6），比这里强；但要保留两条 GXNU 的好习惯：
1. **key 用稳定英文标识符**（`iso_peel` 而不是中文串），便于 grep 与覆盖率统计；
2. **带参数的消息也进字典**（不是只在代码里拼字符串），否则中英切换后语序错。

## B.9 线程模型

`workers.py`（155 行）是整个并发层的全部：

```python
class CubeWorker(QThread):      # :14
    def __init__(self, files, out_dir, orbitals, iso, grid, style_name, ...)
    def stop(self)              # :36  置标志位
    def run(self)               # :39  逐文件调 Multiwfn，emit progress/finished/error

class RenderWorker(QThread):    # :134
    def __init__(self, port, render_dir, output_png, tachyon_exe, resolution, ...)
    def run(self)               # :153 调 render_current_view（Tachyon 出图）
```

特征：

- **QThread 子类化 + `pyqtSignal`**（`finished(object)` / `error(str)` / `progress(str)`），从工作线程 emit，主线程槽函数更新 UI——Qt 的标准做法，跨线程安全靠 Qt 的队列连接。
- 取消是**协作式**：`stop()` 置 `_stop` 标志，`run()` 的循环里检查；但 `subprocess.run(..., timeout=600)` 是**阻塞**的，stop 只能在两次子进程之间生效（`esp_panel.EspWorker.kill()` 同理，:222）。也就是说**一次 Multiwfn 跑 3 分钟时，用户点"停止"要等它跑完**。
- 没有任务队列：每个面板自己 `_start_worker`，多个面板可以同时跑 Multiwfn（争 CPU、争工作目录），无全局并发上限。
- GL 资源与线程：cube 解析在 worker 里，但 `GlMesh.upload` 必须回主线程（Qt 的 GL 上下文绑定线程），所以 worker 只返回数据，上传在主线程——这一点做对了。

**对 v2**：v2 的 `engine/async/surface_jobs`（单工作线程 FIFO + 指纹去重 + 主线程 consume，蓝图 §4）比这里严格；但 GXNU 暴露了两个 v2 也要处理的问题：
1. **不可中断的长任务**：v2 的表面任务应支持"取消标志 + 分段检查点"（MC 可以按 z 切片检查），并把"已用时间/预计剩余"上报；
2. **进度语义**：`progress(str)` 只有文字，v2 应有 `(stage, current, total)` 三元组，才能画出确定进度条。

## B.10 单体 god object：可维护性问题的具体证据

### B.10.1 规模

| 证据 | 数字 | 后果 |
|---|---|---|
| `OrbitalVisApp` 单类 | **3253 行 / 110 个方法**（`main_window.py:330–3583`） | 一个类同时是：窗口布局、7 个 tab 的构建、Multiwfn 调用、Tcl 脚本生成、VMD socket 通信、GL 画布同步、样式管理、颜色选择、快捷键、拖放、日志、进度、i18n 刷新、路径配置、规则对话框 |
| 同名的上一代类 | `orbital_viewer_v53.py` 里还有一个 `OrbitalVisApp`（2449 行 / 96 方法） | **两代主窗口并存于源码树**，修 bug 要改两处或悄悄只改一处（典型"复制-分叉"债务） |
| `_setup_ui` 系列 | `_setup_ui`(:420) 之后紧跟 `_build_input_panel`(:700)、`_build_orbital_panel`(:737)、`_build_render_params_panel`(:780)、`_build_buttons_panel`(:890)、`_build_live_panel`(:971)、`_build_draw_bond_panel`(:1016) | 布局代码约 700 行写在一个类里，控件全是 `self.xxx` 字段（**参数状态 = 控件状态**） |
| 参数读取 | `_get_params() const`(:2265) 从几十个控件逐个 `.text()/.value()/.isChecked()` 取 | 没有参数结构体/数据模型，**"参数"没有单一真相**；任何加参数都要改 3 处（建控件、读参数、apply） |
| 三个渲染路径的手工同步 | `_sync_canvas_to_vmd`(:1933)、`_refresh_vmd_scene`(:2129)、`_push_cubes_to_canvas`(:2778)、`_send_vmd_cmd`(:3358)、`_sync_dash_to_canvas`(:3521) | **N 条路径 = N(N-1) 条同步边**，每加一个渲染后端成本平方增长 |
| 面板本身也是 god object | `EspPanel` 1146 行 / 57 方法；`AreaChartDialog` 584 行 | 一个面板管：6 个 GroupBox、worker 生命周期、cube 目录扫描、VMD 命令名映射、单位换算、色标、面积图、CSV 导出 |

### B.10.2 具体症状（可复用的"坏味道"清单）

1. **状态散落**：iso 值同时存在于滑杆（`sld_iso`）、编辑框、canvas 内部、VMD 的 `volume` 命令里；改一处要 `_apply_iso_change`(:3622) 统一广播，漏一个就不同步。
2. **同名概念多份实现**：`extract_esp_surface` 在 `esp_viewer.py`，`IGMHCompositeField` 在 `igmh_panel.py`，`orbital_gl_widget` 里又有一份 cube 采样——**"cube 采样"写了三遍**（`_grid_index`/`sample_volume`/`_trilinear` 各处一份）。
3. **对话框与主窗口耦合**：`AreaChartDialog` 需要主窗口的 cube 路径与单位，构造函数收一堆回调/lambda。
4. **异常处理靠 `traceback.print_exc()` + 日志框**：`_append_log`(:1365) 把 traceback 打到 UI，没有错误分类（用户可见错误 vs 开发者诊断）。
5. **魔法数字散落**：`0.15`（EXT 的 iso）、`627.509`、`2000x1500`（渲染分辨率）、`620`（splitter 初值）都在调用点。
6. **i18n 刷新靠手写**：`_apply_lang_ui`(:1197) 约 95 行逐控件赋值。

### B.10.3 v2 已经规避的部分（值得确认保持）

- v2 有 `IInspectorModule` 契约（`src/app/workbench/module.h:15`：`id()/label()/subtitle()/render()/onActivate()`）→ **一个模块一个类**，天然拆掉了 3000 行主窗口。
- v2 有 8 个控制器（蓝图 §2）→ 参数修改走控制器，不直接读控件。
- v2 只有一个渲染器 → 没有"多路径同步"问题。
- v2 的参数在 `Representation` / `RenderParameter` 结构体里（`representation.h:40`）→ 有单一真相。

### B.10.4 但 v2 有同样风险的两处（提前预防）

1. `modules.cpp` 已经 900+ 行、9 个模块类挤在一个文件里（`id()/label()/render()` 交错）。**建议按模块拆文件**（`modules/scene_module.cpp` 等），否则半年后就是 GXNU 的 `main_window.py`。
2. `workbench.cpp` 若继续承载布局 + 主题 + 持久化 + 快捷键，会变成新的 god object。建议把"布局常量"抽成 `WorkbenchLayout` 结构体（左栏宽/右栏宽/条高），主题在 `UITheme`，快捷键在 `Shortcuts`。

---

# C. Multiwfn / 同类工具的 UI 范式

## C.0 证据说明（重要）

**本会话未收到用户提供的 Multiwfn 截图**（截图在 lead 侧，我这边不可见）。因此本节不描述"截图里长什么样"，而是用三条**可核对**的来源重建范式：

1. 【源码】GXNU 留给我们的 Multiwfn 交互痕迹——它把 Multiwfn 当子进程喂 stdin，`esp_panel.py:63–68`、`fchk_orbital.py:2654–2657` 的命令序列就是 Multiwfn 菜单路径（`5→4→orbital→grid→2→0→q` 这种逐级编号）。这**证明** Multiwfn 的交互模型是"编号菜单 + 顺序输入"，不是表单。
2. 【未验证-公开资料】Multiwfn 手册（[Manual_Multiwfn.pdf](https://docs.hpc.taltech.ee/chemistry/Manual_Multiwfn.pdf)、[3.8_dev 手册](https://mgcf.cchem.berkeley.edu/mgcf/Multiwfn_3.8_dev.pdf)）与检索片段所描述的：主功能上百项、编号进入子功能、参数以文字提示逐项询问、图形输出在独立窗口、结果以文本报告 + 导出文件为主。其中手册自述"Since Multiwfn has too many functions, some of the functions are relatively 'small' compared to main functions"——**功能太多导致层级被迫扁平化**，这正是我们要避免的。
3. 【源码】`docs/_internal/blueprint.md` §3.2 已把"Multiwfn 式固定工作台"落成像素规格（左导航 92px / 右检查器 340px / 底快捷条 46px / 状态栏 26px），本节是在其基础上给**ImGui 落地细节**。

> 若用户截图与本节描述冲突，以截图为准；请 lead 在最终定稿时对照截图核对 C.2–C.5 的分组命名。

## C.1 Multiwfn 范式的四个特征（值得学的部分）

### C.1.1 功能导航：编号 + 一层分组，不做深树

- Multiwfn 的"导航"就是一份**连续编号的清单**（0..100+），用户输入编号进入；子功能再编号一层。没有三级菜单、没有折叠树。
- **优点**：可脚本化（编号即 API）、可口头交流（"跑 12→3 做表面分析"）、可全文检索（手册与程序编号一致）。
- **缺点**：编号无语义（用户记不住）、功能增多必然扁平成巨表、同一功能在不同上下文重复出现（如 5 既是"输出格点数据"又在多处作为前置）。

**v2 的正确取舍**：保留"一层分组 + 组内平铺"，但**用语义名而非编号**（蓝图 §3.3 的 9 个模块 id：`scene/visualize/surface/esp/orbital/trajectory/analysis/log/settings`），同时在**每个参数旁显示它的稳定 key**（如 `surface.marchingCubes.iso`），既保住可交流性又不靠记数字。

### C.1.2 参数面板：分层是"全局 → 体系 → 对象 → 参数"

Multiwfn 的文字提示顺序隐含了一个层级，GXNU 把它翻译成了 GroupBox 顺序（`esp_panel.py:_build_ui` :646 起，约 240 行）：

```
模式（ISO / PT / EXT / ALL）         ← 全局：决定后面显示哪些参数
文件 / cube 目录                     ← 输入
等值面：iso、vmin/vmax、单位、cmap、opacity、深度剥离、材质   ← 对象
原子/键：样式、原子缩放、键缩放、细线                          ← 对象（外观）
极值点：球/标签/描边/字号/半径/距离                            ← 子对象（EXT 专属）
导出：透明背景、DPI、导出 PNG                                  ← 输出
状态：日志框、进度条                                            ← 反馈
```

**这就是"参数面板的信息层级"标准答案**：`模式 → 输入 → 对象属性 → 子对象属性 → 输出 → 反馈`。v2 的 `IInspectorModule::render` 应按这个顺序排布，且**模式相关的参数要就地灰化而不是隐藏**（隐藏会让人以为功能没了；灰化告诉用户"先切模式"）。GXNU 用的是隐藏（`_build_ui` 里按 mode 调 `setVisible`），这是可改进点。

### C.1.3 快捷条：把"常用预设"提到一级

Multiwfn 没有样式快捷条（它是控制台程序），但**"预设"概念在化学软件里普遍存在**：IboView 的 6 套配色、HoukMol 的论文风、GXNU 的 `STYLES` 字典（24 个样式键）。GXNU 的做法是下拉框（`fchk_orbital.py:3276` `style_display = [f"{n} ({desc})" for ...]`）——**一个下拉塞 24 项，用户无法快速比较**。

**v2 的改进（蓝图已定）**：底部快捷条放 **4 个一键按钮**（sob-art / IBOview / HoukMol / IQmol），其余样式进"更多"弹出层。4 个按钮的理由：
- 覆盖 90% 场景（论文图 / 发表级 / 白底插图 / 冷调）；
- 每个按钮 96×30，四个加分隔正好 46px 高的条；
- 一键切换**不改变相机与选择**，只改样式参数（原子缩放/键缩放/相位色/不透明度/背景/光照）——这是 GXNU 的 `_on_style_changed`(:3241) 已经在做的事，v2 应明确"样式 = 参数快照"。

### C.1.4 状态反馈：三段式

Multiwfn（文字）：`提示 → 结果摘要 → 写入文件路径`。
GXNU（图形）：`_set_status(msg)` 单行 + `_append_log(msg)` 日志框 + 进度条（`_set_progress` :1436）。

**v2 应做成三处同时可见**：
1. 视口右下角：轻量 toast / 进度环（"生成表面 63%"）；
2. 状态栏：常驻指标（FPS · 原子数 · 三角面 · 后端）；
3. 日志模块：完整历史（可过滤、可复制）。

GXNU 的教训是**只做了 2 和 3 的一半**（进度条与日志都在面板底部，切到别的 tab 就看不见了）——v2 的进度必须在**视口内也可见**（蓝图 §5.4 已要求"进度在视口与状态栏可见"）。

## C.2 落到 ImGui 的具体布局建议（1280×800 基线）

### C.2.1 区域与尺寸

| 区域 | 高度/宽度 | 要点 | 依据 |
|---|---|---|---|
| 自绘标题栏 | 36 px | 拖拽区 + 三个窗口按钮；双击最大化 | 蓝图 §3.2 |
| 顶栏第二行（菜单） | 40 px | `文件/编辑/视图/分析/帮助` + 语言切换 + 齿轮；用 `ImGui::BeginMenuBar` 或自绘 | 同上 |
| 文件栏 | 44 px | 只读路径框（`InputText` with `ImGuiInputTextFlags_ReadOnly`）+ 打开/保存/导出 + 最近文件 `BeginPopup` | 同上 |
| 左导航 | **92 px** | 9 项，每项 **64 px 高**（图标 22px + 标签 11px），项间距 6 px | 9×64+8×6+上下 padding 12 ≈ 640，800 高够用 |
| 中央视口 | 自适应 | 圆角卡片；左上浮动工具条（样式/取景/截图）、右下色标 | 蓝图 §3.2 |
| 右检查器 | **340 px**（可拖 300–420） | 卡片分区，可滚动；分割条热区 6 px | 同上 |
| 底部快捷条 | **46 px** | 4 个样式按钮（96×30）+ 分隔符 + 清空/隐藏氢/背景/导出/重置 | 同上 |
| 状态栏 | **26 px** | 左：文档名/后端；右：FPS · 原子 · 三角面 · 帧 | 同上 |

### C.2.2 控件选型与像素细节

| 参数类型 | ImGui 控件 | 尺寸/标志 | 说明 |
|---|---|---|---|
| 标量参数（iso、opacity、间距） | `DragFloat` + `InputFloat` 联动 | 标签固定宽 **96 px**，滑杆占剩余，数值框 **72 px** | `PushItemWidth(-96.0f)`；`ImGuiSliderFlags_AlwaysClamp`；双击数值框进入输入模式 |
| 对数/跨数量级参数（iso 0.0001–0.5） | `DragFloat` + `ImGuiSliderFlags_Logarithmic` | 同上 | 否则低值区间不可调（GXNU 用 QSlider 0–100 映射，失真） |
| 区间（ESP vmin/vmax） | 同一行两个 `DragFloat`（宽 45%/45%）+ 中间"⟷" | 行高 22 px | 加"自动范围"小按钮，点后显示探测值（C.1.2 的灰化原则） |
| 枚举（cmap、单位、模式） | `BeginCombo`（不是 `ListBox`） | 宽 140 px | 选项前加 12×12 色块预览（色标尤其需要） |
| 布尔 | `Checkbox` + 右侧 `HelpMarker("…")` | 行高 20 px | 释义放 tooltip，不占版面 |
| 互斥模式（ISO/PT/EXT/ALL） | 4 个 `RadioButton` 一行，或 4 段 Segmented | 段高 24 px | 切换后**灰化不适用项**，不要隐藏 |
| 文件路径 | `InputText`(ReadOnly) + `Button("…")` | 按钮 28×22 | 拖放支持单独做（`IsWindowHovered` + `AcceptDragDropPayload`） |
| 长列表（MO 列表、表示列表） | `BeginTable` + 可排序表头 | 行高 20 px，斑马纹 | 轨道列表要 3 列：编号 / 能量 / 占据（GXNU 的 `_fill_orbital_table` 就是这个结构） |
| 色标 | `ImDrawList::AddRectFilledMultiColor` 竖条 | 宽 16 × 高 120，5 个刻度 | 画在视口内（右下 16 px 边距），不要放检查器——用户看图时要能对照 |
| 进度 | 视口内 `ProgressBar` 圆角小条 或 环形 | 宽 180 × 高 18 | 必须能在任何模块页可见 |
| 卡片/分区 | `CollapsingHeader`（自绘背景圆角 8 px） | 标题高 28 px，内边距 12 px | 最多两级：`CollapsingHeader` → 参数行；**禁止三级**（Multiwfn 的教训） |

### C.2.3 圆角与描边（ImGui 实现要点）

- `ImGui::GetStyle().WindowRounding/ChildRounding/FrameRounding/PopupRounding = 8`，`GrabRounding = 6`；卡片用 `ChildRounding = 10`。
- `FramePadding = (8,5)`，`ItemSpacing = (8,6)`，`WindowPadding = (12,10)`。
- 描边 `#DCE3EC` 在浅色主题下需要自定义 `ImDrawList` 绘制分隔线（ImGui 没有"边框颜色"样式项），用 `AddLine(p_min, p_max, IM_COL32(...), 1.0f)`。
- 深色主题切换只换调色板常量，不改布局（蓝图 §3.1 已定两套配色）。

### C.2.4 交互与"参数即快照"

- 所有参数行都应显示**是否偏离默认**：偏离时标题右侧一个 4 px 琥珀色圆点（`#F59E0B`），并提供"恢复默认"右键菜单——这是 Multiwfn 类工具最缺的东西（改了 20 个参数后不知道哪些改过）。
- **样式按钮 = 参数快照**：点击后把一组参数（原子/键缩放、相位色、不透明度、背景、光照、材质）一次性写入当前 Representation/渲染参数，并记为"预设名"显示在快捷条上；手动改任一参数则显示"自定义"。
- 快捷键 `1–4` 直接对应四个样式按钮；`F` 取景、`R` 重置（蓝图 §3.4）。

## C.3 GXNU 面板层级 vs 建议层级的对照

| 层 | GXNU 实际 | 问题 | v2 建议 |
|---|---|---|---|
| 一级 | `QTabWidget` 7 个 tab（setup/style/charge/bond_order/nbo/esp/igmh） | tab 名混了"流程"与"功能"（setup 是流程，esp 是功能）；7 个 tab 横向排列在 340px 内会截断 | 左导航 9 模块（功能），流程放顶部文件栏与视口提示 |
| 二级 | `QGroupBox`（QSS 里是白色卡片 + `#1565C0` 标题标签） | GroupBox 标题是"实心色块"，信息密度高但每个占 16px margin | `CollapsingHeader` + 12px 内边距；标题左对齐 + 右侧状态点 |
| 三级 | 参数行（label + 控件 + 小标签显示数值） | `sld_row()` 里 label 与数值标签重复显示（滑杆下方又一行） | 一行内完成：label(96) + slider + 数值框(72)；数值框可编辑 |
| 反馈 | 面板底部 `QTextEdit` 日志 + 进度条 | 切 tab 后不可见；日志框吃掉 120px 高度 | 日志进 log 模块；面板只留一行状态 + 视口内进度 |

## C.4 从 GXNU QSS 可复用的配色（已核对的十六进制）

`theme.py` 的 `LIGHT_QSS`（429 行）给出了"浅色科技风"的完整取值，v2 的浅色主题可直接对齐（蓝图 §3.1 的 Aurora Lab 略偏中性，可二选一或取并集）：

| 用途 | GXNU 值 | v2 蓝图值 | 建议 |
|---|---|---|---|
| 窗口背景 | `#E4EAF2` | `#EEF2F7` | 用 v2 值（更亮） |
| 卡片背景 | `#FFFFFF` | `#FFFFFF` | 一致 |
| 描边 | `#CBD5E1` | `#DCE3EC` | 用 v2（更淡） |
| 正文 | `#2C3E50` | `#1F2937` | 用 v2 |
| 次要文字 | `#4A5568` | `#64748B` | 用 v2 |
| 卡片标题标签 | `#1565C0`（白字实心） | `#2563EB` 主色 | 用 v2 的蓝色，但保留"标题带底色"的做法（辨识度高） |
| 字体 | `Microsoft YaHei 9.5pt` | OPPO_B.ttf + 16/20/13 | 用 v2 |
| 圆角 | 8 px（GroupBox） | 10 px 卡片 / 8 px 控件 | 一致 |

---

# D. 对照结论

## D.1 能力对照表

"v2 现状"列只写**已在 `src/` 源码核实**的事实；蓝图声明但未核实的用"〔蓝图〕"标注。

| 能力 | VMD 1.9.4 | Multiwfn | GXNU-MolStudio | MolecuLens v2 现状/规划 |
|---|---|---|---|---|
| 分子数据模型 | `Timestep` 裸指针 + 每帧深拷贝；`QMData` 挂波函数 | 无对象模型（文件进、文件出） | 无统一模型：`_load_molecule` 读 fchk/xyz 成列表，坐标/键/电荷分散在各 panel | `entity` 层 + `CoordsSoA` + `DIRTY_*` 位段【源码：`src/entity`】；比三者都正规 |
| 轨迹 | 完整动画 + 帧增删复制 | 无 | 无（单帧） | `trajectory` 模块【源码：`modules.cpp:748` id="trajectory"】 |
| 表示系统 | `DrawMolItem` = `AtomRep` + `AtomSel` + 表面生成器 | 无 | 无（样式是全局开关） | `Representation{type, selection, colorScheme, radiusScale, bondScale, visible, systemSurface}`【源码：`representation.h:40–54`】 |
| 表示类型数 | 约 15（CPK/Licorice/QuickSurf/NewRibbons/…） | — | 3（CPK/球棍/线框，经 VMD Tcl） | **13**（Wireframe…EspSurface，`representation.h:17–32`） |
| 配色方案 | 按 resname/name/index/colorid + `ColorScale` 连续映射 | 无 | colormap 字符串（matplotlib 名 → VMD 名映射） | **5**（ElementCPK/Chain/Residue/BFactor/Uniform，`:34–37`） |
| QuickSurf | 参数化（quality/radscale/gridspacing/isovalue），CUDA+CPU | 无（体数据靠格点文件） | 表面靠 Multiwfn cube → MC | `QuickSurf` 表示已存在，参数注释对齐 VMD（`:25`） |
| 分子表面（vdW/密度） | QuickSurf / MSMS / NanoShaper | 12 号功能出 `.cub` + `surfanalysis.pdb` | Multiwfn cube iso=0.001 | `Surface` + `EspSurface`；async 表面任务【蓝图】 |
| ESP 映射表面 | 只能靠 `VolumetricData` 纹理或外部着色 | 只出数据（`totesp.cub`） | **完整**：密度定几何 + ESP 逐顶点着色 + BWR 色标 + 极值标注 + 面积分布图 | `EspSurface`（`:30`，注释"密度等值面 + 静电势逐顶点着色"）；极值点/面积分布〔未实现，蓝图路线图〕 |
| 轨道等值面 | `Orbital` rep + `QMData::Wavefunction` | 5→4 出 `.cub` | **完整**：± 相位双色 + 相对 iso（IboView 语义）+ 轮廓线 + 相位翻转 | `orbital` 模块 id 存在（`modules.cpp:656`）；波函数对象〔未见，现为 cube 驱动〕 |
| 透明正确性 | 两阶段 pass，**无深度排序**（源码注释自认） | — | **深度剥离 4 层**（GLSL 三件套 + ping-pong FBO） | 两阶段 + 〔蓝图〕WBOIT 规划；**建议保留 peeling 作为精确档** |
| 一键样式 | 无（靠 `.vmd` 脚本） | 无 | **24 个样式键 + 4 个 JSON 预设**（sob-art/HoukMol/IQmol/gxnu） | 底快捷条 4 按钮〔蓝图〕；**参数值应照抄 GXNU** |
| 色标 | `ColorScale{min,mid,max,name[32]}` | 文本报告 + 导出 | matplotlib colormap + BGR 自定义 + `_vmd_cmap_name` 翻译 | 需自建色标表（见 D.2 建议 5） |
| 分析能力 | 有限（RMSD/氢键/`measure`） | **最强**（上百项） | 电荷/键级/NBO/IGMH/IRI/ESP 面积 | `analysis`/`esp` 模块骨架；IGMH/NBO〔路线图〕 |
| 离线出图 | **22 个文件后端** + Tachyon/OSPRay 光追 | 无 | 外部 VMD+Tachyon，固定 2000×1500 | Vulkan 离屏 FBO + PNG〔蓝图〕；无光追 |
| UI 形态 | Tk 控制台 + 若干窗口（非工作台） | 控制台编号菜单 + 独立图形窗 | 单窗口 7 tab + 上下/左右 splitter，无左导航 | **固定工作台**：左导航 92 / 右检查器 340 / 底条 46 / 状态栏 26〔蓝图〕；9 模块契约已落地（`module.h:15`） |
| 命令/脚本化 | **命令队列**（Tk/GUI/脚本同源）+ Tcl socket | 纯 stdin 序列 | 生成 Tcl 脚本 + socket 全量重放 | 控制器 + 事件总线（**通知式**，非命令式） |
| 线程模型 | 多进程渲染进程（共享内存） | 单线程 | QThread + 协作式取消（长任务不可中断） | 单工作线程 FIFO + 指纹去重【蓝图 §4】 |
| i18n | 无（仅英文） | 无（英文） | `TR{key:{zh,en}}` 约 283 键，手写 refresh | i18n + 覆盖率脚本【蓝图 §6】 |
| 可维护性 | 模块化良好，接口过宽 | 单体但线性 | **单体 god object（3253 行/110 方法）+ 双代码并存** | 模块化契约已定；风险在 `modules.cpp` 单文件 900+ 行 |

## D.2 对 MolecuLens v2 的具体建议

每条给出**依据**（指向本文件前述证据或源码位置），便于评审时反驳。

### 架构（A 组）

**1. 把"修改"建模成命令对象，而不是只发通知。**
现状：v2 是"控制器改状态 → 发 `RepresentationChanged`/`SceneChanged`"。建议：控制器产出 `Command`（可入队、可在帧边界执行、可序列化），事件总线只负责"已执行"通知。
依据：VMD `CommandQueue.h:51–64` + 头注释"one global instance in VMDApp"——Tk 控制台、GUI、脚本三入口共用一个执行路径，这是它 20 年不乱的根因；v2 当前的 .mlscene 命令流（`cmd.h`）已经是命令式的，缺的只是"运行时也走这条路径"。

**2. 引入"体系级波函数对象"，不要让 cube 文件成为唯一入口。**
现状：`EspSurface`/`orbital` 以 cube 路径为输入。建议：`Wavefunction{orbIds, energies, occupancies, coefficients}` 挂在 `SceneStructure` 上，cube 只是它的一个缓存/导出格式；iso 变化时不重建密度网格。
依据：VMD `QMData.h` 的 `Wavefunction` + `orb_id2index/orb_sort_map`（解决 HOMO-2 ↔ 内部下标）；反例是 GXNU 用字符串 `"ha-1"` 直接拼进 Multiwfn stdin（`fchk_orbital.py:2654`）。另：VMD 的密度预乘 `1/isovalue`（`QuickSurf.C:214`）导致改 iso 必须重跑整个 `calc_surf` —— v2 必须避免。

**3. 模块拆分到文件级，契约保持 `IInspectorModule`。**
现状：`src/app/workbench/modules.cpp` 单文件承载 9 个模块类（`id()/label()/render()` 交错，已 900+ 行）。建议：每模块一个 `.cpp/.h`，`modules.cpp` 只做注册表。
依据：GXNU `main_window.py` 的 `OrbitalVisApp` 3253 行 / 110 方法，且 `orbital_viewer_v53.py` 里还并存一个 2449 行同名类——**复制-分叉**是单体化的必然终点（B.10.1）。
补充：`module.h:15` 目前只有 `id/label/subtitle/render/onActivate`，**没有图标/glyph 接口**，而左导航需要图标；建议加 `virtual const char* glyph() const`（或 `ImTextureID icon()`），否则图标会硬编码在 `workbench.cpp` 的 switch 里，重新长出 god object。

### 渲染（R 组）

**4. 透明分两档：WBOIT 默认 + depth peeling 精确档（4 层）。**
依据：GXNU 的 peeling 实现完整可抄（`glsl_shaders.py:114/142/179` + `orbital_gl_widget.py:556 DEPTH_PEEL_LAYERS=4`、`:880 _ensure_fbos`、`:851 paintGL`），但代价是 N 层 = O(N) 次几何提交；VMD 则根本没有排序（`Scene.C:571` 注释自认 "needs to be changed"）。v2 用 Vulkan，WBOIT 单 pass 即可近似正确，peeling 留作"对照/论文图"档，并在 UI 上标注层数。

**5. 色标做成数据（五元组表），视口与离屏共用一份。**
建议结构：`ColorStop{pos, rgb}` + `ColorScale{name, stops[], diverging, unit, range}`；内置 BWR / BGR（IGMH）/ viridis + 相位双色（轨道）四套。
依据：GXNU 的色标是两套系统硬桥接（matplotlib colormap + `_vmd_cmap_name` 翻译到 VMD 名，`esp_panel.py:1105`），改一处要改两处；而 VMD 的 `Scene::ColorScale{min,mid,max,name[32]}` 虽简陋但"色标是场景数据"的思路对。

**6. 表面/轨道任务必须可中断 + 有阶段进度。**
建议：`SurfaceJob{stage, current, total, cancelled}`，MC 按 z 切片检查取消位；取消后保留已完成部分或整块丢弃（明确语义）。
依据：GXNU 的 `EspWorker.kill()`/`CubeWorker.stop()` 只置标志，而 `subprocess.run(timeout=600)` 阻塞——**用户点停止要等 3 分钟**（B.9）；v2 无子进程但 MC 同样可能跑 10 秒以上，必须有检查点。

**7. QuickSurf 增加"质量"语义层，参数向下映射。**
建议：UI 给 低/中/高/极高 → gausslim 2.0/2.5/3.0/4.0（照抄 `QuickSurf.C:1515–1525`），并据此推导 gridspacing 上界；`radscale/gridspacing/isovalue` 仍可展开手调（高级）。
依据：VMD 映射表已给出经验值；GXNU/Multiwfn 都把底层网格参数直接摊给用户，导致"调不出来"。

### UI（U 组）

**8. 一键样式的四组数值照抄 GXNU，不要重调。**
具体：`atom_scale=1.68 / bond_scale=2.0`、`orb_opacity=0.3`（HoukMol/IQmol）/`0.75`（sob-art）、`phase_pos=[0.667,1.0,0.498]`+`phase_neg=[0,0.667,1.0]`（HoukMol）、`[0.05,0.35,0.80]`+`[0.90,0.25,0.25]`（IQmol）、`atom_outline=[on,[0,0,0],0.35]`、三点光 `light_glow=0.51~1.25`、白底 `bg=[1,1,1,1]`。
依据：B.4 的实测参数表（`fchk_orbital.py` STYLES + 4 份 JSON）。这些是"看起来对"的经验值，重调一遍至少浪费两天。
注意：`atom_cpk="0.6 0.4 30 30"` 是 VMD `mol modstyle CPK` 的四参数语义，**不要直接搬进 v2 的 CPK 实现**（球/键半径系数可参考 0.6/0.4 比例，30/30 是细分级别）。

**9. 参数行统一为"96 px 标签 + 滑杆 + 72 px 数值框"，并显示偏离默认。**
依据：GXNU 的 `sld_row()` 把数值重复显示在滑杆下方（浪费一行），且改过 20 个参数后无法知道哪些非默认（C.3）；Multiwfn 靠文字回显，同样无状态可视化。

**10. 进度/状态三处可见：视口内 + 状态栏 + 日志模块。**
依据：GXNU 的进度条与日志都在 panel 底部，切 tab 即不可见（`_set_progress` `main_window.py:1436`）；Multiwfn 靠控制台文字流，用户无法并行操作。v2 的 9 模块切换更频繁，问题会被放大。

## D.3 三条"不要做"（负面清单）

1. **不要建通用场景图。** VMD 的 `Displayable` 树是为多分子/PBC/实例化准备的，代价是 `do_color_changed` 之类的横切钩子渗透到每个节点。v2 的"实例 + 表示"两层扁平结构更小更快，且已被 `representation.h` 定型。
2. **不要为每种导出格式写一个后端类。** VMD 22 个 FileRenderer 子类各重写 20+ 原语，维护成本极高；v2 应走 glTF/PLY/PNG 少量格式 + 一个中间表示。
3. **不要让宏或构建选项改变渲染语义。** VMD 的 `VMDENABLEORBITALGRIDBACKDOOR`（`DrawMolecule.h`）以 `#if` 塞入轨道网格缓存，同一份源码两种行为，注释自称 hack。

---

# E. 未验证清单与后续核对项

| # | 项目 | 状态 | 建议核对方式 |
|---|---|---|---|
| 1 | Multiwfn 界面截图内容（C 节的分组命名、快捷条形态） | **未验证**：本会话未收到截图 | lead 对照截图核对 C.1–C.4；若冲突以截图为准 |
| 2 | Multiwfn 主功能编号语义（`5`=输出格点数据、`12`=定量分子表面分析等） | 字面值来自源码【源码】，编号含义**未对手册逐条核对**【未验证】 | 查 Multiwfn 手册对应章节 |
| 3 | GXNU 是否在 `_detect_esp_range` 后把自动范围显示给用户 | 未逐行验证 `_detect_esp_range`(:1345) 与 UI 的联动 | 读 `esp_panel.py:1345–1395` |
| 4 | v2 波函数/cube 是否已有实体类（`src/io` 里可能有 cube 读取） | 未核对 `src/io`、`src/engine` 全部文件 | grep `class .*Cube|Wavefunction|Orbital` |
| 5 | v2 `app/controller` 目录是否已存在控制器实现 | `Get-ChildItem` 无输出（空目录或不存在） | `dir src\app\controller` |
| 6 | v2 `module.h` 是否已有图标接口 | 已核对：**没有**（只有 id/label/subtitle/render/onActivate） | 已在 D.2 建议 3 中提出 |
| 7 | GXNU `ovcanvas/_glwidget.py`（218 KB）与 `orbital_gl_widget.py`（1315 行）的关系 | 未读，仅知体量 | 若需"另一套 GL 实现"细节再读 |
| 8 | 深度剥离层数默认值是否可在 UI 改 | 已核对：`esp_panel` 固定 4 层、`orbital_gl_viewer.py` 可下拉选 | — |

## 附：本文件引用的源码位置索引

**VMD**（前缀 `H:\04_代码_项目\c_study\vmd-1.9.4a57.src\vmd-1.9.4a57\src\`）
`DisplayDevice.h`（接口/枚举）、`FileRenderer.h`（22 个离线后端基类）、`OpenGLRenderer.h`、`Scene.h` / `Scene.C:532,571`、`Displayable.h`、`VMDDisplayList.h:68,100,136`、`CommandQueue.h:23,51–64`、`DrawMolecule.h`、`DrawMolItem.h`、`Timestep.h:45–73`、`QMData.h`、`QuickSurf.h` / `QuickSurf.C:210,214,232,1166,1515–1547`、`IsoSurface.h`、`OpenGLRenderer.C:~832,3045,4501`

**GXNU**（前缀 `F:\MolecuLens\other\gxnu_ref\src\`）
`main_window.py:330–3583`、`fchk_orbital.py:136(STYLES),2620(gen_cube),2654–2657,3241`、`marching_cubes.py:42,166,259,295,329`、`esp_panel.py:61–68,646,1105,1148,1345`、`esp_viewer.py:57–91,140,154`、`glsl_shaders.py:114,142,163,179`、`orbital_gl_widget.py:556,851,880`、`igmh_panel.py:1–17,62,93,307`、`i18n.py:10–19`、`theme.py`、`workers.py:14,134`、`molcanvas.py:431`

**v2**（前缀 `F:\MolecuLens\`）
`src/engine/rep/representation.h:13–54`、`src/app/workbench/module.h:15–30`、`src/app/workbench/modules.cpp:200–945`（9 个模块 id/label）、`docs/_internal/blueprint.md`（§3.1–3.4 / §4 / §5 / §6 / §13）

---

*文档结束。共 4 章 + 附录；所有"源码"结论均可在上述位置复核，"未验证"项已在 E 节列出。*
