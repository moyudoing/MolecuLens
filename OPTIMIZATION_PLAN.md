# MolecuLens 全面优化计划

## 1. 已修复的问题 ✓
- representer.cpp 渲染模式分派表索引对齐问题（已在注释中确认修复）
- PNG 导出目录自动创建（已实现）
- 字体 Glyph Ranges 生命周期（已使用 static 修复）

## 2. 待实现的优化任务

### 任务 1: 实现可切换的 UI 主题系统
**目标**: 在现有 Tokyo Night 基础上增加多套配色方案
**实现方案**:
- 新增 `UIThemePreset` 枚举（TokyoNight, Dracula, Nord, OneDark, Solarized）
- 修改 `UITheme` 类支持主题切换
- 在设置面板添加主题选择器
- 保存主题偏好到配置文件

### 任务 2: 修复文字显示 bug
**分析**: 字体系统已经完善，包含：
- 中文 Glyph Ranges（~3500 常用字）
- 三档字号（16/20/13px）
- 多级回退机制
**需要**: 检查是否有特定字符缺失或渲染异常

### 任务 3: 修复轨迹导入功能
**当前状态**: 
- ITrajectoryReader 协议已定义
- PDB（MODEL/ENDMDL）已实现
- GRO 单帧已实现
- XTC/DCD 格式缺失
**实现方案**: 实现 XTC/DCD 读取器（可使用 libxdrfile）

### 任务 4: 修复无法使用的渲染模式
**验证**: 所有 9 种模式（Wireframe/Stick/BallAndStick/Spacefill/Cartoon/Surface/Label/Vdw/QuickSurf）
**状态**: 索引已对齐，Label 为占位符

### 任务 5: 设计并实现新的渲染模式
**参考 VMD 风格新增**:
- Licorice（甘草棍，粗棍状）
- Tube（管状，卡通简化版）
- NewCartoon（改进卡通，更平滑）
- Ribbon（丝带，backbone only）

### 任务 6: 实现原子级渲染控制
**目标**: 支持 per-atom representation
**实现方案**:
- 扩展 Representation 支持原子选择子集
- 多表示叠加渲染
- UI 工具面板支持多表示管理

### 任务 7: 提升图片导出质量
**当前参数**:
- SSAA 超采样：默认 ssaa=1（可调）
- 光线追踪：samplesPerPixel=2（可调）
- MSAA：msaaSamples=4
**优化方案**:
- 提高默认超采样倍数（2x-4x）
- 增加导出分辨率预设（4K, 8K）
- 优化抗锯齿算法

## 3. 实现优先级
1. UI 主题系统（用户体验）
2. 图片导出质量提升（实用性）
3. 新渲染模式（功能增强）
4. 原子级渲染控制（高级功能）
5. 轨迹导入修复（需要外部库）
