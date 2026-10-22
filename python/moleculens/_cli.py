"""通过 ``moleculens-cli`` 完成渲染、动画与格式转换的 Python 封装。

设计上只有一份渲染实现：本模块拼装命令行参数后调用同仓库的
``moleculens-cli``（与 GUI、C++ 测试共用同一套 Vulkan/CPU 路径），
Python 侧不复制材质、相机或表面参数表。这样：

* 无图形界面的服务器可以批量出图；
* 命令行帮助（``moleculens-cli render --help``）始终是参数的唯一事实来源；
* 失败时保留完整命令行、退出码与 stderr，问题可复现。

示例::

    import moleculens
    moleculens.render("data/EPI.pdb", "epi.png",
                      rep="ballstick", style="publication", size=(1280, 720))

    moleculens.render_animation("data/md.gro", "md.gif",
                                trajectory="data/md.xtc",
                                frame_range="0:30", fps=15, size=(640, 480))
"""

from __future__ import annotations

import os
import shutil
import subprocess
from collections.abc import Sequence
from pathlib import Path

_CLI_NAMES = ("moleculens-cli.exe", "moleculens-cli")
_CONFIGS = ("Release", "RelWithDebInfo", "Debug")


class CliError(RuntimeError):
    """``moleculens-cli`` 返回非零退出码时抛出。"""

    def __init__(
        self,
        message: str,
        *,
        command: Sequence[str],
        returncode: int,
        stdout: str = "",
        stderr: str = "",
    ) -> None:
        super().__init__(message)
        self.command = tuple(command)
        self.returncode = returncode
        self.stdout = stdout
        self.stderr = stderr


def cli_path(executable: str | bytes | os.PathLike[str] | None = None) -> Path:
    """定位 ``moleculens-cli`` 可执行文件。

    查找顺序：显式 ``executable`` → 环境变量 ``MOLECULENS_CLI`` → ``PATH`` →
    仓库 ``build/src/cli/<Config>/``（Release / RelWithDebInfo / Debug）。

    找不到时抛出 :class:`FileNotFoundError`，并提示构建命令。
    """
    if executable is not None:
        path = Path(os.fsdecode(os.fspath(executable))).expanduser()
        if path.is_file():
            return path
        raise FileNotFoundError(f"moleculens-cli not found at: {path}")

    env = os.environ.get("MOLECULENS_CLI")
    if env:
        path = Path(env).expanduser()
        if path.is_file():
            return path

    on_path = shutil.which("moleculens-cli")
    if on_path:
        return Path(on_path)

    # 源码树 / 任意构建树：本文件位于 <root>/python/moleculens/ 或
    # <build>/python/moleculens/，向上逐级查找 build/src/cli 与 <build>/src/cli。
    for parent in Path(__file__).resolve().parents:
        for build in (parent / "build" / "src" / "cli", parent / "src" / "cli"):
            for config in _CONFIGS:
                for name in _CLI_NAMES:
                    candidate = build / config / name
                    if candidate.is_file():
                        return candidate
            for name in _CLI_NAMES:
                candidate = build / name
                if candidate.is_file():
                    return candidate

    raise FileNotFoundError(
        "moleculens-cli not found. Build it with:\n"
        "  cmake -B build -S .\n"
        "  cmake --build build --config Release --target moleculens-cli\n"
        "or set MOLECULENS_CLI to its full path."
    )


def _run(
    args: Sequence[str],
    *,
    executable: str | bytes | os.PathLike[str] | None,
    timeout: float,
) -> subprocess.CompletedProcess[str]:
    exe = cli_path(executable)
    command = [str(exe), *(str(arg) for arg in args)]
    try:
        completed = subprocess.run(
            command,
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
            timeout=timeout,
            check=False,
        )
    except subprocess.TimeoutExpired as exc:
        raise CliError(
            f"moleculens-cli timed out after {timeout:g}s",
            command=command,
            returncode=-1,
            stdout=exc.stdout or "",
            stderr=exc.stderr or "",
        ) from exc
    if completed.returncode != 0:
        detail = (completed.stderr or completed.stdout or "").strip()
        raise CliError(
            f"moleculens-cli failed with exit code {completed.returncode}"
            + (f": {detail}" if detail else ""),
            command=command,
            returncode=completed.returncode,
            stdout=completed.stdout,
            stderr=completed.stderr,
        )
    return completed


def _append_flag(args: list[str], flag: str, value: object) -> None:
    if value is None or value is False:
        return
    if value is True:
        args.append(flag)
        return
    args.extend([flag, str(value)])


def _append_path_flag(args: list[str], flag: str, value: object) -> None:
    if value is None or value is False:
        return
    args.extend([flag, os.fsdecode(os.fspath(value))])


def _append_rep(args: list[str], rep: object) -> None:
    if rep is None:
        return
    if isinstance(rep, str):
        # 单个表示：用 --repr，--coloring 才能作用在这套表示上。
        args.extend(["--repr", rep])
        return
    for item in rep:  # type: ignore[union-attr]
        args.extend(["--rep", str(item)])


def _append_size(args: list[str], size: object, height: object) -> None:
    if size is not None:
        if isinstance(size, int):
            args.extend(["--size", str(size)])
        else:
            width, tall = size  # type: ignore[misc]
            args.extend(["--size", str(width), "--height", str(tall)])
    if height is not None:
        args.extend(["--height", str(height)])


def _common_render_args(
    *,
    rep: object = None,
    coloring: object = None,
    material: object = None,
    style: object = None,
    size: object = None,
    height: object = None,
    background: object = None,
    camera: object = None,
    zoom: object = None,
    opacity: object = None,
    depth: object = None,
    scale: object = None,
    msaa: object = None,
    detail: object = None,
    seg: object = None,
    color: object = None,
    clip: object = None,
    clip_offset: object = None,
    light_az: object = None,
    light_el: object = None,
    surf_iso: object = None,
    surf_grid: object = None,
    system_surface: bool = False,
    trajectory: object = None,
    frame: object = None,
    esp: object = None,
    density: object = None,
    cmap: object = None,
    vmin: object = None,
    vmax: object = None,
    iso: object = None,
    esp_opacity: object = None,
    no_auto_range: bool = False,
    invert: bool = False,
    ray_trace: object = None,
    rt_backend: object = None,
) -> list[str]:
    args: list[str] = []
    _append_rep(args, rep)
    _append_flag(args, "--coloring", coloring)
    _append_flag(args, "--material", material)
    _append_flag(args, "--style", style)
    _append_size(args, size, height)
    _append_flag(args, "--bg", background)
    _append_flag(args, "--camera", camera)
    _append_flag(args, "--zoom", zoom)
    _append_flag(args, "--opacity", opacity)
    _append_flag(args, "--depth", depth)
    _append_flag(args, "--scale", scale)
    _append_flag(args, "--msaa", msaa)
    _append_flag(args, "--detail", detail)
    _append_flag(args, "--seg", seg)
    _append_flag(args, "--color", color)
    _append_flag(args, "--clip", clip)
    _append_flag(args, "--clip-offset", clip_offset)
    _append_flag(args, "--light-az", light_az)
    _append_flag(args, "--light-el", light_el)
    _append_flag(args, "--surf-iso", surf_iso)
    _append_flag(args, "--surf-grid", surf_grid)
    _append_flag(args, "--system-surface", system_surface)
    _append_path_flag(args, "--traj", trajectory)
    _append_flag(args, "--frame", frame)
    _append_path_flag(args, "--esp", esp)
    _append_path_flag(args, "--density", density)
    _append_flag(args, "--cmap", cmap)
    _append_flag(args, "--vmin", vmin)
    _append_flag(args, "--vmax", vmax)
    _append_flag(args, "--iso", iso)
    _append_flag(args, "--esp-opacity", esp_opacity)
    _append_flag(args, "--no-auto-range", no_auto_range)
    _append_flag(args, "--invert", invert)
    _append_flag(args, "--rt", ray_trace)
    _append_flag(args, "--rt-backend", rt_backend)
    return args


def render(
    input: str | bytes | os.PathLike[str],
    out: str | bytes | os.PathLike[str],
    *,
    rep: str | Sequence[str] | None = None,
    coloring: str | None = None,
    material: str | None = None,
    style: str | None = None,
    size: int | tuple[int, int] | None = None,
    height: int | None = None,
    background: str | None = None,
    camera: str | None = None,
    zoom: float | None = None,
    opacity: float | None = None,
    depth: int | None = None,
    scale: int | None = None,
    msaa: int | None = None,
    detail: int | None = None,
    seg: int | None = None,
    color: str | None = None,
    clip: str | None = None,
    clip_offset: float | None = None,
    light_az: float | None = None,
    light_el: float | None = None,
    surf_iso: float | None = None,
    surf_grid: float | None = None,
    system_surface: bool = False,
    trajectory: str | bytes | os.PathLike[str] | None = None,
    frame: int | None = None,
    esp: str | bytes | os.PathLike[str] | None = None,
    density: str | bytes | os.PathLike[str] | None = None,
    cmap: str | None = None,
    vmin: float | None = None,
    vmax: float | None = None,
    iso: float | None = None,
    esp_opacity: float | None = None,
    no_auto_range: bool = False,
    invert: bool = False,
    ray_trace: int | None = None,
    rt_backend: str | None = None,
    extra_args: Sequence[str] = (),
    executable: str | bytes | os.PathLike[str] | None = None,
    timeout: float = 300.0,
) -> Path:
    """渲染单张图片，返回输出路径。

    参数名与 ``moleculens-cli render --help`` 一一对应（连字符换成下划线）：
    ``rep`` / ``repr`` 对应 ``--repr``，序列形式的 ``rep`` 表示多套表示
    （逐项 ``--rep``）；``size`` 可传宽度整数或 ``(宽, 高)``；ESP 用
    ``esp=`` / ``density=`` / ``cmap=``。无法预见的参数可通过 ``extra_args``
    原样追加。

    失败（非零退出码或超时）抛出 :class:`CliError`，其中保留完整命令行与
    stderr，便于复现。
    """
    args = ["render", os.fsdecode(os.fspath(input))]
    args += _common_render_args(
        rep=rep,
        coloring=coloring,
        material=material,
        style=style,
        size=size,
        height=height,
        background=background,
        camera=camera,
        zoom=zoom,
        opacity=opacity,
        depth=depth,
        scale=scale,
        msaa=msaa,
        detail=detail,
        seg=seg,
        color=color,
        clip=clip,
        clip_offset=clip_offset,
        light_az=light_az,
        light_el=light_el,
        surf_iso=surf_iso,
        surf_grid=surf_grid,
        system_surface=system_surface,
        trajectory=trajectory,
        frame=frame,
        esp=esp,
        density=density,
        cmap=cmap,
        vmin=vmin,
        vmax=vmax,
        iso=iso,
        esp_opacity=esp_opacity,
        no_auto_range=no_auto_range,
        invert=invert,
        ray_trace=ray_trace,
        rt_backend=rt_backend,
    )
    args += ["--out", os.fsdecode(os.fspath(out)), *(str(a) for a in extra_args)]
    _run(args, executable=executable, timeout=timeout)
    return Path(os.fsdecode(os.fspath(out)))


def render_animation(
    input: str | bytes | os.PathLike[str],
    out: str | bytes | os.PathLike[str],
    *,
    trajectory: str | bytes | os.PathLike[str],
    fps: int = 30,
    frame_range: str | None = None,
    step: int | None = None,
    interp: int | None = None,
    rep: str | Sequence[str] | None = None,
    coloring: str | None = None,
    style: str | None = None,
    size: int | tuple[int, int] | None = None,
    background: str | None = None,
    camera: str | None = None,
    zoom: float | None = None,
    executable: str | bytes | os.PathLike[str] | None = None,
    timeout: float = 600.0,
    extra_args: Sequence[str] = (),
    **extra_render_options: object,
) -> Path:
    """导出轨迹动画（扩展名决定格式：``.gif`` / ``.apng`` / ``.mp4`` / ``.webm``）。

    ``frame_range`` 采用 CLI 的 ``a:b`` 语法（``-1`` 表示末帧）。其余渲染参数
    （材质、光照、光追等）通过 ``**extra_render_options`` 透传，名称与
    :func:`render` 相同。
    """
    args = ["render", os.fsdecode(os.fspath(input))]
    args += _common_render_args(
        rep=rep,
        coloring=coloring,
        style=style,
        size=size,
        background=background,
        camera=camera,
        zoom=zoom,
        trajectory=trajectory,
        **extra_render_options,
    )
    args += ["--anim", os.fsdecode(os.fspath(out)), "--anim-fps", str(fps)]
    if frame_range is not None:
        args += ["--anim-range", frame_range]
    if step is not None:
        args += ["--anim-step", str(step)]
    if interp is not None:
        args += ["--anim-interp", str(interp)]
    args += [str(a) for a in extra_args]
    _run(args, executable=executable, timeout=timeout)
    return Path(os.fsdecode(os.fspath(out)))


def convert(
    input: str | bytes | os.PathLike[str],
    out: str | bytes | os.PathLike[str],
    *,
    rep: str = "ballstick",
    executable: str | bytes | os.PathLike[str] | None = None,
    timeout: float = 300.0,
) -> Path:
    """把结构导出为 ``.obj`` / ``.stl`` / ``.pov`` / ``.mlscene``。

    返回输出路径；``.obj`` 会在同目录写出同名 ``.mtl``。
    """
    args = [
        "convert",
        os.fsdecode(os.fspath(input)),
        os.fsdecode(os.fspath(out)),
        "--repr",
        rep,
    ]
    _run(args, executable=executable, timeout=timeout)
    return Path(os.fsdecode(os.fspath(out)))
