"""MolecuLens 的 Python 接口。

这个包提供两层能力：

* **数据层**（C++ 绑定）：:func:`load` 解析 PDB / GRO / SDF / cube，
  :class:`Structure` 暴露原子、键、坐标与统计，:meth:`Structure.select`
  / :meth:`Structure.selection` 复用程序内部的选择语言；
* **绘图流程**（调用同仓库的 ``moleculens-cli``）：:func:`render` 出单张图片，
  :func:`render_animation` 导出 GIF / APNG / MP4 / WebM，:func:`convert`
  导出 OBJ / STL / POV / mlscene。

渲染不重新实现：参数名与 ``moleculens-cli render --help`` 一一对应，
GUI、CLI、Python 共用同一套 C++ 渲染路径。

示例::

    >>> import moleculens
    >>> mol = moleculens.load("data/EPI.pdb")
    >>> len(mol), mol.atom_count
    (35, 35)
    >>> mol.atoms[0].element, mol.atoms[0].residue_name
    ('O', 'EP')
    >>> mol.select("element C")
    [6, 7, ...]          # 全局原子下标

    >>> moleculens.render(
    ...     "data/EPI.pdb", "epi.png",
    ...     rep="ballstick", style="publication", size=(1280, 720),
    ... )
    PosixPath('epi.png')
"""

from __future__ import annotations

import os
from pathlib import Path
from typing import Any

from . import _native
from ._cli import CliError, cli_path, convert, render, render_animation
from ._native import (
    Atom,
    Bond,
    Element,
    MoleculensError,
    Selection,
    Structure,
    Warning,
    __version__,
    element_info,
    elements,
    supported_extensions,
)

__all__ = [
    "Atom",
    "Bond",
    "CliError",
    "Element",
    "MoleculensError",
    "Selection",
    "Structure",
    "Warning",
    "__version__",
    "cli_path",
    "convert",
    "element_info",
    "elements",
    "info",
    "load",
    "render",
    "render_animation",
    "supported_extensions",
]

def _coerce_path(path: str | bytes | os.PathLike[str] | os.PathLike[bytes]) -> Path:
    """把 str / bytes / os.PathLike 统一成 Path，并给出 Python 风格的报错。"""
    try:
        raw = os.fspath(path)
    except TypeError as exc:  # pragma: no cover - 参数类型错误
        raise TypeError(
            "path must be str, bytes or os.PathLike, "
            f"not {type(path).__name__}"
        ) from exc
    if isinstance(raw, bytes):
        raw = os.fsdecode(raw)
    return Path(raw).expanduser()


def load(
    path: str | bytes | os.PathLike[str] | os.PathLike[bytes],
    *,
    infer_bonds: bool = True,
    strict: bool = False,
) -> Structure:
    """解析结构文件，返回只读的 :class:`Structure`。

    参数
    ----
    path:
        输入文件（PDB / ENT / GRO / SDF / MOL / cube / cub）。
    infer_bonds:
        文件没有显式键表时是否按距离推断键（默认开启，与 GUI 一致）。
    strict:
        严格模式：遇到畸形行立即失败；默认宽松模式把问题收进
        ``Structure.warnings``。

    异常
    ----
    FileNotFoundError
        文件不存在。
    IsADirectoryError
        传入的是目录。
    MoleculensError
        没有可用解析器、解析失败或文件内容非法。消息前缀是错误码，
        例如 ``"FormatError: No parser found for: x.xyz"``。

    说明
    ----
    ``Structure`` 是加载时刻的**快照**，包含原子下标、坐标、残基/链名、
    键表与解析警告；它不会在 C++ 场景后续变化时自动更新。
    """
    file_path = _coerce_path(path)

    if not file_path.exists():
        raise FileNotFoundError(f"structure file not found: {file_path}")
    if not file_path.is_file():
        raise IsADirectoryError(f"expected a structure file, got directory: {file_path}")

    return _native.load(
        file_path,
        infer_bonds=bool(infer_bonds),
        strict=bool(strict),
    )


def info(
    path: str | bytes | os.PathLike[str] | os.PathLike[bytes],
    **load_kwargs: Any,
) -> dict[str, Any]:
    """返回 :meth:`Structure.summary` 的字典（便捷函数）。

    统计口径与 ``moleculens-cli info`` 一致：原子/键数按**实例**累加
    （例如 ``data/md.gro`` 是 6452 个原子，而不是首个水分子结构的 3 个）。
    """
    return load(path, **load_kwargs).summary()
