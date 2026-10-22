"""绘图全流程测试：数据解析 → CLI 渲染/动画/导出 → 产物校验。

这些用例走的是**真实**的 ``moleculens-cli``（Vulkan 离屏 + CPU 光追），
不是 mock：它们保证 Python 封装、CLI 参数、C++ 渲染器与示例数据在一条
链路上同时成立。没有构建 CLI 或没有 Pillow 时自动跳过（见 conftest）。

覆盖：
1. PDB → 球棍 + publication 风格 → PNG（尺寸/位深/白底/非空）
2. 透明背景 + alpha 通道；16-bit PNG 位深
3. 轨迹帧切换（同一体系不同帧的像素必须不同）
4. ESP 密度/势 cube → 彩色映射表面
5. CPU 光线追踪出图
6. 轨迹 → GIF 动画（帧数/时长）
7. 结构 → OBJ 几何导出（顶点/面/材质 sidecar）
"""

from __future__ import annotations

import struct
from pathlib import Path

import pytest

import moleculens as ml

PIL_Image = pytest.importorskip("PIL.Image")
ImageChops = pytest.importorskip("PIL.ImageChops")

REPO_ROOT = Path(__file__).resolve().parents[2]
CLI_CONFIGS = ("Release", "RelWithDebInfo", "Debug")


def _find_cli() -> Path | None:
    try:
        return ml.cli_path()
    except FileNotFoundError:
        return None


@pytest.fixture(scope="module")
def cli() -> Path:
    path = _find_cli()
    if path is None:
        pytest.skip(
            "moleculens-cli not built; build target moleculens-cli to run render tests"
        )
    return path


@pytest.fixture(scope="module")
def render_dir(tmp_path_factory: pytest.TempPathFactory) -> Path:
    return tmp_path_factory.mktemp("render_pipeline")


def _png_header(path: Path) -> tuple[int, int, int, int]:
    """返回 (width, height, bit_depth, color_type)，直接读 IHDR。"""
    data = path.read_bytes()
    assert data[:8] == b"\x89PNG\r\n\x1a\n", f"not a PNG: {path}"
    assert data[12:16] == b"IHDR"
    return struct.unpack(">IIBB", data[16:26])


def _open_rgba(path: Path):
    image = PIL_Image.open(path)
    image.load()
    return image.convert("RGBA")


def _assert_not_blank(path: Path, *, min_colors: int = 30) -> None:
    image = _open_rgba(path)
    colors = image.convert("RGB").getcolors(maxcolors=1 << 20)
    assert colors is not None, f"image has too many colors to count: {path}"
    assert len(colors) >= min_colors, f"image looks blank: {path} ({len(colors)} colors)"


def test_render_publication_png(epi_path: Path, cli: Path, render_dir: Path) -> None:
    out = render_dir / "epi_publication.png"
    result = ml.render(
        epi_path,
        out,
        rep="ballstick",
        style="publication",
        size=(320, 240),
        executable=cli,
    )
    assert result == out
    assert out.is_file() and out.stat().st_size > 1000
    assert _png_header(out) == (320, 240, 8, 6)  # 8-bit RGBA
    image = _open_rgba(out)
    assert image.size == (320, 240)
    assert len(image.convert("RGB").getcolors(maxcolors=1 << 20) or []) > 50
    # publication 预设：白底（角落为纯白）
    assert image.getpixel((0, 0)) == (255, 255, 255, 255)
    assert image.getpixel((320 - 1, 240 - 1)) == (255, 255, 255, 255)
    # 分子占据画面中心附近，不能是纯白
    assert image.getpixel((160, 120))[:3] != (255, 255, 255)


def test_render_transparent_alpha_and_16bit(
    epi_path: Path, cli: Path, render_dir: Path
) -> None:
    transparent = render_dir / "epi_transparent.png"
    ml.render(
        epi_path,
        transparent,
        rep="wireframe",
        background="transparent",
        size=(200, 150),
        executable=cli,
    )
    image = _open_rgba(transparent)
    alpha = image.getchannel("A")
    assert alpha.getextrema() == (0, 255), "透明背景应同时有全透明与不透明像素"
    assert _png_header(transparent) == (200, 150, 8, 6)

    deep = render_dir / "epi_16bit.png"
    ml.render(
        epi_path,
        deep,
        rep="ballstick",
        depth=16,
        size=(160, 120),
        executable=cli,
    )
    width, height, bit_depth, color_type = _png_header(deep)
    assert (width, height, bit_depth, color_type) == (160, 120, 16, 6)


def test_render_trajectory_frames_differ(
    md_gro_path: Path, data_dir: Path, cli: Path, render_dir: Path
) -> None:
    xtc = data_dir / "md.xtc"
    if not xtc.is_file():
        pytest.skip(f"fixture missing: {xtc}")
    first = render_dir / "traj_frame_000.png"
    later = render_dir / "traj_frame_012.png"
    kwargs = dict(
        rep="wireframe",
        trajectory=xtc,
        size=(240, 180),
        executable=cli,
    )
    ml.render(md_gro_path, first, frame=0, **kwargs)
    ml.render(md_gro_path, later, frame=12, **kwargs)

    assert _png_header(first)[:2] == (240, 180)
    assert _png_header(later)[:2] == (240, 180)
    assert first.read_bytes() != later.read_bytes(), "不同轨迹帧的像素应不同"

    a = PIL_Image.open(first).convert("RGB")
    b = PIL_Image.open(later).convert("RGB")
    assert ImageChops.difference(a, b).getbbox() is not None
    # 画面至少有明显差异：非零像素差的比例超过 1%
    pixels = list(a.getdata())
    other = list(b.getdata())
    changed = sum(1 for p, q in zip(pixels, other) if p != q)
    assert changed / len(pixels) > 0.01


def test_render_esp_surface(cli: Path, render_dir: Path) -> None:
    density = REPO_ROOT / "datasets" / "esp" / "glycine" / "glycine_density.cube"
    esp = REPO_ROOT / "datasets" / "esp" / "glycine" / "glycine_esp.cube"
    if not density.is_file() or not esp.is_file():
        pytest.skip("ESP cube fixtures missing")

    out = render_dir / "glycine_esp.png"
    ml.render(
        density,
        out,
        esp=esp,
        cmap="coolwarm",
        vmin=-40,
        vmax=40,
        size=(240, 180),
        executable=cli,
    )
    assert _png_header(out) == (240, 180, 8, 6)
    image = _open_rgba(out)
    colors = image.convert("RGB").getcolors(maxcolors=1 << 20)
    assert colors is not None and len(colors) >= 100, "ESP 表面应有连续色带"

    # coolwarm：应同时出现明显偏蓝与偏红的像素（不是灰阶球）
    pixels = list(image.convert("RGB").getdata())
    reds = sum(1 for r, g, b in pixels if r > b + 40)
    blues = sum(1 for r, g, b in pixels if b > r + 40)
    assert reds > 100 and blues > 100, (reds, blues)


def test_render_ray_tracing_cpu(epi_path: Path, cli: Path, render_dir: Path) -> None:
    out = render_dir / "epi_raytrace.png"
    ml.render(
        epi_path,
        out,
        rep="ballstick",
        ray_trace=1,
        rt_backend="cpu",
        size=(160, 120),
        executable=cli,
    )
    assert _png_header(out) == (160, 120, 8, 6)
    _assert_not_blank(out, min_colors=20)


def test_render_animation_gif(
    md_gro_path: Path, data_dir: Path, cli: Path, render_dir: Path
) -> None:
    xtc = data_dir / "md.xtc"
    if not xtc.is_file():
        pytest.skip(f"fixture missing: {xtc}")

    out = render_dir / "trajectory.gif"
    ml.render_animation(
        md_gro_path,
        out,
        trajectory=xtc,
        frame_range="0:4",
        fps=8,
        rep="wireframe",
        size=(160, 120),
        executable=cli,
    )
    assert out.is_file() and out.stat().st_size > 1000
    animation = PIL_Image.open(out)
    assert animation.size == (160, 120)
    assert animation.n_frames == 5  # 0:4 含端点
    duration = animation.info.get("duration", 0)
    assert 80 <= duration <= 200, f"8 fps 的帧时长应约 125 ms，实际 {duration}"


def test_convert_obj_geometry(epi_path: Path, cli: Path, render_dir: Path) -> None:
    out = render_dir / "epi.obj"
    result = ml.convert(epi_path, out, rep="ballstick", executable=cli)
    assert result == out and out.is_file()

    text = out.read_text(encoding="utf-8", errors="replace")
    assert text.startswith("# MolecuLens OBJ export")
    assert "\nv " in text
    assert "\nf " in text
    vertex_count = sum(1 for line in text.splitlines() if line.startswith("v "))
    face_count = sum(1 for line in text.splitlines() if line.startswith("f "))
    assert vertex_count > 100
    assert face_count > 50

    mtl = out.with_suffix(".mtl")
    assert mtl.is_file(), "OBJ 导出应同时写出 mtl 材质文件"
    assert "newmtl" in mtl.read_text(encoding="utf-8", errors="replace")


def test_render_errors_are_reported(cli: Path, tmp_path: Path) -> None:
    with pytest.raises(FileNotFoundError):
        ml.cli_path(tmp_path / "missing-cli.exe")

    missing_input = tmp_path / "missing.pdb"
    with pytest.raises(ml.CliError) as excinfo:
        ml.render(
            missing_input,
            tmp_path / "out.png",
            rep="ballstick",
            size=(64, 64),
            executable=cli,
        )
    error = excinfo.value
    assert error.returncode != 0
    assert error.command[0] == str(cli)
    assert "render" in error.command
    assert str(missing_input) in " ".join(error.command)
