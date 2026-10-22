"""pytest 配置：定位 CMake 构建出的 moleculens 包并准备测试数据路径。

原生扩展默认在 ``<repo>/build/python/moleculens``；如果构建目录不是 ``build``，
设置环境变量 ``MOLECULENS_PYTHON_DIR`` 指向含 ``moleculens/`` 的目录即可。
"""

from __future__ import annotations

import os
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
BUILD_PYTHON = Path(
    os.environ.get("MOLECULENS_PYTHON_DIR", str(ROOT / "build" / "python"))
).resolve()

DATA_DIR = ROOT / "data"
DATASETS_DIR = ROOT / "datasets"


def _has_native_package(directory: Path) -> bool:
    package = directory / "moleculens"
    return (package / "__init__.py").is_file() and any(
        package.glob("_native*")
    )


if not _has_native_package(BUILD_PYTHON):
    raise RuntimeError(
        "moleculens native package not found at "
        f"{BUILD_PYTHON}.\n"
        "Build it first:\n"
        "  cmake -S . -B build -DMOLECULENS_BUILD_PYTHON=ON\n"
        "  cmake --build build --config Release --target _native\n"
        "or point MOLECULENS_PYTHON_DIR at the directory containing moleculens/."
    )

if str(BUILD_PYTHON) not in sys.path:
    sys.path.insert(0, str(BUILD_PYTHON))

# 运行期 DLL 的后备搜索路径（正常构建已由 CMake POST_BUILD 拷到扩展旁）。
_DLL_HANDLES = []
if os.name == "nt":
    for candidate in (
        ROOT / "vcpkg_installed" / "x64-windows" / "bin",
        ROOT / "build" / "vcpkg_installed" / "x64-windows" / "bin",
    ):
        if candidate.is_dir():
            _DLL_HANDLES.append(os.add_dll_directory(str(candidate)))


@pytest.fixture(scope="session")
def repo_root() -> Path:
    return ROOT


@pytest.fixture(scope="session")
def data_dir() -> Path:
    if not DATA_DIR.is_dir():
        pytest.skip(f"test data directory missing: {DATA_DIR}")
    return DATA_DIR


@pytest.fixture(scope="session")
def epi_path(data_dir: Path) -> Path:
    path = data_dir / "EPI.pdb"
    if not path.is_file():
        pytest.skip(f"fixture missing: {path}")
    return path


@pytest.fixture(scope="session")
def glycine_path(data_dir: Path) -> Path:
    path = data_dir / "glycine.pdb"
    if not path.is_file():
        pytest.skip(f"fixture missing: {path}")
    return path


@pytest.fixture(scope="session")
def md_gro_path(data_dir: Path) -> Path:
    path = data_dir / "md.gro"
    if not path.is_file():
        pytest.skip(f"fixture missing: {path}")
    return path
